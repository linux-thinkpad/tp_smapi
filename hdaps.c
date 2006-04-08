/*
 * drivers/hwmon/hdaps.c - driver for IBM's Hard Drive Active Protection System
 *
 * Copyright (C) 2005 Robert Love <rml@novell.com>
 * Copyright (C) 2005 Jesper Juhl <jesper.juhl@gmail.com>
 *
 * The HardDisk Active Protection System (hdaps) is present in IBM ThinkPads
 * starting with the R40, T41, and X40.  It provides a basic two-axis
 * accelerometer and other data, such as the device's temperature.
 *
 * This driver is based on the document by Mark A. Smith available at
 * http://www.almaden.ibm.com/cs/people/marksmith/tpaps.html and a lot of trial
 * and error.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License v2 as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/dmi.h>
#include <linux/tp_base.h>
#include <linux/jiffies.h>
#include <asm/io.h>

#define HDAPS_IDX_STATE		0x1	/* device state */
#define HDAPS_IDX_YPOS		0x2	/* y-axis position */
#define	HDAPS_IDX_XPOS		0x4	/* x-axis position */
#define HDAPS_IDX_TEMP1		0x6	/* device temperature, in celcius */
#define HDAPS_IDX_YVAR		0x7	/* y-axis variance (what is this?) */
#define HDAPS_IDX_XVAR		0x9	/* x-axis variance (what is this?) */
#define HDAPS_IDX_TEMP2		0xb	/* device temperature (again?) */
#define HDAPS_IDX_UNKNOWN	0xc	/* what is this? */
#define HDAPS_IDX_KMACT		0xd	/* keyboard or mouse activity */

#define KEYBD_MASK		0x20	/* set if keyboard activity */
#define MOUSE_MASK		0x40	/* set if mouse activity */

#define STATE_HAVE_POS          0x01    /* have position data */
#define STATE_HAVE_VAR          0x02    /* have position and variance data */

#define READ_TIMEOUT_MSECS	100	/* wait this long for device read */
#define RETRY_MSECS		3	/* retry delay */

#define HDAPS_POLL_PERIOD	(HZ/20)	/* poll for input every 1/20s */
#define HDAPS_INPUT_FUZZ	4	/* input event threshold */
#define HDAPS_INPUT_FLAT	4

#define KMACT_REMEMBER_PERIOD   (HDAPS_POLL_PERIOD*2) /* k/m persistance */

static struct timer_list hdaps_timer;
static struct platform_device *pdev;
static struct input_dev *hdaps_idev;
static unsigned int hdaps_invert;
static unsigned int hdaps_force;
static int needs_calibration = 0;

/* Latest state read */
static int pos_x, pos_y;   /* position */
static int var_x, var_y;   /* variance (what is this?) */
static int rest_x, rest_y; /* calibrated rest position */
static u8 temp1, temp2;    /* temperatures */

/* Last time we saw keyboard and mouse activity */
u64 last_keyboard_jiffies = INITIAL_JIFFIES;
u64 last_mouse_jiffies = INITIAL_JIFFIES;

/*
 * __wait_latch - Wait up to 100us for a port latch to get a certain value,
 * returning zero if the value is obtained. Callers must hold controller lock.
 */
static int __wait_latch(u16 port, u8 val)
{
	unsigned int i;

	for (i = 0; i < 200; i++) {
		if (inb(port)==val)
			return 0;
		udelay(5);
	}

	return -EIO;
}


/* hdaps_read_row - read a row of data from the controller.
 * Also prefetches the next read, to reduce udelay busy-waiting.
 * If fast, do one quick attempt without retries.
 * Caller must hold controller lock.
 */
static int hdaps_read_row(int fast, u8* row) {
	int ret;
	if (fast)
		ret = tp_controller_try_read_row(0x11, 0x01, row);
	else
		ret = tp_controller_read_row(0x11, 0x01, row);
	tp_controller_prefetch_row(0x11, 0x01);
	return ret;
}

/* __hdaps_update - read current state and update global state variables.
 * Caller must hold controller lock. 
 */
static int __hdaps_update(int fast)
{
	u8 row[TP_CONTROLLER_ROW_LEN];
	int ret;

	ret = hdaps_read_row(fast, row);
	if (ret)
		return ret;

	if (row[HDAPS_IDX_STATE]>=STATE_HAVE_POS) {
		pos_x = *(s16*)(row+HDAPS_IDX_XPOS) * (hdaps_invert?-1:1);
		pos_y = *(s16*)(row+HDAPS_IDX_YPOS) * (hdaps_invert?-1:1);
	} else
		return -EBUSY;

	/* Don't insist on a "variance" readout; it's useless anyway. */
	if (row[HDAPS_IDX_STATE]>=STATE_HAVE_VAR) {
		var_x = *(s16*)(row+HDAPS_IDX_XVAR) * (hdaps_invert?-1:1);
		var_y = *(s16*)(row+HDAPS_IDX_YVAR) * (hdaps_invert?-1:1);
	}

	/* Keyboard and mouse activity status is cleared as soon as it's read,
	 * so applications will eat each other's events. Thus we remember any
	 * event for KMACT_REMEMBER_PERIOD jiffies.
	 */
	if (row[HDAPS_IDX_KMACT] & KEYBD_MASK)
		last_keyboard_jiffies = get_jiffies_64();
	if (row[HDAPS_IDX_KMACT] & MOUSE_MASK)
		last_mouse_jiffies = get_jiffies_64();

	/* Temperatures */
	temp1 = row[HDAPS_IDX_TEMP1];
	temp2 = row[HDAPS_IDX_TEMP2];

	if (needs_calibration) {
		rest_x = pos_x;
		rest_y = pos_y;
		needs_calibration = 0;
	}

	return 0;
}

/*
 * hdaps_read_pair - reads the values from a pair of ports, placing the values
 * in the given pointers.  Returns zero on success.  Can sleep.
 * Retries until timeout if the accelerometer is not in ready status (common).
 * Does internal locking.
 */
static int hdaps_update(void)
{
	int total, ret;
	for (total=READ_TIMEOUT_MSECS; total>0; total-=RETRY_MSECS) {
		tp_controller_lock();
		ret = __hdaps_update(0);
		tp_controller_unlock();

		if (!ret)
			return 0;
		if (ret != -EBUSY)
			break;
		msleep(RETRY_MSECS);
	}
	return ret;
}

/*
 * hdaps_device_init - initialize the accelerometer.  Returns zero on success
 * and negative error code on failure.  Can sleep.
 */
static int hdaps_device_init(void)
{
	int ret = -ENXIO;
	u8 row[TP_CONTROLLER_ROW_LEN];
	u8 status;

	tp_controller_lock();

	if (tp_controller_read_row(0x13, 0x01, row))
		goto out;
	if (row[0xf]!=0x00)
		goto out;
	status = row[1];	
		
	if (status != 0x03 && /* Invertex axes (ThinkPad R50p, T41p, R42p) */
	    status != 0x02 && /* Chip already initialized */
	    status != 0x01 )  /* Normal axes */
	{ 
		printk(KERN_ERR "hdaps: initial latch check bad (0x%02x).\n",
		       status);
		goto out;
	}

	printk(KERN_DEBUG "hdaps: initial latch check good (0x%02x).\n",
	       status);

	outb(0x17, 0x1610);
	outb(0x81, 0x1611);
	outb(0x01, 0x161f);
	if (__wait_latch(0x161f, 0x00))
		goto out;
	if (__wait_latch(0x1611, 0x00))
		goto out;
	if (__wait_latch(0x1612, 0x60))
		goto out;
	if (__wait_latch(0x1613, 0x00))
		goto out;
	outb(0x14, 0x1610);
	outb(0x01, 0x1611);
	outb(0x01, 0x161f);
	if (__wait_latch(0x161f, 0x00))
		goto out;
	outb(0x10, 0x1610);
	outb(0xc8, 0x1611);
	outb(0x00, 0x1612);
	outb(0x02, 0x1613);
	outb(0x01, 0x161f);
	if (__wait_latch(0x161f, 0x00))
		goto out;
	tp_controller_invalidate();
	udelay(200);

	/* Just prefetch instead of reading, to avoid ~1sec delay on load */
	ret = tp_controller_prefetch_row(0x11, 0x01);
	goto good;

out:
	printk(KERN_ERR "hdaps: init failed!\n");
good:
	tp_controller_invalidate();
	tp_controller_unlock();
	return ret;
}


/* Device model stuff */

static int hdaps_probe(struct platform_device *dev)
{
	int ret;

	ret = hdaps_device_init();
	if (ret)
		return ret;

	printk(KERN_INFO "hdaps: device successfully initialized.\n");
	return 0;
}

static int hdaps_suspend(struct platform_device *dev, pm_message_t state)
{
	/* Don't do mouse polls until resume re-initializes the sensor. */
	del_timer_sync(&hdaps_timer);
	return 0;
}

static int hdaps_resume(struct platform_device *dev)
{
	return hdaps_device_init();
}

static struct platform_driver hdaps_driver = {
	.probe = hdaps_probe,
	.suspend = hdaps_suspend,
	.resume = hdaps_resume,
	.driver	= {
		.name = "hdaps",
		.owner = THIS_MODULE,
	},
};

/*
 * hdaps_calibrate - Set our "resting" values.
 * Does its own locking.
 */
static void hdaps_calibrate(void)
{
	needs_calibration = 1;
	hdaps_update();
	/* If that fails, the mousedev poll will take care of things later. */
}

/* Timer handler for updating the input device. Runs in softirq context,
 * so avoid lenghty or blocking operations.
 */
static void hdaps_mousedev_poll(unsigned long unused)
{
	int ret;

	/* Cannot sleep.  Try nonblockingly.  If we fail, try again later. */
	if (tp_controller_trylock())
		goto keep_active;

	ret = __hdaps_update(1); /* fast update, we're in softirq context */
	/* Any of "successful", "not yet ready" and "not prefetched"? */
	if (ret!=0 && ret!=-EBUSY && ret!=-ENODATA) {
		printk(KERN_ERR 
		       "hdaps: poll failed, disabling mousedev updates\n");
		goto out;
	}

keep_active:
	mod_timer(&hdaps_timer, jiffies + HDAPS_POLL_PERIOD);
	/* Even if we failed now, pos_x,y may have been updated earlier. */
	input_report_abs(hdaps_idev, ABS_X, pos_x - rest_x);
	input_report_abs(hdaps_idev, ABS_Y, pos_y - rest_y);
	input_sync(hdaps_idev);
out:
	tp_controller_unlock();
}


/* Sysfs Files */

static ssize_t hdaps_position_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	int ret = hdaps_update();
	if (ret)
		return ret;
	return sprintf(buf, "(%d,%d)\n", pos_x, pos_y);
}

static ssize_t hdaps_variance_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	int ret = hdaps_update();
	if (ret)
		return ret;
	return sprintf(buf, "(%d,%d)\n", var_x, var_y);
}

static ssize_t hdaps_temp1_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret = hdaps_update();
	if (ret)
		return ret;
	return sprintf(buf, "%u\n", temp1);
}

static ssize_t hdaps_temp2_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret = hdaps_update();
	if (ret)
		return ret;
	return sprintf(buf, "%u\n", temp2);
}

static ssize_t hdaps_keyboard_activity_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	/* Time-insensitive, so hdaps_mousedev_poll ensure updates. */
	return sprintf(buf, "%u\n", 
	   get_jiffies_64() < last_keyboard_jiffies + KMACT_REMEMBER_PERIOD);
}

static ssize_t hdaps_mouse_activity_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	/* Time-insensitive, so hdaps_mousedev_poll ensure updates. */
	return sprintf(buf, "%u\n", 
	   get_jiffies_64() < last_mouse_jiffies + KMACT_REMEMBER_PERIOD);
}

static ssize_t hdaps_calibrate_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "(%d,%d)\n", rest_x, rest_y);
}

static ssize_t hdaps_calibrate_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	hdaps_calibrate();
	return count;
}

static ssize_t hdaps_invert_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", hdaps_invert);
}

static ssize_t hdaps_invert_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int invert;

	if (sscanf(buf, "%d", &invert) != 1 || (invert != 1 && invert != 0))
		return -EINVAL;

	hdaps_invert = invert;
	hdaps_calibrate();

	return count;
}

static DEVICE_ATTR(position, 0444, hdaps_position_show, NULL);
static DEVICE_ATTR(variance, 0444, hdaps_variance_show, NULL);
static DEVICE_ATTR(temp1, 0444, hdaps_temp1_show, NULL);
static DEVICE_ATTR(temp2, 0444, hdaps_temp2_show, NULL);
static DEVICE_ATTR(keyboard_activity, 0444, hdaps_keyboard_activity_show, NULL);
static DEVICE_ATTR(mouse_activity, 0444, hdaps_mouse_activity_show, NULL);
static DEVICE_ATTR(calibrate, 0644, hdaps_calibrate_show,hdaps_calibrate_store);
static DEVICE_ATTR(invert, 0644, hdaps_invert_show, hdaps_invert_store);

static struct attribute *hdaps_attributes[] = {
	&dev_attr_position.attr,
	&dev_attr_variance.attr,
	&dev_attr_temp1.attr,
	&dev_attr_temp2.attr,
	&dev_attr_keyboard_activity.attr,
	&dev_attr_mouse_activity.attr,
	&dev_attr_calibrate.attr,
	&dev_attr_invert.attr,
	NULL,
};

static struct attribute_group hdaps_attribute_group = {
	.attrs = hdaps_attributes,
};


/* Module stuff */

/* hdaps_dmi_match - found a match.  return one, short-circuiting the hunt. */
static int hdaps_dmi_match(struct dmi_system_id *id)
{
	printk(KERN_INFO "hdaps: %s detected.\n", id->ident);
	return 1;
}

/* hdaps_dmi_match_invert - found an inverted match. */
static int hdaps_dmi_match_invert(struct dmi_system_id *id)
{
	hdaps_invert = 1;
	printk(KERN_INFO "hdaps: inverting axis readings.\n");
	return hdaps_dmi_match(id);
}

#define HDAPS_DMI_MATCH_NORMAL(model)	{		\
	.ident = "IBM " model,				\
	.callback = hdaps_dmi_match,			\
	.matches = {					\
		DMI_MATCH(DMI_BOARD_VENDOR, "IBM"),	\
		DMI_MATCH(DMI_PRODUCT_VERSION, model)	\
	}						\
}

#define HDAPS_DMI_MATCH_INVERT(model)	{		\
	.ident = "IBM " model,				\
	.callback = hdaps_dmi_match_invert,		\
	.matches = {					\
		DMI_MATCH(DMI_BOARD_VENDOR, "IBM"),	\
		DMI_MATCH(DMI_PRODUCT_VERSION, model)	\
	}						\
}

static int __init hdaps_init(void)
{
	int ret;

	/* Note that DMI_MATCH(...,"ThinkPad T42") will match "ThinkPad T42p" */
	struct dmi_system_id hdaps_whitelist[] = {
		HDAPS_DMI_MATCH_INVERT("ThinkPad R50p"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad R50"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad R51"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad R52"),
		HDAPS_DMI_MATCH_INVERT("ThinkPad T41p"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad T41"),
		HDAPS_DMI_MATCH_INVERT("ThinkPad T42p"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad T42"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad T43"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad X40"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad X41 Tablet"),
		HDAPS_DMI_MATCH_NORMAL("ThinkPad X41"),
		{ .ident = NULL }
	};

	if (!(dmi_check_system(hdaps_whitelist) || hdaps_force)) {
		printk(KERN_WARNING "hdaps: supported laptop not found!\n");
		ret = -ENXIO;
		goto out;
	}

	ret = platform_driver_register(&hdaps_driver);
	if (ret)
		goto out;

	pdev = platform_device_register_simple("hdaps", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto out_driver;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &hdaps_attribute_group);
	if (ret)
		goto out_device;

	hdaps_idev = input_allocate_device();
	if (!hdaps_idev) {
		ret = -ENOMEM;
		goto out_group;
	}

	/* calibration for the input device (deferred to avoid delay) */
	needs_calibration = 1;

	/* initialize the input class */
	hdaps_idev->name = "hdaps";
	hdaps_idev->cdev.dev = &pdev->dev;
	hdaps_idev->evbit[0] = BIT(EV_ABS);
	input_set_abs_params(hdaps_idev, ABS_X,
			-256, 256, HDAPS_INPUT_FUZZ, HDAPS_INPUT_FLAT);
	input_set_abs_params(hdaps_idev, ABS_Y,
			-256, 256, HDAPS_INPUT_FUZZ, HDAPS_INPUT_FLAT);

	input_register_device(hdaps_idev);

	/* start up our timer for the input device */
	init_timer(&hdaps_timer);
	hdaps_timer.function = hdaps_mousedev_poll;
	hdaps_timer.expires = jiffies + HDAPS_POLL_PERIOD;
	add_timer(&hdaps_timer);

	printk(KERN_INFO "hdaps: driver successfully loaded.\n");
	return 0;

out_group:
	sysfs_remove_group(&pdev->dev.kobj, &hdaps_attribute_group);
out_device:
	platform_device_unregister(pdev);
out_driver:
	platform_driver_unregister(&hdaps_driver);
out:
	printk(KERN_WARNING "hdaps: driver init failed (ret=%d)!\n", ret);
	return ret;
}

static void __exit hdaps_exit(void)
{
	del_timer_sync(&hdaps_timer);
	input_unregister_device(hdaps_idev);
	sysfs_remove_group(&pdev->dev.kobj, &hdaps_attribute_group);
	platform_device_unregister(pdev);
	platform_driver_unregister(&hdaps_driver);

	printk(KERN_INFO "hdaps: driver unloaded.\n");
}

module_init(hdaps_init);
module_exit(hdaps_exit);

module_param_named(invert, hdaps_invert, bool, 0);
MODULE_PARM_DESC(invert, "invert data along each axis");

module_param_named(force, hdaps_force, bool, 0);
MODULE_PARM_DESC(force, "force loading on non whitelisted laptops");

MODULE_AUTHOR("Robert Love");
MODULE_DESCRIPTION("IBM Hard Drive Active Protection System (HDAPS) driver");
MODULE_LICENSE("GPL v2");
