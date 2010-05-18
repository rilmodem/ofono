/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __OFONO_NETTIME_H
#define __OFONO_NETTIME_H

#ifdef __cplusplus
extern "C" {
#endif

struct ofono_network_time;

struct ofono_nettime_context {
	struct ofono_nettime_driver *driver;
	struct ofono_modem *modem;
	void *data;
};

struct ofono_nettime_driver {
	const char *name;
	int (*probe)(struct ofono_nettime_context *context);
	void (*remove)(struct ofono_nettime_context *context);
	void (*info_received)(struct ofono_nettime_context *context,
				struct ofono_network_time *info);
};

int ofono_nettime_driver_register(const struct ofono_nettime_driver *driver);
void ofono_nettime_driver_unregister(const struct ofono_nettime_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_NETTIME_H */
