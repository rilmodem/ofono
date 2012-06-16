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

#ifndef __OFONO_CALL_METER_H
#define __OFONO_CALL_METER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_call_meter;

typedef void (*ofono_call_meter_query_cb_t)(const struct ofono_error *error,
						int value, void *data);

typedef void (*ofono_call_meter_puct_query_cb_t)(
					const struct ofono_error *error,
					const char *currency, double ppu,
					void *data);

typedef void(*ofono_call_meter_set_cb_t)(const struct ofono_error *error,
						void *data);

struct ofono_call_meter_driver {
	const char *name;
	int (*probe)(struct ofono_call_meter *cm, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_call_meter *cm);
	void (*call_meter_query)(struct ofono_call_meter *cm,
				ofono_call_meter_query_cb_t cb, void *data);
	void (*acm_query)(struct ofono_call_meter *cm,
				ofono_call_meter_query_cb_t cb, void *data);
	void (*acm_reset)(struct ofono_call_meter *cm, const char *sim_pin2,
				ofono_call_meter_set_cb_t cb, void *data);
	void (*acm_max_query)(struct ofono_call_meter *cm,
				ofono_call_meter_query_cb_t cb, void *data);
	void (*acm_max_set)(struct ofono_call_meter *cm, int new_value,
				const char *sim_pin2,
				ofono_call_meter_set_cb_t cb, void *data);
	void (*puct_query)(struct ofono_call_meter *cm,
			ofono_call_meter_puct_query_cb_t cb, void *data);
	void (*puct_set)(struct ofono_call_meter *cm, const char *currency,
				double ppu, const char *sim_pin2,
				ofono_call_meter_set_cb_t cb, void *data);
};

int ofono_call_meter_driver_register(const struct ofono_call_meter_driver *d);
void ofono_call_meter_driver_unregister(
				const struct ofono_call_meter_driver *d);

struct ofono_call_meter *ofono_call_meter_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data);

void ofono_call_meter_register(struct ofono_call_meter *cm);
void ofono_call_meter_remove(struct ofono_call_meter *cm);

void ofono_call_meter_maximum_notify(struct ofono_call_meter *cm);
void ofono_call_meter_changed_notify(struct ofono_call_meter *cm,
					int new_value);

void ofono_call_meter_set_data(struct ofono_call_meter *cm, void *data);
void *ofono_call_meter_get_data(struct ofono_call_meter *cm);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CALL_METER_H */
