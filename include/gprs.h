/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_DATA_CONNECTION_H
#define __OFONO_DATA_CONNECTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_data_connection;

typedef void (*ofono_data_connection_cb_t)(const struct ofono_error *error,
						void *data);

typedef void (*ofono_data_connection_alloc_cb_t)(
						const struct ofono_error *error,
						struct ofono_data_context *ctx,
						void *data);

struct ofono_data_connection_driver {
	const char *name;
	int (*probe)(struct ofono_data_connection *dc, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_data_connection *dc);
	void (*set_attached)(struct ofono_data_connection *dc,
				int attached, ofono_data_connection_cb_t cb,
				void *data);
	void (*set_active)(struct ofono_data_connection *dc, unsigned id,
				int active, ofono_data_connection_cb_t cb,
				void *data);
	void (*set_active_all)(struct ofono_data_connection *dc,
				int active, ofono_data_connection_cb_t cb,
				void *data);
	void (*create_context)(struct ofono_data_connection *dc,
				ofono_data_connection_alloc_cb_t cb,
				void *data);
	void (*remove_context)(struct ofono_data_connection *dc, unsigned id,
				ofono_data_connection_cb_t cb, void *data);
};

void ofono_data_connection_notify(struct ofono_data_connection *dc,
					struct ofono_data_context *ctx);
void ofono_data_connection_deactivated(struct ofono_data_connection *dc,
					unsigned id);
void ofono_data_connection_detached(struct ofono_data_connection *dc);
void ofono_data_netreg_status_notify(struct ofono_data_connection *dc,
					int status, int lac, int ci, int tech);

int ofono_data_connection_driver_register(
				const struct ofono_data_connection_driver *d);
void ofono_data_connection_driver_unregister(
				const struct ofono_data_connection_driver *d);

struct ofono_data_connection *ofono_data_connection_create(
		struct ofono_modem *modem, unsigned int vendor,
		const char *driver, void *data);
void ofono_data_connection_register(struct ofono_data_connection *dc);
void ofono_data_connection_remove(struct ofono_data_connection *dc);

void ofono_data_connection_set_data(struct ofono_data_connection *dc,
					void *data);
void *ofono_data_connection_get_data(struct ofono_data_connection *dc);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_DATA_CONNECTION_H */
