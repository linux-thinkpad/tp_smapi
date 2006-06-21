/*
 *  tp_base.c - coordinate access to ThinkPad-specific hardware resources
 * 
 *  The embedded controller on ThinkPad laptops has a non-standard interface
 *  at IO ports 0x1600-0x161F (mapped to LCP channel 3 of the H8S chip).
 *  The interface provides various system management services (currently 
 *  known: battery information and accelerometer readouts). This driver
 *  provides access and mutual exclusion for the EC interface.
 *  H8S hardware documentation and terminology is used in this file:
 *  "H8S/2104B Group Hardware Manual",
 * http://documentation.renesas.com/eng/products/mpumcu/rej09b0300_2140bhm.pdf
 *
 *  Copyright (C) 2006 Shem Multinymous <multinymous@gmail.com>
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

#define TP_VERSION "0.22"

MODULE_AUTHOR("Shem Multinymous");
MODULE_DESCRIPTION("ThinkPad embedded controller hardware access");
MODULE_VERSION(TP_VERSION);
MODULE_LICENSE("GPL");

/* IO ports used by embedded controller LPC channel 3: */
#define TPC_BASE_PORT 0x1600
#define TPC_NUM_PORTS 0x20
#define TPC_STR3_PORT 0x1604  /* Reads H8S EC register STR3 */
#define TPC_TWR0_PORT  0x1610 /* Mapped to H8S EC register TWR0MW/SW  */
#define TPC_TWR15_PORT 0x161F /* Mapped to H8S EC register TWR15 */
                 /* port 1610+i is mapped to H8S reg TWRi for 0<i<16 */

/* H8S STR3 status flags (see H8S/2104B Group Hardware Manual p.549) */
#define H8S_STR3_IBF3B 0x80  /* Bidi. Data Register Input Buffer Full */
#define H8S_STR3_OBF3B 0x40  /* Bidi. Data Register Output Buffer Full */
#define H8S_STR3_MWMF  0x20  /* Master Write Mode Flag */
#define H8S_STR3_SWMF  0x10  /* Slave Write Mode Flag */
#define H8S_STR3_MASK  0xF0  /* All bits we care about in STR3 */

/* Timeouts and retries */
#define TPC_READ_RETRIES    150
#define TPC_READ_NDELAY     500
#define TPC_REQUEST_RETRIES 100
#define TPC_REQUEST_NDELAY   10
#define TPC_PREFETCH_TIMEOUT   (HZ/10)  /* invalidate prefetch after 0.1sec */

/* Module parameters: */
static int tp_debug = 0;
module_param_named(debug, tp_debug, int, 0600);
MODULE_PARM_DESC(debug, "Debug level (0=off, 1=on)");

/* A few macros for printk()ing: */
#define DPRINTK(fmt, args...) \
  do { if (tp_debug) printk(KERN_DEBUG fmt, ## args); } while (0)
#define MSG_FMT(fmt, args...) \
  "tp_base: %s: " fmt "\n", __func__, ## args
#define ARG_FMT(msg,code) \
  MSG_FMT("%s: (0x%02x:0x%02x)->0x%02x", \
          msg, args->val[0x0], args->val[0xF], code)

/* State of request prefetching: */
static u8 prefetch_arg0, prefetch_argF;           /* Args of last prefetch */
static u64 prefetch_jiffies;                      /* time of prefetch, or: */
#define TPC_PREFETCH_NONE   INITIAL_JIFFIES       /* - No prefetch */
#define TPC_PREFETCH_JUNK   INITIAL_JIFFIES+1     /* - Ignore prefetch */

/* Locking: */

static DECLARE_MUTEX(tp_controller_mutex);

int tp_controller_lock(void) 
{
	int ret;
	ret = down_interruptible(&tp_controller_mutex);
	if (ret)
		DPRINTK("tp_controller mutex down interrupted: %d\n", ret);
	return ret;
}

EXPORT_SYMBOL_GPL(tp_controller_lock); 

int tp_controller_try_lock(void) 
{
	return down_trylock(&tp_controller_mutex);
}

EXPORT_SYMBOL_GPL(tp_controller_try_lock); 

void tp_controller_unlock(void) 
{
 	up(&tp_controller_mutex);
}

EXPORT_SYMBOL_GPL(tp_controller_unlock);

/* Tell embedded controller to prepare a row */
static int tp_controller_request_row(struct tp_controller_row *args) 
{
	u8 str3;
	int i;

	/* EC protocol requires write to TWR0 (function code): */
	if (!(args->mask & 0x0001)) {
		printk(KERN_ERR MSG_FMT("bad args->mask=0x%02x", args->mask));
		return -EINVAL;
	}

	/* EC protocol requires write to TWR15. Default to 0x01: */
	if (!(args->mask & 0x8000))
		args->val[0xF] = 0x01;

	/* Check initial STR3 status: */
	str3 = inb(TPC_STR3_PORT) & H8S_STR3_MASK;
	if (str3 & H8S_STR3_OBF3B) { /* data already pending */
		inb(TPC_TWR15_PORT); /* marks end of previous transaction */
		if (prefetch_jiffies == TPC_PREFETCH_NONE)
			printk(KERN_WARNING
			       ARG_FMT("readout already pending", str3));
		return -EBUSY; /* EC will be ready in a few usecs */
	} else if (str3 == H8S_STR3_SWMF) { /* busy with previous request */
		if (prefetch_jiffies == TPC_PREFETCH_NONE)
			printk(KERN_WARNING
			       ARG_FMT("EC handles previous request", str3));
		return -EBUSY; /* data will be pending in a few usecs */
	} else if (str3 != 0x00) { /* unexpected status? */
		printk(KERN_WARNING ARG_FMT("bad initial STR3", str3));
		return -EIO;
	}

	/* Send TWR0MW: */
	outb(args->val[0], TPC_TWR0_PORT);
	str3 = inb(TPC_STR3_PORT) & H8S_STR3_MASK;
	if (str3 != H8S_STR3_MWMF) { /* not accepted? */
		printk(KERN_WARNING ARG_FMT("arg0 rejected", str3));
		return -EIO;
	}

	/* Send TWR1 through TWR14: */
	for (i=1; i<TP_CONTROLLER_ROW_LEN-1; i++)
		if ((args->mask>>i)&1)
			outb(args->val[i], TPC_TWR0_PORT+i);

	/* Send TWR15. This marks end of command. */
	outb(args->val[0xF], TPC_TWR15_PORT);

	/* Wait until EC starts writing its reply (~60ns on average).
	 * Releasing locks before this happens may cause an EC hang
	 * due to firmware bug!
	 */
	for (i=0; i<TPC_REQUEST_RETRIES; ++i) {
		str3 = inb(TPC_STR3_PORT) & H8S_STR3_MASK;
		if (str3 & H8S_STR3_SWMF) /* EC started replying */
			return 0;
		else if (str3==(H8S_STR3_IBF3B|H8S_STR3_MWMF) ||
		         str3==0x00) /* normal progress, wait it out */
			ndelay(TPC_REQUEST_NDELAY);
		else { /* weird EC status */
			printk(KERN_WARNING
			       ARG_FMT("bad end STR3", str3));
			return -EIO;
		}
	}
	printk(KERN_WARNING ARG_FMT("EC is mysteriously silent", str3));
	return -EIO;
}

/* Read current row data from the controller, assuming it's already 
 * requested. 
 */
static int tp_controller_read_data(struct tp_controller_row *data)
{
	int i;
	u8 str3 = inb(TPC_STR3_PORT) & H8S_STR3_MASK;
	/* Once we make a request, STR3 assumes the following sequence of 
         * values as it reads the request and writes its data.
	 * It takes about a few dozen nanosecs total, with very high variance.
	 */
	if (str3==(H8S_STR3_IBF3B|H8S_STR3_MWMF) ||
	    str3==0x00 ||
	    str3==H8S_STR3_SWMF )
		return -EBUSY; /* not ready yet */
	/* Finally, it signals output buffer full: */
	if (str3 != (H8S_STR3_OBF3B|H8S_STR3_SWMF)) {
		printk(KERN_WARNING
		       MSG_FMT("bad initial STR3 (0x%02x)", str3));
		return -EIO;
	}
		
	/* Read first byte (signals start of read transactions): */
	data->val[0] = inb(TPC_TWR0_PORT);
	/* Optionally read 14 more bytes: */
	for (i=1; i<TP_CONTROLLER_ROW_LEN-1; ++i)
		if ((data->mask >> i)&1)
			data->val[i] = inb(TPC_TWR0_PORT+i);
	/* Read last byte from 0x161F (signals end of read transaction): */
	data->val[0xF] = inb(TPC_TWR15_PORT);
		
	/* Readout still pending? */
	str3 = inb(TPC_STR3_PORT) & H8S_STR3_MASK;
	if (str3 & H8S_STR3_OBF3B)
		printk(KERN_WARNING
		       MSG_FMT("OBF3B=1 after read (0x%02x)", str3));
	return 0;
}

/* Is the given row currently prefetched? 
 * To keep things simple we compare only the first and last args;
 * in practice this suffices                                        .*/
static int tp_controller_is_row_fetched(struct tp_controller_row *args)
{
	return (prefetch_jiffies != TPC_PREFETCH_NONE) &&
	       (prefetch_jiffies != TPC_PREFETCH_JUNK) &&
	       (prefetch_arg0 == args->val[0]) &&
	       (prefetch_argF == args->val[0xF]) &&
	       (get_jiffies_64() < prefetch_jiffies + TPC_PREFETCH_TIMEOUT);
}

/* Read a row from the embedded controller */
int tp_controller_read_row(struct tp_controller_row *args,
                           struct tp_controller_row *data)
{
	int retries, ret;

	if (tp_controller_is_row_fetched(args))
		goto read_row; /* already requested */

	/* Request the row */
	for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
		ret = tp_controller_request_row(args);
		if (!ret)
			goto read_row;
		if (ret != -EBUSY)
			break;
		ndelay(TPC_READ_NDELAY);
	}
	printk(KERN_ERR ARG_FMT("failed requesting row", ret));
	goto out;

read_row:
	/* Read the row's data */
	for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
		ret = tp_controller_read_data(data);
		if (!ret)
			goto out;
		if (ret!=-EBUSY)
			break;
		ndelay(TPC_READ_NDELAY);
	}

	printk(KERN_ERR ARG_FMT("failed waiting for data", ret));

out:
	prefetch_jiffies = TPC_PREFETCH_JUNK;
	return ret;
}

EXPORT_SYMBOL_GPL(tp_controller_read_row);

/* Read a prefetched row from the controller. Don't fetch, don't retry. */
int tp_controller_try_read_row(struct tp_controller_row *args,
                               struct tp_controller_row *data)
{
	int ret;
	if (!tp_controller_is_row_fetched(args)) {
		ret = -ENODATA;
	} else {
		ret = tp_controller_read_data(data);
		if (!ret)
			prefetch_jiffies = TPC_PREFETCH_NONE; /* eaten up */
	}
	return ret;
}

EXPORT_SYMBOL_GPL(tp_controller_try_read_row);

/* Prefech a row from the controller. This is a one-shot prefetch
 * attempt, without retries or delays.
 */
int tp_controller_prefetch_row(struct tp_controller_row *args)
{
	int ret;
 	ret = tp_controller_request_row(args);
	if (ret) {
		prefetch_jiffies = TPC_PREFETCH_JUNK;
	} else {
		prefetch_jiffies = get_jiffies_64();
		prefetch_arg0 = args->val[0x0];
		prefetch_argF = args->val[0xF];
	}
	return ret;
}

EXPORT_SYMBOL_GPL(tp_controller_prefetch_row);

void tp_controller_invalidate(void) 
{
	prefetch_jiffies = TPC_PREFETCH_JUNK;
}

EXPORT_SYMBOL_GPL(tp_controller_invalidate);


/*** Model whitelist ***/

#define TP_DMI_MATCH(vendor,model)	{		\
	.ident = vendor " " model,			\
	.matches = {					\
		DMI_MATCH(DMI_BOARD_VENDOR, vendor),	\
		DMI_MATCH(DMI_PRODUCT_VERSION, model)	\
	}						\
}

static int is_thinkpad(void) 
{
	struct dmi_system_id tp_whitelist[] = {
		TP_DMI_MATCH("LENOVO","ThinkPad"),
		TP_DMI_MATCH("IBM","ThinkPad"),
		TP_DMI_MATCH("IBM","Not Available"), /* e.g., ThinkPad R40 */
		{ .ident = NULL }
	};
	return dmi_check_system(tp_whitelist);
}


/*** Init and cleanup ***/

static int __init tp_base_init(void)
{
	if (!is_thinkpad()) {
		printk(KERN_ERR "tp_base: not a ThinkPad!\n");
		return -ENODEV;
	}

	if (!request_region(TPC_BASE_PORT, 
	                    TPC_NUM_PORTS , "ThinkPad controller")) {
		printk(KERN_ERR "tp_base: cannot claim ports %#x-%#x"
		       " (conflict with old hdaps driver?)\n",
		       TPC_BASE_PORT,
		       TPC_BASE_PORT + TPC_NUM_PORTS -1);
		return -ENXIO;
	}
	prefetch_jiffies = TPC_PREFETCH_JUNK;
	printk(KERN_INFO "tp_base: tp_base " TP_VERSION " loaded.\n");
	return 0;
}

static void __exit tp_base_exit(void)
{
	release_region(TPC_BASE_PORT, TPC_NUM_PORTS );
	printk(KERN_INFO "tp_base: unloaded.\n");
}

module_init(tp_base_init);
module_exit(tp_base_exit);
