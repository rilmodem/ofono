/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011 ST-Ericsson AB.
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

#ifndef __OFONO_GNSS_H
#define __OFONO_GNSS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_gnss;

typedef void (*ofono_gnss_cb_t)(const struct ofono_error *error, void *data);

struct ofono_gnss_driver {
	const char *name;
	int (*probe)(struct ofono_gnss *gnss, unsigned int vendor, void *data);
	void (*remove)(struct ofono_gnss *gnss);
	void (*send_element)(struct ofono_gnss *gnss,
				const char *xml,
				ofono_gnss_cb_t cb, void *data);
	void (*set_position_reporting)(struct ofono_gnss *gnss,
					ofono_bool_t enable,
					ofono_gnss_cb_t cb,
					void *data);
};

void ofono_gnss_notify_posr_request(struct ofono_gnss *gnss, const char *xml);
void ofono_gnss_notify_posr_reset(struct ofono_gnss *gnss);
int ofono_gnss_driver_register(const struct ofono_gnss_driver *d);
void ofono_gnss_driver_unregister(const struct ofono_gnss_driver *d);

struct ofono_gnss *ofono_gnss_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_gnss_register(struct ofono_gnss *gnss);
void ofono_gnss_remove(struct ofono_gnss *gnss);

void ofono_gnss_set_data(struct ofono_gnss *gnss, void *data);
void *ofono_gnss_get_data(struct ofono_gnss *gnss);


#ifdef __cplusplus
}
#endif

#endif /* __OFONO_GNSS_H */
