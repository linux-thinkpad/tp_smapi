/*
 *  tp_base.h - coordinate access to ThinkPad-specific hardware resources
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

#ifndef _TP_BASE_H
#define _TP_BASE_H

#ifdef __KERNEL__

#define TP_CONTROLLER_BASE_PORT 0x1600
#define TP_CONTROLLER_NUM_PORTS 0x20
#define TP_CONTROLLER_ROW_LEN 16

/* Get exclusive lock for accesing the controller. */
extern void tp_controller_lock(void);

/* Likewise but non-blocking. Returns 0 if acquired lock. */
extern int tp_controller_trylock(void);

/* Release lock. */
extern void tp_controller_unlock(void);

/* Read a data row from the controller. Caller must hold controller lock. */
extern int tp_controller_read_row(u8 arg1610, u8 arg161F, u8* buf);

/* Prefetch data row from the controller. A subsequent call to
 * tp_controller_read_row() with the same arguments will be faster
 * (if it happens neither too soon nor too late).
 * Caller must hold controller lock.
 */
extern int tp_controller_prefetch_row(u8 arg1610, u8 arg161F);

/* Invalidate the prefetched controller data.
 * Must be called before unclocking by any code that accesses the controller
 * ports directly.
 */
extern void tp_controller_invalidate(void);


#endif /* __KERNEL */
#endif /* _TP_BASE_H */
