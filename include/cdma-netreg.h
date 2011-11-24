/*
 *
 *  oFono - Open Source Telephony
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

#ifndef __OFONO_CDMA_NETREG_H
#define __OFONO_CDMA_NETREG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

enum cdma_netreg_status {
	CDMA_NETWORK_REGISTRATION_STATUS_NOT_REGISTERED =	0,
	CDMA_NETWORK_REGISTRATION_STATUS_REGISTERED =		1,
	CDMA_NETWORK_REGISTRATION_STATUS_ROAMING =		2,
};

struct ofono_cdma_netreg;

typedef void (*ofono_cdma_netreg_serving_system_cb_t)(
				const struct ofono_error *error,
				const char *sid,
				void *data);

struct ofono_cdma_netreg_driver {
	const char *name;
	int (*probe)(struct ofono_cdma_netreg *cdma_netreg,
				unsigned int vendor,
				void *data);
	void (*remove)(struct ofono_cdma_netreg *cdma_netreg);
	void (*serving_system)(struct ofono_cdma_netreg *cdma_netreg,
			ofono_cdma_netreg_serving_system_cb_t cb, void *data);
};

void ofono_cdma_netreg_status_notify(struct ofono_cdma_netreg *netreg,
					enum cdma_netreg_status status);
void ofono_cdma_netreg_strength_notify(struct ofono_cdma_netreg *netreg,
					int strength);
void ofono_cdma_netreg_data_strength_notify(struct ofono_cdma_netreg *netreg,
						int data_strength);
int ofono_cdma_netreg_get_status(struct ofono_cdma_netreg *netreg);

int ofono_cdma_netreg_driver_register(
				const struct ofono_cdma_netreg_driver *d);
void ofono_cdma_netreg_driver_unregister(
				const struct ofono_cdma_netreg_driver *d);

struct ofono_cdma_netreg *ofono_cdma_netreg_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data);

void ofono_cdma_netreg_register(struct ofono_cdma_netreg *cdma_netreg);
void ofono_cdma_netreg_remove(struct ofono_cdma_netreg *cdma_netreg);

void ofono_cdma_netreg_set_data(struct ofono_cdma_netreg *cdma_netreg,
				void *data);
void *ofono_cdma_netreg_get_data(struct ofono_cdma_netreg *cdma_netreg);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CDMA_NETREG_H */
