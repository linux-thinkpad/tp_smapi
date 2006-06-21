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

#define TP_CONTROLLER_ROW_LEN 16

struct tp_controller_row {
	u8 val[TP_CONTROLLER_ROW_LEN];
	u16 mask;
};

/* tp_controller_lock:
 * Get exclusive lock for accesing the controller. 
 */
extern int tp_controller_lock(void);

/* tp_controller_trylock:
 * Likewise, but non-blocking. Returns 0 if acquired lock. 
 */
extern int tp_controller_try_lock(void);

/* tp_controller_unlock:
 * Release lock. 
 */
extern void tp_controller_unlock(void);

/* tp_controller_read_row:
 * Read a data row from the controller, fetching and retrying if needed.
 * The row args are specified by 16 byte arguments, some of which may be 
 * missing (but the first and last are mandatory). These are given in 
 * args->val[],   args->val[i] is used iff (args->mask>>i)&1).
 * The rows's data is stored in data->val[], but is only guaranteed to be 
 * valid for indices corresponding to set bit in data->maska. That is,
 * if (data->mask>>i)&1==0 then data->val[i] may not be filled (to save time).
 * Returns -EBUSY on transient error and -EIO on abnormal condition.
 * Caller must hold controller lock. 
 */
extern int tp_controller_read_row(struct tp_controller_row *args,
                                  struct tp_controller_row *data);

/* tp_controller_try_read_row:
 * Try read a prefetched row from the controller. Don't fetch or retry.
 * See tp_controller_read_row above for the meaning of the arguments.
 * Returns -EBUSY is data not ready and -ENODATA if row not prefetched.
 * Caller must hold controller lock. 
 */
extern int tp_controller_try_read_row(struct tp_controller_row *args,
                                      struct tp_controller_row *mask);

/* tp_controller_prefetch_row:
 * Prefetch data row from the controller. A subsequent call to
 * tp_controller_read_row() with the same arguments will be faster,
 * and a subsequent call to tp_controller_try_read_row stands a 
 * good chance of succeeding if done neither too soon nor too late.
 * See tp_controller_read_row above for the meaning of the arguments.
 * Returns -EBUSY on transient error and -EIO on abnormal condition.
 * Caller must hold controller lock.
 */
extern int tp_controller_prefetch_row(struct tp_controller_row *args);

/* tp_controller_invalidate:
 * Invalidate the prefetched controller data.
 * Must be called before unlocking by any code that accesses the controller
 * ports directly.
 */
extern void tp_controller_invalidate(void);


#endif /* __KERNEL */
#endif /* _TP_BASE_H */
