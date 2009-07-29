/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_MODEM_H
#define __OFONO_MODEM_H

#ifdef __cplusplus
extern "C" {
#endif

void ofono_modem_add_interface(struct ofono_modem *modem,
				const char *interface);

void ofono_modem_remove_interface(struct ofono_modem *modem,
					const char *interface);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_MODEM_H */
