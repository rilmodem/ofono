/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) 2010 ProFUSION embedded systems.
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

#ifndef __OFONO_LOCATION_REPORTING_H
#define __OFONO_LOCATION_REPORTING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_location_reporting;

enum ofono_location_reporting_type {
	OFONO_LOCATION_REPORTING_TYPE_NMEA = 0,
};

typedef void (*ofono_location_reporting_enable_cb_t)(
						const struct ofono_error *error,
						int fd, void *data);
typedef void (*ofono_location_reporting_disable_cb_t)(
						const struct ofono_error *error,
						void *data);

struct ofono_location_reporting_driver {
	const char *name;
	enum ofono_location_reporting_type type;
	int (*probe)(struct ofono_location_reporting *lr, unsigned int vendor,
								void *data);
	void (*remove)(struct ofono_location_reporting *lr);
	void (*enable)(struct ofono_location_reporting *lr,
			ofono_location_reporting_enable_cb_t cb, void *data);
	void (*disable)(struct ofono_location_reporting *lr,
			ofono_location_reporting_disable_cb_t cb, void *data);
};

int ofono_location_reporting_driver_register(
			const struct ofono_location_reporting_driver *d);
void ofono_location_reporting_driver_unregister(
			const struct ofono_location_reporting_driver *d);

struct ofono_location_reporting *ofono_location_reporting_create(
						struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data);

void ofono_location_reporting_register(struct ofono_location_reporting *lr);
void ofono_location_reporting_remove(struct ofono_location_reporting *lr);

void ofono_location_reporting_set_data(struct ofono_location_reporting *lr,
								void *data);
void *ofono_location_reporting_get_data(struct ofono_location_reporting *lr);

struct ofono_modem *ofono_location_reporting_get_modem(
					struct ofono_location_reporting *lr);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_LOCATION_REPORTING_H */
