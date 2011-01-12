/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_CALL_SETTINGS_H
#define __OFONO_CALL_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_call_settings;

typedef void (*ofono_call_settings_status_cb_t)(const struct ofono_error *error,
						int status, void *data);

typedef void (*ofono_call_settings_set_cb_t)(const struct ofono_error *error,
						void *data);

typedef void (*ofono_call_settings_clir_cb_t)(const struct ofono_error *error,
					int override, int network, void *data);

struct ofono_call_settings_driver {
	const char *name;
	int (*probe)(struct ofono_call_settings *cs, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_call_settings *cs);
	void (*clip_query)(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data);
	void (*cnap_query)(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data);
	void (*cdip_query)(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data);
	void (*colp_query)(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data);
	void (*clir_query)(struct ofono_call_settings *cs,
				ofono_call_settings_clir_cb_t cb, void *data);
	void (*colr_query)(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data);
	void (*clir_set)(struct ofono_call_settings *cs, int mode,
				ofono_call_settings_set_cb_t cb, void *data);
	void (*cw_query)(struct ofono_call_settings *cs, int cls,
			ofono_call_settings_status_cb_t cb, void *data);
	void (*cw_set)(struct ofono_call_settings *cs, int mode, int cls,
			ofono_call_settings_set_cb_t cb, void *data);
};

int ofono_call_settings_driver_register(const struct ofono_call_settings_driver *d);
void ofono_call_settings_driver_unregister(const struct ofono_call_settings_driver *d);

struct ofono_call_settings *ofono_call_settings_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data);

void ofono_call_settings_register(struct ofono_call_settings *cs);
void ofono_call_settings_remove(struct ofono_call_settings *cs);

void ofono_call_settings_set_data(struct ofono_call_settings *cs, void *data);
void *ofono_call_settings_get_data(struct ofono_call_settings *cs);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CALL_SETTINGS_H */
