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
MODULE_VERSION("0.02");
MODULE_LICENSE("GPL");

static DECLARE_MUTEX(tp_controller_sem);

#define TPC_READ_RETRIES 1000
#define TPC_READ_UDELAY 5

#define TPC_PREFETCH_TIMEOUT   (HZ/2)  /* invalidate prefetch after 0.5sec */
#define TPC_PREFETCH_INVALID   INITIAL_JIFFIES
static u64 prefetch_jiffies = TPC_PREFETCH_INVALID;  /* time of prefetch */
static u8 prefetch_arg1610, prefetch_arg161F;   /* args of last prefetch */


#define TP_DMI_MATCH(model)	{		\
	.ident = "IBM " model,				\
	.matches = {					\
		DMI_MATCH(DMI_BOARD_VENDOR, "IBM"),	\
		DMI_MATCH(DMI_PRODUCT_VERSION, model)	\
	}						\
}

static int is_thinkpad(void) {
	struct dmi_system_id tp_whitelist[] = {
		TP_DMI_MATCH("ThinkPad"),      /* starts with "ThinkPad" */
		TP_DMI_MATCH("Not Available"), /* e.g., ThinkPad R40 */
		{ .ident = NULL }
	};
	return dmi_check_system(tp_whitelist);
}

void tp_controller_lock(void) {
	down(&tp_controller_sem);
}

int tp_controller_trylock(void) {
	return down_trylock(&tp_controller_sem);
}

void tp_controller_unlock(void) {
	up(&tp_controller_sem);
}

/* Tell controller to prepare a row */
static int tp_controller_fetch_row(u8 arg1610, u8 arg161F) {
	int retries;
	for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
		if (inb(0x1604)&0x40) { /* readout pending? */
			inb(0x161F); /* discard it */
			udelay(TPC_READ_UDELAY);
		} else {
			outb(arg1610, 0x1610);
			if (inb(0x1604)&0x20) /* accepted? */
				goto wrote1610;
		}
	}
	printk(KERN_ERR "thinkpad controller read(%x,%x): "
	       "failed writing to 0x1610\n", (int)arg1610, (int)arg161F);
	return -EIO;

wrote1610:
	outb(arg161F, 0x161F);
	return 0;
}

/* Read a row from the controller */
int tp_controller_read_row(u8 arg1610, u8 arg161F, u8* buf) {
	int retries, i;
	int ret;

	/* Check if this row was prefetched, and fetch otherwise */
	if ((prefetch_jiffies != TPC_PREFETCH_INVALID) &&
	    (prefetch_arg1610 == arg1610) &&
	    (prefetch_arg161F == arg161F) &&
	    (get_jiffies_64() < prefetch_jiffies + TPC_PREFETCH_TIMEOUT))
	{
		ret = 0; /* rely on prefetch */
	} else {
		ret = tp_controller_fetch_row(arg1610, arg161F);
	}
	prefetch_jiffies = TPC_PREFETCH_INVALID;
	if (ret)
		return ret;

	/* Wait for row to become ready */
	for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
		if (inb(0x1604)&0x40)
			goto gotdata;
		udelay(TPC_READ_UDELAY);
	}
	printk(KERN_ERR "thinkpad controller read(%x,%x): "
	       "failed waiting for data\n", (int)arg1610, (int)arg161F);
	return -EIO;

gotdata:
	for (i=0; i<TP_CONTROLLER_ROW_LEN; ++i) {
		buf[i] = inb(0x1610+i);
	}

	return 0;
}

/* Prefeth a row from the controller */
int tp_controller_prefetch_row(u8 arg1610, u8 arg161F) {
	int ret = tp_controller_fetch_row(arg1610, arg161F);
	if (ret) {
		prefetch_jiffies = TPC_PREFETCH_INVALID;
	} else {
		prefetch_jiffies = get_jiffies_64();
		prefetch_arg1610 = arg1610;
		prefetch_arg161F = arg161F;
	}
	return ret;
}

void tp_controller_invalidate(void) {
	prefetch_jiffies = TPC_PREFETCH_INVALID;
}

/* Init and cleanup */

static int __init tp_base_init(void) {
	if (!is_thinkpad()) {
		printk(KERN_ERR "tp_base: not a ThinkPad!\n");
		return -ENODEV;
	}

	if (!request_region(TP_CONTROLLER_BASE_PORT, 
	                    TP_CONTROLLER_NUM_PORTS, "ThinkPad controller")) {
		printk(KERN_ERR "tp_base: cannot claim ports 0x%x-0x%x"
		       " (conflict with old hdaps driver?)\n",
		       TP_CONTROLLER_BASE_PORT,
		       TP_CONTROLLER_BASE_PORT+TP_CONTROLLER_NUM_PORTS-1);
		return -ENXIO;
	}
	printk(KERN_INFO "tp_base: loaded.\n");
	return 0;
}

static void __exit tp_base_exit(void) {
	release_region(TP_CONTROLLER_BASE_PORT, TP_CONTROLLER_NUM_PORTS);
	printk(KERN_INFO "tp_base: unloaded.\n");
}

EXPORT_SYMBOL_GPL(tp_controller_lock); 
EXPORT_SYMBOL_GPL(tp_controller_trylock); 
EXPORT_SYMBOL_GPL(tp_controller_unlock);
EXPORT_SYMBOL_GPL(tp_controller_read_row);
EXPORT_SYMBOL_GPL(tp_controller_prefetch_row);
EXPORT_SYMBOL_GPL(tp_controller_invalidate);

module_init(tp_base_init);
module_exit(tp_base_exit);
