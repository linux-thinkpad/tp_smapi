/*
 *  tp_base.c - coordinate access to ThinkPad-specific hardware resources
 * 
 *  ThinkPad laptops have a controller, accessible at ports 0x1600-0x161F,
 *  which provides system management services (currently known: battery
 *  information and accelerometer readouts). This driver coordinates access
 *  to the controller, and abstracts it to the extent possible.
 *
 *
 *  Copyright (C) 2005 Shem Multinymous <multinymous@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/tp_base.h>
#include <linux/jiffies.h>
#include <asm/io.h>

MODULE_AUTHOR("Shem Multinymous");
MODULE_DESCRIPTION("ThinkPad hardware access coordination");
MODULE_VERSION("0.03");
MODULE_LICENSE("GPL");

#define TPC_BASE_PORT 0x1600
#define TPC_NUM_PORTS 0x20

#define TPC_READ_RETRIES 100
#define TPC_READ_UDELAY 5

#define TPC_PREFETCH_TIMEOUT   (HZ/10)  /* invalidate prefetch after 0.1sec */
#define TPC_PREFETCH_INVALID   INITIAL_JIFFIES

static DECLARE_MUTEX(tp_controller_sem);

static u64 prefetch_jiffies = TPC_PREFETCH_INVALID;  /* time of prefetch */
static u8 prefetch_arg1610, prefetch_arg161F;   /* args of last prefetch */


/*** Functionality ***/

void tp_controller_lock(void) {
	down(&tp_controller_sem);
}

EXPORT_SYMBOL_GPL(tp_controller_lock); 

int tp_controller_trylock(void) {
	return down_trylock(&tp_controller_sem);
}

EXPORT_SYMBOL_GPL(tp_controller_trylock); 

void tp_controller_unlock(void) {
	up(&tp_controller_sem);
}

EXPORT_SYMBOL_GPL(tp_controller_unlock);

/* Tell embedded controller to prepare a row */
static int tp_controller_request_row(u8 arg1610, u8 arg161F) {
	u8 status;
	status = inb(0x1604);
	if (status&0x40) { /* readout data already pending? */
		inb(0x161F); /* marks end of previous transaction */
		return -EBUSY; /* EC will be ready in a few usecs */
	}

	/* send 1st argument */
	outb(arg1610, 0x1610);
	status = inb(0x1604);
	if (status!=0x20) { /* not accepted? */
		return -EBUSY; /* the EC is handling a prior request */
	}

	/* send 2nd argument */
	outb(arg161F, 0x161F);
	status = inb(0x1604);
	if (status==0x20) { /* not responding? */
		printk(KERN_WARNING "tp_base: 161F rejected (status=%#x)\n",
		       status);
		return -EIO;  /* this is abnormal */
	}

	return 0;
}

/* Read current row data from the controller, assuming it's already 
 * requested. 
 */
static int tp_controller_read_data(u8* buf) {
	int i;
	u8 status = inb(0x1604);
	/* After writing to ports 0x1610 and 0x161F, the status register at
	 * IO port 0x1604 assumes the sequence of values 0xA0, 0x00, 0x10 and
	 * finally bit 0x40 goes up (usually 0x50) signalling data ready.
	 * It takes about a dozen nanosecs total, with very high variance.
	 */
	if (status==0xA0 || status==0x00 || status==0x10)
		return -EBUSY; /* not ready yet */
	if (!(status&0x40)) {
		printk(KERN_WARNING "tp_base: bad status (%#x) in read\n",
		       (int)status);
		return -EIO;
	}
		
	/* Data in IO ports 0x1610-0x161F. Reading 0x161F ends transaction. */
	for (i=0; i<TP_CONTROLLER_ROW_LEN; ++i)
		buf[i] = inb(0x1610+i);
	if (inb(0x1604)&0x40) /* readout still pending? */
		printk(KERN_WARNING "tp_base: data pending after read\n");
	return 0;
}

/* Is the given row currently prefetched? */
static int tp_controller_is_row_fetched(u8 arg1610, u8 arg161F) {
	return (prefetch_jiffies != TPC_PREFETCH_INVALID) &&
	       (prefetch_arg1610 == arg1610) &&
	       (prefetch_arg161F == arg161F) &&
	       (get_jiffies_64() < prefetch_jiffies + TPC_PREFETCH_TIMEOUT);
}

/* Read a row from the embedded controller */
int tp_controller_read_row(u8 arg1610, u8 arg161F, u8* buf) {
	int retries, ret;
	if (tp_controller_is_row_fetched(arg1610,arg161F))
		goto read_row; /* already requested */

	/* Request the row */
	for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
		ret = tp_controller_request_row(arg1610, arg161F);
		if (!ret)
			goto read_row;
		if (ret != -EBUSY)
			break;
		udelay(TPC_READ_UDELAY);
	}
	printk(KERN_ERR 
	       "thinkpad controller read(%#x,%#x): failed requesting row\n",
	       (int)arg1610, (int)arg161F);
	goto out;

read_row:
	/* Read the row's data */
	for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
		ret = tp_controller_read_data(buf);
		if (!ret)
			goto out;
		if (ret!=-EBUSY)
			break;
		udelay(TPC_READ_UDELAY);
	}

	printk(KERN_ERR 
	       "thinkpad controller read(%#x,%#x): failed waiting for data\n",
	       (int)arg1610, (int)arg161F);

out:
	prefetch_jiffies = TPC_PREFETCH_INVALID;
	return ret;
}

EXPORT_SYMBOL_GPL(tp_controller_read_row);

/* Read a prefetched row from the controller. Don't fetch, don't retry. */
int tp_controller_try_read_row(u8 arg1610, u8 arg161F, u8* buf) {
	if (!tp_controller_is_row_fetched(arg1610,arg161F))
		return -ENODATA;
	else {
		prefetch_jiffies = TPC_PREFETCH_INVALID; /* data eaten up */
		return tp_controller_read_data(buf);
	}
}

EXPORT_SYMBOL_GPL(tp_controller_try_read_row);

/* Prefech a row from the controller. This is a one-shot prefetch
 * attempt, without retries or delays.
 */
int tp_controller_prefetch_row(u8 arg1610, u8 arg161F) {
	int ret = tp_controller_request_row(arg1610, arg161F);
	if (ret) {
		prefetch_jiffies = TPC_PREFETCH_INVALID;
	} else {
		prefetch_jiffies = get_jiffies_64();
		prefetch_arg1610 = arg1610;
		prefetch_arg161F = arg161F;
	}
	return ret;
}

EXPORT_SYMBOL_GPL(tp_controller_prefetch_row);

void tp_controller_invalidate(void) {
	prefetch_jiffies = TPC_PREFETCH_INVALID;
}

EXPORT_SYMBOL_GPL(tp_controller_invalidate);


/*** Model whitelist ***/

#define TP_DMI_MATCH(vendor,model)	{		\
	.ident = "IBM " model,				\
	.matches = {					\
		DMI_MATCH(DMI_BOARD_VENDOR, vendor),	\
		DMI_MATCH(DMI_PRODUCT_VERSION, model)	\
	}						\
}

static int is_thinkpad(void) {
	struct dmi_system_id tp_whitelist[] = {
		TP_DMI_MATCH("LENOVO","ThinkPad"),
		TP_DMI_MATCH("IBM","ThinkPad"),
		TP_DMI_MATCH("IBM","Not Available"), /* e.g., ThinkPad R40 */
		{ .ident = NULL }
	};
	return dmi_check_system(tp_whitelist);
}


/*** Init and cleanup ***/

static int __init tp_base_init(void) {
	if (!is_thinkpad()) {
		printk(KERN_ERR "tp_base: not a ThinkPad!\n");
		return -ENODEV;
	}

	if (!request_region(TPC_BASE_PORT, 
	                    TPC_NUM_PORTS , "ThinkPad controller")) {
		printk(KERN_ERR "tp_base: cannot claim ports %#x-%#x"
		       " (conflict with old hdaps driver?)\n",
		       TPC_BASE_PORT,
		       TPC_BASE_PORT+TPC_NUM_PORTS -1);
		return -ENXIO;
	}
	printk(KERN_INFO "tp_base: loaded.\n");
	return 0;
}

static void __exit tp_base_exit(void) {
	release_region(TPC_BASE_PORT, TPC_NUM_PORTS );
	printk(KERN_INFO "tp_base: unloaded.\n");
}

module_init(tp_base_init);
module_exit(tp_base_exit);
