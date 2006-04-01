/*
 *  tp_smapi.c - ThinkPad SMAPI support
 * 
 *  This driver exposes some features of the System Management Application 
 *  Program Interface (SMAPI) BIOS found on ThinkPad laptops. It works on
 *  models in which the SMAPI BIOS runs in SMM and is invoked by writing
 *  to the APM control port 0xB2. Older models use a different interface; 
 *  for those, try the "thinkpad" module.
 *  It also exposes battery status information, obtained from the ThinkPad
 *  embedded controller (via the tp_base module).
 *
 *
 *  Copyright (C) 2005 Shem Multinymous <multinymous@gmail.com>.
 *  SMAPI access code based on the mwave driver by Mike Sullivan,
 *  Copyright (C) 1999 IBM Corporation.
 *  Module interface and DMI whitelist based on the hdaps module by
 *  Robert Love and Jesper Juhl.
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

#define TP_VERSION "0.19"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/mc146818rtc.h>	/* CMOS defines */
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/tp_base.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
#include <linux/platform_device.h>
#else
#include <linux/device.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
#error Requires kernel 2.6.13 or higher. Use tp_smapi 0.08 for older kernels.
#endif

#define TP_DESC "ThinkPad SMAPI Support"
#define TP_DIR "smapi"

MODULE_AUTHOR("Shem Multinymous");
MODULE_DESCRIPTION(TP_DESC);
MODULE_VERSION(TP_VERSION);
MODULE_LICENSE("GPL");

#define TP_LOG  "tp_smapi: "
#define TP_ERR    KERN_ERR     TP_LOG
#define TP_WARN   KERN_WARNING TP_LOG
#define TP_NOTICE KERN_NOTICE  TP_LOG
#define TP_INFO   KERN_INFO    TP_LOG
#define TP_DEBUG  KERN_DEBUG   TP_LOG

static int tp_debug = 0;
module_param_named(debug, tp_debug, int, 0600);
MODULE_PARM_DESC(debug, "Debug level (0=off, 1=on)");

#define DPRINTK(fmt, args...) { if (tp_debug) printk(TP_DEBUG fmt, ## args); }

/* provide smapi/cd_speed? redundant and dangerous - see README */
/* #define PROVIDE_CD_SPEED  */



/*********************************************************************
 * SMAPI interface
 */

/* SMAPI functions (register BX when calling). 
   For most of these we don't know the exact meaning or arguments. */
#define SMAPI_GET_LCD_BRIGHTNESS_2              0x1004
#define SMAPI_SET_LCD_BRIGHTNESS_2              0x1005
#define SMAPI_GET_DOCKING_OPTION                0x1602
#define SMAPI_SET_DOCKING_OPTION                0x1603
#define SMAPI_GET_SOUND_STATUS                  0x2002
#define SMAPI_SET_SOUND_STATUS                  0x2003
#define SMAPI_GET_DISCHARGING                   0x2104
#define SMAPI_SET_DISCHARGING                   0x2105
#define SMAPI_GET_INHIBIT_CHARGE_STATUS         0x2114
#define SMAPI_SET_INHIBIT_CHARGE_STATUS         0x2115
#define SMAPI_GET_THRESH_START                  0x2116
#define SMAPI_SET_THRESH_START                  0x2117
#define SMAPI_GET_DISCHARGE_STATUS              0x2118
#define SMAPI_SET_DISCHARGE_STATUS              0x2119
#define SMAPI_GET_THRESH_STOP                   0x211a
#define SMAPI_SET_THRESH_STOP                   0x211b
#define SMAPI_GET_LCD_BRIGHTNESS_1              0x4102
#define SMAPI_SET_LCD_BRIGHTNESS_1              0x4103
#define SMAPI_GET_CPU_BUS_POWER_SAVING_OPTION   0x4002
#define SMAPI_SET_CPU_BUS_POWER_SAVING_OPTION   0x4003
#define SMAPI_GET_PCI_BUS_POWER_SAVING_OPTION   0x4004
#define SMAPI_SET_PCI_BUS_POWER_SAVING_OPTION   0x4005
#define SMAPI_GET_CPU_PERF_CONTROL_STATUS       0x4006
#define SMAPI_SET_CPU_PERF_CONTROL_STATUS       0x4007
#define SMAPI_GET_CDROM_STATUS                  0x8220
#define SMAPI_SET_CDROM_STATUS                  0x8221
#define SMAPI_GET_FAN_CONTROL_STATUS_1          0x826c
#define SMAPI_SET_FAN_CONTROL_STATUS_1          0x826d
#define SMAPI_GET_FAN_CONTROL_STATUS_2          0x826e
#define SMAPI_SET_FAN_CONTROL_STATUS_2          0x826f

/* SMAPI error codes (see ThinkPad 770 Technical Reference Manual p.83
   http://www-307.ibm.com/pc/support/site.wss/document.do?lndocid=PFAN-3TUQQD */
static struct {u8 rc; char *msg; int ret;} smapi_rc[]=
{
	{0x00,"OK",0},
	{0x53,"SMAPI fuction is not available",-ENXIO},
	{0x81,"Invalid parameter",-EINVAL},
	{0x86,"Function is not supported by SMAPI BIOS",-ENOSYS},
	{0x90,"System error",-EIO},
	{0x91,"System is invalid",-EIO},
	{0x92,"System is busy,-EBUSY"},
	{0xa0,"Device error (disk read error)",-EIO},
	{0xa1,"Device is busy",-EBUSY},
	{0xa2,"Device is not attached",-ENXIO},
	{0xa3,"Device is disbled",-EIO},
	{0xa4,"Request parameter is out of range",-EINVAL},
	{0xa5,"Request parameter is not accepted",-EINVAL},
	{0xa6,"Transient error",-EBUSY}, /* ? */
	{0xff,"Unknown error code",-EIO} /* EOF marker */
};


#define SMAPI_PORT2 0x4F              /* fixed port, meaning unclear */
static unsigned short smapi_port = 0; /* APM control port, normally 0xB2 */

static DECLARE_MUTEX(smapi_sem);

#define SMAPI_MAX_RETRIES 10

/* Read SMAPI port from CMOS */
static int find_smapi_port(void)
{
	u16 smapi_id = 0;
	unsigned short port = 0;
	unsigned long flags;

	spin_lock_irqsave(&rtc_lock, flags);
	smapi_id = CMOS_READ(0x7C);
	smapi_id |= (CMOS_READ(0x7D) << 8);
	spin_unlock_irqrestore(&rtc_lock, flags);

	if (smapi_id != 0x5349) {
		printk(TP_ERR "SMAPI not supported (ID=0x%x)\n", smapi_id);
		return -ENXIO;
	}
	spin_lock_irqsave(&rtc_lock, flags);
	port = CMOS_READ(0x7E);
	port |= (CMOS_READ(0x7F) << 8);
	spin_unlock_irqrestore(&rtc_lock, flags);
	if (port == 0) {
		printk(TP_ERR "unable to read SMAPI port number\n");
		return -ENXIO;
	}
	return port;
}

/* SMAPI call: invoke SM BIOS */
static int smapi_request(u32 inBX, u32 inCX,
			 u32 inDI, u32 inSI,
			 u32 *outAX, u32 *outBX,
			 u32 *outCX, u32 *outDX,
			 u32 *outDI, u32 *outSI, const char** msg)
{
	int ret = 0;
	int i;
	int retries = SMAPI_MAX_RETRIES;
	u32 myoutAX, myoutBX, myoutCX, myoutDX, myoutDI, myoutSI;
	u8 rc;

retry:
	DPRINTK("req_in: BX=%x CX=%x DI=%x SI=%x\n",
		(int)inBX, (int)inCX, (int)inDI, (int)inSI);

	tp_controller_lock(); /* SMAPI and tp_base use different interfaces to
	                       * the same chip, so stay on the safe side.      */

	__asm__ __volatile__(
		"movl  $0x00005380,%%eax\n\t"
		"movl  %6,%%ebx\n\t"
		"movl  %7,%%ecx\n\t"
		"movl  %8,%%edi\n\t"
		"movl  %9,%%esi\n\t"
		"movw  %10,%%dx\n\t"
		"out   %%al,%%dx\n\t"  /* trigger SMI to invoke BIOS in SMM */
		"out   %%al,$0x4F\n\t"
		"movl  %%eax,%0\n\t"
		"movl  %%ebx,%1\n\t"
		"movl  %%ecx,%2\n\t"
		"movl  %%edx,%3\n\t"
		"movl  %%edi,%4\n\t"
		"movl  %%esi,%5\n\t"
		:"=m"(myoutAX),
		 "=m"(myoutBX),
		 "=m"(myoutCX),
		 "=m"(myoutDX),
		 "=m"(myoutDI),
		 "=m"(myoutSI)
		:"m"(inBX), "m"(inCX), "m"(inDI), "m"(inSI),
		 "m"((u16)smapi_port)
		:"%eax", "%ebx", "%ecx", "%edx", "%edi",
		 "%esi");

	tp_controller_invalidate();
	tp_controller_unlock();

	*outAX=myoutAX; *outBX=myoutBX; *outCX=myoutCX;
	*outDX=myoutDX; *outDI=myoutDI; *outSI=myoutSI;

	/* Look up error code */
	rc = (myoutAX>>8)&0xFF;
	for (i=0; smapi_rc[i].rc!=0xFF && smapi_rc[i].rc!=rc; ++i) {}
	ret = smapi_rc[i].ret;
	*msg = smapi_rc[i].msg;

	DPRINTK("req_out: AX=%x BX=%x CX=%x DX=%x DI=%x SI=%x ret=%d\n",
	        (int)*outAX, (int)*outBX, (int)*outCX, (int)*outDX,
	        (int)*outDI, (int)*outSI, ret);
	if (ret)
		printk(TP_NOTICE "SMAPI error: %s (func=%x)\n", *msg, (int)inBX);

	if (ret==-EBUSY && --retries) {
		/* Retry */
		DPRINTK("retrying (%d left)...\n", retries);
		msleep(50);
		goto retry;
	}
	msleep(5); /* Don't let the next access happen too quickly */
	return ret;
}

/* Convenience wrapper: make output arguments optional */
static int smapi_read(u32 inBX, u32 inCX,
                      u32 inDI, u32 inSI,
                      u32 *outBX, u32 *outCX, 
                      u32 *outDX,
                      u32 *outDI, u32 *outSI,
                      const char** msg)
{
	u32 tmpAX=0, tmpBX=0, tmpCX=0, tmpDX=0, tmpDI=0, tmpSI=0;
	return smapi_request(inBX, inCX, inDI, inSI, &tmpAX,
	                     outBX?outBX:&tmpBX, outCX?outCX:&tmpCX,
	                     outDX?outDX:&tmpDX, outDI?outDI:&tmpDI,
	                     outSI?outSI:&tmpSI, msg);
}

/* Convenience wrapper: discard output arguments */
static int smapi_write(u32 inBX, u32 inCX,
                       u32 inDI, u32 inSI, const char **msg)
{
	u32 outAX=0, outBX=0, outCX, outDX=0, outDI=0, outSI=0;
	return smapi_request(inBX, inCX, inDI, inSI,
	                     &outAX, &outBX, &outCX, &outDX, &outDI, &outSI,
	                     msg);
}


/*********************************************************************
 * ThinkPad controller readout
 */

/* Lock controller and read row */
static int tpc_read_row(u8 arg1610, u8 arg161F, u8* buf) {
	int ret;
	tp_controller_lock();
	ret = tp_controller_read_row(arg1610, arg161F, buf);
	tp_controller_unlock();
	return ret;
}


/*********************************************************************
 * Specific SMAPI services
 */

#define MIN_THRESH_DELTA      4  /* Min delta between start and stop thresh */
#define MIN_THRESH_START      2
#define MAX_THRESH_START      (100-MIN_THRESH_DELTA)
#define MIN_THRESH_STOP       (MIN_THRESH_START + MIN_THRESH_DELTA)
#define MAX_THRESH_STOP       100
#define DEFAULT_THRESH_START  MAX_THRESH_START
#define DEFAULT_THRESH_STOP   MAX_THRESH_STOP
#define BATMAX_FIX            1  /* Compatibility with IBM's Battery Maximizer
                                  * which shows the start threshold as 1 more 
                                  * than the value written to the controller.
                                  */

#define THRESH_START 1
#define THRESH_STOP  0

/* Read physical charge start/stop threshold from embedded controller 
 * (1..99, 0=default)
 */
static int get_real_thresh(int bat, int start, int *thresh, 
                           u32 *outDI, u32 *outSI) 
{
	u32 bx = start ? SMAPI_GET_THRESH_START : SMAPI_GET_THRESH_STOP;
	u32 cx = (bat+1)<<8;
	const char* msg;
	int ret = smapi_read(bx, cx, 0, 0, NULL, &cx, NULL, outDI, outSI, &msg);
	if (ret) {
		printk(TP_NOTICE "cannot get %s_thresh of battery %d: %s\n", 
		       start?"start":"stop", bat, msg);
		return ret;
	}
	if (!(cx&0x00000100)) {
		printk(TP_NOTICE "cannot get %s_thresh of battery %d: cx=0%x\n", 
		       start?"start":"stop", bat, (int)cx);
		return -EIO;
	}
	if (thresh)
		*thresh = cx&0xFF;
	return 0;
}

/* Get charge start/stop threshold (1..100), 
 * substituting default values if needed and applying BATMAT_FIX.
 */
static int get_thresh(int bat, int start, int *thresh) {
	int ret = get_real_thresh(bat, start, thresh, NULL, NULL);
	if (ret)
		return ret;
	if (*thresh==0)
		*thresh = start ? DEFAULT_THRESH_START : DEFAULT_THRESH_STOP;
	else if (start)
		*thresh += BATMAX_FIX;
	return 0;
}

/* Write battery start/top charge threshold to embedded controller
 * (1..99, 0=default) 
 */
static int set_real_thresh(int bat, int start, int thresh) {
	u32 bx = start ? SMAPI_SET_THRESH_START : SMAPI_SET_THRESH_STOP;
	u32 cx = ((bat+1)<<8) + thresh;
	u32 getDI, getSI;
	const char* msg;
	int ret;

	/* verify read before writing */
	ret = get_real_thresh(bat, start, NULL, &getDI, &getSI);
	if (ret)
		return ret;

	ret = smapi_write(bx, cx, getDI, getSI, &msg);
	if (ret)
		printk(TP_NOTICE
		       "cannot set %s thresh of battery %d to %d: %s\n", 
                       start?"start":"stop", bat, thresh, msg);
	else
		printk(TP_INFO "battery %d: changed %s threshold to %d%s\n",
		       bat, start?"start":"stop", thresh, start?"(+1)":"");
	return ret;
}

/* Set charge start/stop threshold (1..100), 
 * substituting default values if needed and applying BATMAT_FIX.
 */
static int set_thresh(int bat, int start, int thresh) {
	if (start)
		thresh -= BATMAX_FIX;
	else if (thresh==DEFAULT_THRESH_STOP) /* the EC refuses 100 */
		thresh = 0;
	return set_real_thresh(bat, start, thresh);
}


/* Get inhibit charge period (1..65535 minutes, 0=disabled) */
static int get_inhibit_charge(int bat, int *minutes, u8 *outCL) {
	u32 cx = (bat+1)<<8;
	u32 si;
	const char* msg;
	int ret = smapi_read(SMAPI_GET_INHIBIT_CHARGE_STATUS, cx, 0, 0, 
	                     NULL, &cx, NULL, NULL, &si, &msg);
	if (ret) {
		printk(TP_NOTICE "cannot get inhibit_charge of battery %d: "
		       "%s\n", bat, msg);
		return ret;
	}
	if (!(cx&0x0100)) {
		printk(TP_NOTICE "cannot get inhibit_charge of battery %d: "
		       "cx=0x%x\n", bat, (int)cx);
		return -EIO;
	}
	if (minutes)
		*minutes = (cx&0x0001)?si:0;
	if (outCL)
		*outCL = (u8)(cx);
	return 0;
}

/* Set battery inhibit charge period (1..65535 minutes, 0=disabled) */
static int set_inhibit_charge(int bat, int minutes) {
	u32 cx;
	u8 getCL;
	const char* msg;
	int ret;
	/* verify read before writing */
	ret = get_inhibit_charge(bat, NULL, &getCL); 
	if (ret)
		return ret;
	cx = ((bat+1)<<8) | (getCL&0x00FE) | (minutes>0 ? 0x0001 : 0x0000);
	ret = smapi_write(SMAPI_SET_INHIBIT_CHARGE_STATUS, cx, 
                          0, (u32)minutes, &msg);
	if (ret)
		printk(TP_NOTICE
		       "cannot set inhibit charge of battery %d to %d: %s\n", 
		       bat, minutes, msg);
	else
		printk(TP_INFO
		       "battery %d: inhibited charge for %d minutes\n", 
		       bat, minutes);
	return ret;
}

/* Get status of forced battery discharge */
static int get_force_discharge(int bat, int *enabled) 
{
	u32 cx = (bat+1)<<8;
	const char* msg;
	int status;
	int ret = smapi_read(SMAPI_GET_DISCHARGE_STATUS, cx, 0, 0, 
	                     NULL, &cx, NULL, NULL, NULL, &msg);
	if (ret) {
		printk(TP_NOTICE
		       "cannot get force_discharge of battery %d: %s\n", 
                       bat, msg);
		return ret;
	}

	/* Collect status bits (including some we don't understand) */
	if (cx&0x00000100) {
		status = 0x00;                /* can't force discharge */
	} else {
		status = 0x10;                /* can force discharge */
		status |= (cx&0x00000001)?0x01:0; /* force discharge */
		status |= (cx&0x00000040)?0x02:0; /* unknown */
		status |= (cx&0x00000200)?0x04:0; /* unknown */
		status |= (cx&0x00000400)?0x08:0; /* unknown */
	}
	DPRINTK("force_discharge status bits are 0x%02x\n", status);
	*enabled = (status&0x01)?1:0;
	return 0;
}

/* Set forced battery discharge */
static int set_force_discharge(int bat, int enabled) {
	u32 cx = (bat+1)<<8;
	const char* msg;
	int bit2 = 0; /* what does this input bit mean? */
	int ret = smapi_read(SMAPI_GET_DISCHARGE_STATUS, cx, 0, 0, 
	                     NULL, &cx, NULL, NULL, NULL, &msg);
	if (ret) {
		printk(TP_NOTICE
		       "cannot get force_discharge of battery %d: %s\n", 
                       bat, msg);
		return ret;
	}
	if (cx&0x00000100) {
		printk(TP_NOTICE "cannot force_discharge battery %d\n", 
                       bat);
		return -EIO;
	}
	cx = ((bat+1)<<8) | (cx&0xFA) | (enabled?0x01:0) | (bit2?0x04:0);
	ret = smapi_write(SMAPI_SET_DISCHARGE_STATUS, cx, 0, 0, &msg);
	if (ret)
		printk(TP_NOTICE "cannot set force_discharge of battery %d"
		       " to (%d,%d): %s\n", bat, enabled, bit2, msg);
	else
		printk(TP_INFO
		       "battery %d: set force_discharge to (%d,%d)\n", 
		       bat, enabled, bit2);
	return ret;
}


/*********************************************************************
 * Specific ThinkPad controller services
 */

static int is_battery_installed(int bat) {
	u8 row[TP_CONTROLLER_ROW_LEN];
	u8 mask;
	int ret = tpc_read_row(1, bat, row);
	if (ret)
		return ret;
	if (bat==0)
		mask=0x40;
	else if (bat==1)
		mask=0x20;
	else
		mask=0x80; /* AC power */;
	return (row[0] & mask) ? 1 : 0;
}

static int bat_has_extended_status(int bat) {
	u8 row[TP_CONTROLLER_ROW_LEN];
	int ret = tpc_read_row(1, bat, row);
	if (ret)
		return ret;
	if ((row[0] & (bat?0x20:0x40)) == 0)
		return 0;
	if ((row[1] & (0x40|0x20)) == 0)
		return 0;
	return 1;
}

#ifdef PROVIDE_CD_SPEED

static int get_cd_speed(int *speed) {
	const char* msg;
	u32 bx, dx, di;
	int ret = smapi_read(SMAPI_GET_CDROM_STATUS, 0, 0, 0, 
	                     &bx, NULL, &dx, &di, NULL, &msg);
	if (ret) {
		printk(TP_NOTICE "cannot get cd speed: %s\n", msg);
		return ret;
	}
	if (dx==0x78 && di==0x1e) {
		*speed = 2;
	} else if (dx==0x0f && di==0x00) {
		*speed = 0;
	} else {
		*speed = 1;
	}
	/* what does bx&80 mean? */
	return 0;
}

static int set_cd_speed(int speed) {
	const char* msg;
	int ret, dummy;
	short int cx, di;
	ret = get_cd_speed(&dummy); /* verify read before writing */
	if (ret)
		return ret;
	if (speed==0) {
		cx=0x0f; di=0x00;
	} else if (speed==1) {
		cx=0x1e; di=0x04;
	} else {
		cx=0x78; di=0x1e;
	}
	ret = smapi_write(SMAPI_SET_CDROM_STATUS, cx, di, 0, &msg);
	if (ret)
		printk(TP_NOTICE "cannot set cd speed to %d: %s\n", 
		       speed, msg);
	else
		printk(TP_INFO "cd speed set to level %d\n",
		       speed);
	return ret;
}

#endif /* PROVIDE_CD_SPEED */


/*********************************************************************
 * proc file read/write routines 
 */

/* Define custom device attribute struct which adds a battery number */
struct bat_device_attribute {
	struct device_attribute dev_attr;
	int bat;
};

/* Functions to parse and format controller readouts */

static int attr_get_bat(struct device_attribute *attr) {
	return container_of(attr, struct bat_device_attribute, dev_attr)->bat;
}

static int show_tpc_bat_word(const char* fmt, u8 arg1610, int off,
                             struct device_attribute *attr, char *buf)
{
	u8 row[TP_CONTROLLER_ROW_LEN];
	int bat = attr_get_bat(attr);
	int ret;
	if (bat_has_extended_status(bat)!=1) 
		return -ENXIO;
	ret = tpc_read_row(arg1610, bat, row);
	if (ret)
		return ret;
	return sprintf(buf, fmt, (int)*(u16*)(row+off));
}

static int show_tpc_bat_str(u8 arg1610, int off, int maxlen,
                            struct device_attribute *attr, char *buf)
{
	int bat = attr_get_bat(attr);
	u8 row[TP_CONTROLLER_ROW_LEN];
	int ret;
	if (bat_has_extended_status(bat)!=1) 
		return -ENXIO;
	ret = tpc_read_row(arg1610, bat, row);
	if (ret)
		return ret;
	strncpy(buf, (char*)row+off, maxlen);
	buf[maxlen] = 0;
	strcat(buf, "\n");
	return strlen(buf);
}

static int show_tpc_bat_power(u8 arg1610, int offV, int offA,
                              struct device_attribute *attr, char *buf)
{
	u8 row[TP_CONTROLLER_ROW_LEN];
	int milliamp, millivolt, ret;
	int bat = attr_get_bat(attr);
	if (bat_has_extended_status(bat)!=1) 
		return -ENXIO;
	ret = tpc_read_row(1, bat, row);
	if (ret)
		return ret;
	millivolt = *(u16*)(row+offV);
	milliamp = *(s16*)(row+offA);
	return sprintf(buf, "%d mW\n", milliamp*millivolt/1000);
}

static int show_tpc_bat_date(u8 arg1610, int off,
                             struct device_attribute *attr, char *buf)
{
	u8 row[TP_CONTROLLER_ROW_LEN];
	u16 v;
	int ret;
	int day, month, year;
	int bat = attr_get_bat(attr);
	if (bat_has_extended_status(bat)!=1) 
		return -ENXIO;
	ret = tpc_read_row(arg1610, bat, row);
	if (ret)
		return ret;

	/* Decode bit-packed: v = day | (month<<5) | ((year-1980)<<9) */
	v = *(u16*)(row+off);
	day = v & 0x1F;
	month = (v >> 5) & 0xF;
	year = (v >> 9) + 1980;
	
	return sprintf(buf, "%04d-%02d-%02d\n", year, month, day);
}


/* The actual attribute show/store functions */

static int battery_start_charge_thresh_show(struct device *dev, 
                                    struct device_attribute *attr, char *buf)
{
	int thresh;
	int bat = attr_get_bat(attr);
	int ret = get_thresh(bat, THRESH_START, &thresh);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", thresh);
}

static int battery_stop_charge_thresh_show(struct device *dev, 
                                   struct device_attribute *attr, char *buf)
{
	int thresh;
	int bat = attr_get_bat(attr);
	int ret = get_thresh(bat, THRESH_STOP, &thresh);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", thresh);
}

static int battery_start_charge_thresh_store(struct device *dev, 
                                     struct device_attribute *attr, 
                                     const char *buf, size_t count)
{
	int thresh, other_thresh, ret;
	int bat = attr_get_bat(attr);

	if (sscanf(buf, "%d", &thresh)!=1 || thresh<1) {
		printk(TP_ERR
		       "start_charge_thresh: must be between %d and %d\n", 
		       MIN_THRESH_START, MAX_THRESH_START);
		return -EINVAL;
	}
	if (thresh < MIN_THRESH_START) /* clamp up to MIN_THRESH_START */
		thresh = MIN_THRESH_START;
	if (thresh > MAX_THRESH_START) /* clamp down to MAX_THRESH_START */
		thresh = MAX_THRESH_START;

	down(&smapi_sem);
	ret = get_thresh(bat, THRESH_STOP, &other_thresh);
	if (ret!=-ENOSYS) {
		if (ret) /* other threshold is set? */
			goto out;
		ret = get_real_thresh(bat, THRESH_START, NULL, NULL, NULL);
		if (ret) /* this threshold is set? */
			goto out;
		if (other_thresh < thresh+MIN_THRESH_DELTA) { 
			/* move other thresh to keep it above this one */
			ret = set_thresh(bat, THRESH_STOP, 
			                 thresh+MIN_THRESH_DELTA);
			if (ret)
				goto out;
		}
	}
	ret = set_thresh(bat, THRESH_START, thresh);
out:
	up(&smapi_sem);
	return count;

}

static int battery_stop_charge_thresh_store(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count)
{
	int thresh, other_thresh, ret;
	int bat = attr_get_bat(attr);

	if (sscanf(buf, "%d", &thresh)!=1 || thresh>100) {
		printk(TP_ERR
		       "stop_charge_thresh: must be between %d and 100\n", 
		       MIN_THRESH_STOP);
		return -EINVAL;
	}
	if (thresh<MIN_THRESH_STOP) /* clamp up to MIN_THRESH_STOP */
		thresh = MIN_THRESH_STOP;

	down(&smapi_sem);
	ret = get_thresh(bat, THRESH_START, &other_thresh);
	if (ret!=-ENOSYS) { /* other threshold exists? */
		if (ret)
			goto out;
		/* this threshold exists? */
		ret = get_real_thresh(bat, THRESH_STOP, NULL, NULL, NULL);
		if (ret)
			goto out;
		if (other_thresh>=thresh-MIN_THRESH_DELTA) {
			 /* move other thresh to be below this one */
			ret = set_thresh(bat, THRESH_START, 
			                 thresh-MIN_THRESH_DELTA);
			if (ret)
				goto out;
		}
	}
	ret = set_thresh(bat, THRESH_STOP, thresh);
out:
	up(&smapi_sem);
	return count;
}

static int battery_inhibit_charge_minutes_show(struct device *dev, 
                               struct device_attribute *attr, char *buf)
{
	int minutes;
	int bat = attr_get_bat(attr);
	int ret = get_inhibit_charge(bat, &minutes, NULL);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", minutes);
}

static int battery_inhibit_charge_minutes_store(struct device *dev, 
                                struct device_attribute *attr,
                                const char *buf, size_t count)
{
	int ret;
	int minutes;
	int bat = attr_get_bat(attr);
	if (sscanf(buf, "%d", &minutes)!=1 || minutes<0) {
		printk(TP_ERR "inhibit_charge_minutes: "
		              "must be a non-negative integer\n");
		return -EINVAL;
	}
	if (minutes>0xFFFF)
		minutes=0xFFFF;
	ret = set_inhibit_charge(bat, minutes);
	if (ret)
		return ret;
	return count;
}

static int battery_force_discharge_show(struct device *dev, 
                                struct device_attribute *attr, char *buf)
{
	int enabled;
	int bat = attr_get_bat(attr);
	int ret = get_force_discharge(bat, &enabled);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", enabled);
}

static int battery_force_discharge_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
	int ret;
	int enabled;
	int bat = attr_get_bat(attr);
	if (sscanf(buf, "%d", &enabled)!=1 || enabled&(~1)) {
		printk(TP_ERR "force_discharge: must be 0 or 1\n");
		return -EINVAL;
	}
	ret = set_force_discharge(bat, enabled);
	if (ret)
		return ret;
	return count;
}

static int battery_installed_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	int bat = attr_get_bat(attr);
	int ret = is_battery_installed(bat);
	if (ret<0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

static int battery_state_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 row[TP_CONTROLLER_ROW_LEN];
	const char* msg;
	int ret;
	int bat = attr_get_bat(attr);
	if (bat_has_extended_status(bat)!=1) 
		return sprintf(buf, "none\n");
	ret = tpc_read_row(1, bat, row);
	if (ret)
		return ret;
	switch (row[1] & 0xf0) {
		case 0xc0: msg = "idle"; break;
		case 0xd0: msg = "discharging"; break;
		case 0xe0: msg = "charging"; break;
		default:   return sprintf(buf, "unknown (0x%x)\n", row[1]);
	}
	return sprintf(buf, "%s\n", msg);
}
static int ac_connected_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret = is_battery_installed(0xFF);
	if (ret<0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

static int battery_manufacturer_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_str(4, 2, TP_CONTROLLER_ROW_LEN-2, attr, buf);
}

static int battery_model_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_str(5, 2, TP_CONTROLLER_ROW_LEN-2, attr, buf);
}

static int battery_barcoding_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_str(7, 2, TP_CONTROLLER_ROW_LEN-2, attr, buf);
}

static int battery_chemistry_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_str(6, 2, 5, attr, buf);
}

static int battery_voltage_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%u mV\n", 1, 6, attr, buf);
}

static int battery_design_voltage_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%u mV\n", 3, 4, attr, buf);
}

static int battery_current_now_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%d mA\n", 1, 8, attr, buf);
}

static int battery_current_avg_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%d mA\n", 1, 10, attr, buf);
}

static int battery_power_now_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_power(1, 6, 8, attr, buf);
}

static int battery_power_avg_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_power(1, 6, 10, attr, buf);
}

static int battery_remaining_capacity_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%u0 mWh\n", 1, 14, attr, buf);
}

static int battery_last_full_capacity_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%u0 mWh\n", 2, 2, attr, buf);
}

static int battery_design_capacity_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%u0 mWh\n", 3, 2, attr, buf);
}

static int battery_cycle_count_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%u\n", 2, 12, attr, buf);
}

static int battery_serial_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_word("%u\n", 3, 10, attr, buf);
}

static int battery_manufacture_date_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_date(3, 8, attr, buf);
}

static int battery_first_use_date_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_tpc_bat_date(8, 2, attr, buf);
}

static int battery_dump_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	int j;
	char* p = buf;
	int bat = attr_get_bat(attr);
	u8 arg1610;
	u8 row[TP_CONTROLLER_ROW_LEN];
	int ret;

	for (arg1610=1; arg1610<=8; ++arg1610) {
		ret = tpc_read_row(arg1610, bat, row);
		if (ret)
			return ret;
		for (j=0; j<TP_CONTROLLER_ROW_LEN; j++)
			p += sprintf(p, "%02x ", row[j]);
		p += sprintf(p, "\n");
		if (arg1610==1 && bat_has_extended_status(bat)!=1)
			goto done;
		if ( (p-buf)>PAGE_SIZE-256 ) {
			printk(TP_ERR "aps_dump_show: too much output!\n");
			return -EIO;
		}
	}
done:
	return p-buf;
}

#ifdef PROVIDE_CD_SPEED

static int cd_speed_show(struct device *dev, 
                         struct device_attribute *attr, char *buf)
{
	int speed;
	int ret = get_cd_speed(&speed);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", speed);
}

static int cd_speed_store(struct device *dev, 
                          struct device_attribute *attr,
                          const char *buf, size_t count)
{
	int ret;
	int speed;
	if (sscanf(buf, "%d", &speed)<1 || speed<0 || speed>2) {
		printk(TP_ERR "cd_speed: must be between 0 and 2\n");
		return -EINVAL;
	}
	ret = set_cd_speed(speed);
	if (ret)
		return ret;
	return count;
}

#endif /* PROVIDE_CD_SPEED */


/*********************************************************************
 * Power management: the embedded controller forgets the battery 
 * thresholds when the system is suspended to disk and unplugged from
 * AC and battery, so we restore it upon resume.
 */

static int saved_threshs[4] = {-1, -1, -1, -1};  /* -1 = don't know */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
static int tp_suspend(struct platform_device *dev, pm_message_t state)
{
#else
static int tp_suspend(struct device *dev, pm_message_t state, uint32_t level)
{
	if (level == SUSPEND_SAVE_STATE) {
#endif
	if (get_real_thresh(0, THRESH_STOP , &saved_threshs[0], NULL, NULL)) 
		saved_threshs[0]=-1;
	if (get_real_thresh(0, THRESH_START, &saved_threshs[1], NULL, NULL))
		saved_threshs[1]=-1;
	if (get_real_thresh(1, THRESH_STOP , &saved_threshs[2], NULL, NULL))
		saved_threshs[2]=-1;
	if (get_real_thresh(1, THRESH_START, &saved_threshs[3], NULL, NULL))
		saved_threshs[3]=-1;
	DPRINTK("suspend saved: %d %d %d %d\n", saved_threshs[0],
	        saved_threshs[1], saved_threshs[2], saved_threshs[3]);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
	}
#endif
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
static int tp_resume(struct platform_device *dev)
{
#else
static int tp_resume(struct device *dev, u32 level)
{
	if (level == RESUME_RESTORE_STATE) {
#endif
	DPRINTK("resume restoring: %d %d %d %d\n", saved_threshs[0], 
	        saved_threshs[1], saved_threshs[2], saved_threshs[3]);
	if (saved_threshs[0]>=0) 
		set_real_thresh(0, THRESH_STOP , saved_threshs[0]);
	if (saved_threshs[1]>=0)
		set_real_thresh(0, THRESH_START, saved_threshs[1]);
	if (saved_threshs[2]>=0)
		set_real_thresh(1, THRESH_STOP , saved_threshs[2]);
	if (saved_threshs[3]>=0)
		set_real_thresh(1, THRESH_START, saved_threshs[3]);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
	}
#endif
	return 0;
}


/*********************************************************************
 * Driver model
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
static struct platform_driver tp_driver = {
	.suspend = tp_suspend,
	.resume = tp_resume,
	.driver = {
		.name = "smapi",
		.owner = THIS_MODULE
	},
};
#else
static struct device_driver tp_driver = {
	.name = "smapi",
	.bus = &platform_bus_type,
	.owner = THIS_MODULE,
	.suspend = tp_suspend,
	.resume = tp_resume
};
#endif


/*********************************************************************
 * Sysfs device model 
 */

/* Attributes in /sys/devices/platform/smapi/ */
#ifdef PROVIDE_CD_SPEED
static DEVICE_ATTR(cd_speed, 0644, cd_speed_show, cd_speed_store);
#endif
static DEVICE_ATTR(ac_connected, 0644, ac_connected_show, NULL);
static struct attribute *tp_root_attributes[] = {
#ifdef PROVIDE_CD_SPEED
	&dev_attr_cd_speed.attr,
#endif
	&dev_attr_ac_connected.attr,
	NULL
};
static struct attribute_group tp_root_attribute_group = {
	.attrs = tp_root_attributes
};

/* Attributes under /sys/devices/platform/smapi/BAT{0,1}/ :
 * Every attribute needs to be defined (i.e., statically allocated) for
 * each battery, and then referenced in the attribute list of each battery.
 * We use preprocessor voodoo to avoid duplicating the list of attributes.
 */

/* This macro processes all attributes via the given "functions" args: */

#define DO_BAT_ATTRS(_bat, _ATTR_RW, _ATTR_R) \
	_ATTR_RW(_bat, start_charge_thresh) \
	_ATTR_RW(_bat, stop_charge_thresh) \
	_ATTR_RW(_bat, inhibit_charge_minutes) \
	_ATTR_RW(_bat, force_discharge) \
	_ATTR_R (_bat, installed) \
	_ATTR_R (_bat, state) \
	_ATTR_R (_bat, manufacturer) \
	_ATTR_R (_bat, model) \
	_ATTR_R (_bat, barcoding) \
	_ATTR_R (_bat, chemistry) \
	_ATTR_R (_bat, voltage) \
	_ATTR_R (_bat, current_now) \
	_ATTR_R (_bat, current_avg) \
	_ATTR_R (_bat, power_now) \
	_ATTR_R (_bat, power_avg) \
	_ATTR_R (_bat, remaining_capacity) \
	_ATTR_R (_bat, last_full_capacity) \
	_ATTR_R (_bat, design_voltage) \
	_ATTR_R (_bat, design_capacity) \
	_ATTR_R (_bat, cycle_count) \
	_ATTR_R (_bat, serial) \
	_ATTR_R (_bat, manufacture_date) \
	_ATTR_R (_bat, first_use_date) \
	_ATTR_R (_bat, dump)

/* Now define the macro "functions" for DO_BAT_ATTRS: */

#define BAT_DEVICE_ATTR(_name,_bat,_mode,_show,_store) \
	struct bat_device_attribute dev_attr_##_name##_##_bat = { \
		.dev_attr = __ATTR(_name,_mode,_show,_store), \
		.bat = _bat \
	};

#define DEF_BAT_ATTR_RW(_bat,_name) \
	static BAT_DEVICE_ATTR(_name, _bat, 0644, \
	                       battery_##_name##_show, \
	                       battery_##_name##_store);

#define DEF_BAT_ATTR_R(_bat,_name) \
	static BAT_DEVICE_ATTR(_name, _bat, 0444, \
	                       battery_##_name##_show, NULL);

#define REF_BAT_ATTR(_bat,_name) \
	&dev_attr_##_name##_##_bat.dev_attr.attr,


/* Battery 0 */

DO_BAT_ATTRS(0, DEF_BAT_ATTR_RW, DEF_BAT_ATTR_R)
static struct attribute *tp_bat0_attributes[] = {
	DO_BAT_ATTRS(0, REF_BAT_ATTR, REF_BAT_ATTR)
	NULL
};
static struct attribute_group tp_bat0_attribute_group = {
	.name  = "BAT0",
	.attrs = tp_bat0_attributes
};

/* Battery 1 */

DO_BAT_ATTRS(1, DEF_BAT_ATTR_RW, DEF_BAT_ATTR_R)
static struct attribute *tp_bat1_attributes[] = {
	DO_BAT_ATTRS(1, REF_BAT_ATTR, REF_BAT_ATTR)
	NULL
};
static struct attribute_group tp_bat1_attribute_group = {
	.name  = "BAT1",
	.attrs = tp_bat1_attributes
};

/* List of attribute groups */

static struct attribute_group *attr_groups[] = {
	&tp_root_attribute_group,
	&tp_bat0_attribute_group,
	&tp_bat1_attribute_group,
	NULL
};


/*********************************************************************
 * Init and cleanup
 */

static struct platform_device *pdev;
static struct attribute_group **next_attr_group; /* next to register */

static int __init tp_init(void)
{
	int ret;
	printk(TP_INFO "tp_smapi " TP_VERSION " loading...\n");

	ret = find_smapi_port();
	if (ret<0)
		goto err;
	else
		smapi_port = ret;

	if (!request_region(smapi_port, 1, "smapi")) {
		printk(TP_ERR "cannot claim port 0x%x\n", smapi_port);
		ret = -ENXIO;
		goto err;
	}

	if (!request_region(SMAPI_PORT2, 1, "smapi")) {
		printk(TP_ERR "cannot claim port 0x%x\n", SMAPI_PORT2);
		ret = -ENXIO;
		goto err_port1;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
	ret = platform_driver_register(&tp_driver);
#else
	ret = driver_register(&tp_driver);
#endif
	if (ret)
		goto err_port2;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
	pdev = platform_device_alloc("smapi", -1);
	if (!pdev) {
		ret = -ENOMEM;
		goto err_driver;
	}

	ret = platform_device_add(pdev);
	if (ret)
		goto err_device_free;
#else
	pdev = platform_device_register_simple("smapi", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_driver;
	}
#endif

	for (next_attr_group = attr_groups; *next_attr_group; ++next_attr_group) {
		ret = sysfs_create_group(&pdev->dev.kobj, *next_attr_group);
		if (ret)
			goto err_attr;
	}

	printk(TP_INFO "successfully loaded (smapi_port=0x%x).\n", smapi_port);
	return 0;

err_attr:
	while (--next_attr_group >= attr_groups)
		sysfs_remove_group(&pdev->dev.kobj, *next_attr_group);
	platform_device_unregister(pdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
err_device_free:
	platform_device_put(pdev);
#endif
err_driver:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
	platform_driver_unregister(&tp_driver);
#else
	driver_unregister(&tp_driver);
#endif
err_port2:
	release_region(SMAPI_PORT2, 1);
err_port1:
	release_region(smapi_port, 1);
err:
	printk(TP_ERR "driver init failed (ret=%d)!\n", ret);
	return ret;
}

static void __exit tp_exit(void)
{
	while (next_attr_group && --next_attr_group >= attr_groups)
		sysfs_remove_group(&pdev->dev.kobj, *next_attr_group);
	platform_device_unregister(pdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
	platform_driver_unregister(&tp_driver);
#else
	driver_unregister(&tp_driver);
#endif
	release_region(SMAPI_PORT2, 1);
	if (smapi_port)
		release_region(smapi_port, 1);

	printk(TP_INFO "driver unloaded.\n");
}

module_init(tp_init);
module_exit(tp_exit);
