/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  ST-Ericsson AB.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

typedef void (*caif_rtnl_create_cb_t) (int index, const char *ifname,
							void *user_data);

extern int caif_rtnl_create_interface(int type, int connid, int loop,
				caif_rtnl_create_cb_t cb, void *user_data);
extern int caif_rtnl_delete_interface(int index);

extern int caif_rtnl_init(void);
extern void caif_rtnl_exit(void);
