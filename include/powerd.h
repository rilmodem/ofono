/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Canonical Ltd. All rights reserved.
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

#ifndef OFONO_POWERD_H
#define OFONO_POWERD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_powerd;

typedef void (*ofono_powerd_cb_t)(const struct ofono_error *error, void *data);

struct ofono_powerd_driver {
	const char *name;
	int (*probe)(struct ofono_powerd *powerd,
			unsigned int vendor, void *data);
	void (*remove)(struct ofono_powerd *powerd);
	void (*set_display_state)(struct ofono_powerd *powerd, gboolean on,
					ofono_powerd_cb_t, void *data);
};

int ofono_powerd_driver_register(const struct ofono_powerd_driver *d);
void ofono_powerd_driver_unregister(const struct ofono_powerd_driver *d);

struct ofono_powerd *ofono_powerd_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_powerd_register(struct ofono_powerd *powerd);
void ofono_powerd_remove(struct ofono_powerd *powerd);

void ofono_powerd_set_data(struct ofono_powerd *powerd, void *data);
void *ofono_powerd_get_data(struct ofono_powerd *powerd);

#ifdef __cplusplus
}
#endif

#endif /* OFONO_POWERD_H */
