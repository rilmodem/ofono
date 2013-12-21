/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2013  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_SIRI_H
#define __OFONO_SIRI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_siri;

typedef void (*ofono_siri_cb_t)(const struct ofono_error *error,
					struct ofono_siri *siri);

struct ofono_siri_driver {
	const char *name;
	int (*probe)(struct ofono_siri *siri, unsigned int vendor, void *data);
	void (*remove)(struct ofono_siri *siri);
	void (*set_eyes_free_mode) (struct ofono_siri *siri, ofono_siri_cb_t cb,
					unsigned int val);
};

void ofono_siri_set_status(struct ofono_siri *siri, int value);

int ofono_siri_driver_register(const struct ofono_siri_driver *driver);
void ofono_siri_driver_unregister(const struct ofono_siri_driver *driver);

struct ofono_siri *ofono_siri_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_siri_register(struct ofono_siri *siri);
void ofono_siri_remove(struct ofono_siri *siri);

void ofono_siri_set_data(struct ofono_siri *siri, void *data);
void *ofono_siri_get_data(struct ofono_siri *siri);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SIRI_H */
