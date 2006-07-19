/*
 *  tp_base.h - interface to the ThinkPad embedded controller LPC3 functions
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
	u16 mask; /* bitmap of which entries of val[] are meaningful */
	u8 val[TP_CONTROLLER_ROW_LEN];
};

extern int tp_controller_lock(void);
extern int tp_controller_try_lock(void);
extern void tp_controller_unlock(void);

extern int tp_controller_read_row(const struct tp_controller_row *args,
                                  struct tp_controller_row *data);
extern int tp_controller_try_read_row(const struct tp_controller_row *args,
                                      struct tp_controller_row *mask);
extern int tp_controller_prefetch_row(const struct tp_controller_row *args);
extern void tp_controller_invalidate(void);


#endif /* __KERNEL */
#endif /* _TP_BASE_H */
