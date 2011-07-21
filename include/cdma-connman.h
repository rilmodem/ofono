/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011 Nokia Corporation. All rights reserved.
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

#ifndef __OFONO_CDMA_CONNMAN_H
#define __OFONO_CDMA_CONNMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_cdma_connman;

#define OFONO_CDMA_CONNMAN_MAX_USERNAME_LENGTH 63
#define OFONO_CDMA_CONNMAN_MAX_PASSWORD_LENGTH 255

typedef void (*ofono_cdma_connman_cb_t)(const struct ofono_error *error,
						void *data);
typedef void (*ofono_cdma_connman_up_cb_t)(const struct ofono_error *error,
						const char *interface,
						ofono_bool_t static_ip,
						const char *address,
						const char *netmask,
						const char *gw,
						const char **dns,
						void *data);

struct ofono_cdma_connman_driver {
	const char *name;
	int (*probe)(struct ofono_cdma_connman *cm, unsigned int vendor,
						void *data);
	void (*remove)(struct ofono_cdma_connman *cm);
	void (*activate)(struct ofono_cdma_connman *cm,
						const char *username,
						const char *password,
						ofono_cdma_connman_up_cb_t cb,
						void *data);
	void (*deactivate)(struct ofono_cdma_connman *cm,
						ofono_cdma_connman_cb_t cb,
						void *data);
};

int ofono_cdma_connman_driver_register(
				const struct ofono_cdma_connman_driver *d);
void ofono_cdma_connman_driver_unregister(
				const struct ofono_cdma_connman_driver *d);

struct ofono_cdma_connman *ofono_cdma_connman_create(
						struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data);

void ofono_cdma_connman_register(struct ofono_cdma_connman *cm);
void ofono_cdma_connman_remove(struct ofono_cdma_connman *cm);

void ofono_cdma_connman_set_data(struct ofono_cdma_connman *cm,
						void *data);
void *ofono_cdma_connman_get_data(struct ofono_cdma_connman *cm);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CDMA_CONNMAN_H */
