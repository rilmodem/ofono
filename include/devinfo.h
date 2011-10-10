/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_DEVINFO_H
#define __OFONO_DEVINFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_devinfo;

typedef void (*ofono_devinfo_query_cb_t)(const struct ofono_error *error,
					const char *attribute, void *data);

struct ofono_devinfo_driver {
	const char *name;
	int (*probe)(struct ofono_devinfo *info, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_devinfo *info);
	void (*query_manufacturer)(struct ofono_devinfo *info,
			ofono_devinfo_query_cb_t cb, void *data);
	void (*query_serial)(struct ofono_devinfo *info,
			ofono_devinfo_query_cb_t cb, void *data);
	void (*query_model)(struct ofono_devinfo *info,
			ofono_devinfo_query_cb_t cb, void *data);
	void (*query_revision)(struct ofono_devinfo *info,
			ofono_devinfo_query_cb_t cb, void *data);
};

int ofono_devinfo_driver_register(const struct ofono_devinfo_driver *d);
void ofono_devinfo_driver_unregister(const struct ofono_devinfo_driver *d);

struct ofono_devinfo *ofono_devinfo_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data);
void ofono_devinfo_register(struct ofono_devinfo *info);
void ofono_devinfo_remove(struct ofono_devinfo *info);

void ofono_devinfo_set_data(struct ofono_devinfo *info, void *data);
void *ofono_devinfo_get_data(struct ofono_devinfo *info);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_MODEM_INFO_H */
