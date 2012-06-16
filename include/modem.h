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

#ifndef __OFONO_MODEM_H
#define __OFONO_MODEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_modem;

enum ofono_modem_type {
	OFONO_MODEM_TYPE_HARDWARE = 0,
	OFONO_MODEM_TYPE_HFP,
	OFONO_MODEM_TYPE_SAP,
};

typedef void (*ofono_modem_online_cb_t)(const struct ofono_error *error,
					void *data);

struct ofono_modem_driver {
	const char *name;
	enum ofono_modem_type modem_type;

	/* Detect existence of device and initialize any device-specific data
	 * structures */
	int (*probe)(struct ofono_modem *modem);

	/* Destroy data structures allocated during probe and cleanup */
	void (*remove)(struct ofono_modem *modem);

	/* Power up device */
	int (*enable)(struct ofono_modem *modem);

	/* Power down device */
	int (*disable)(struct ofono_modem *modem);

	/* Enable or disable cellular radio */
	void (*set_online)(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t callback, void *data);

	/* Populate the atoms available without SIM / Locked SIM */
	void (*pre_sim)(struct ofono_modem *modem);

	/* Populate the atoms that are available with SIM / Unlocked SIM*/
	void (*post_sim)(struct ofono_modem *modem);

	/* Populate the atoms available online */
	void (*post_online)(struct ofono_modem *modem);
};

void ofono_modem_add_interface(struct ofono_modem *modem,
				const char *interface);
void ofono_modem_remove_interface(struct ofono_modem *modem,
					const char *interface);

const char *ofono_modem_get_path(struct ofono_modem *modem);

void ofono_modem_set_data(struct ofono_modem *modem, void *data);
void *ofono_modem_get_data(struct ofono_modem *modem);

struct ofono_modem *ofono_modem_create(const char *name, const char *type);
int ofono_modem_register(struct ofono_modem *modem);

ofono_bool_t ofono_modem_is_registered(struct ofono_modem *modem);
void ofono_modem_remove(struct ofono_modem *modem);

void ofono_modem_reset(struct ofono_modem *modem);

void ofono_modem_set_powered(struct ofono_modem *modem, ofono_bool_t powered);
ofono_bool_t ofono_modem_get_powered(struct ofono_modem *modem);

ofono_bool_t ofono_modem_get_online(struct ofono_modem *modem);

ofono_bool_t ofono_modem_get_emergency_mode(struct ofono_modem *modem);

void ofono_modem_set_name(struct ofono_modem *modem, const char *name);
void ofono_modem_set_driver(struct ofono_modem *modem, const char *type);

int ofono_modem_set_string(struct ofono_modem *modem,
				const char *key, const char *value);
const char *ofono_modem_get_string(struct ofono_modem *modem, const char *key);

int ofono_modem_set_integer(struct ofono_modem *modem,
				const char *key, int value);
int ofono_modem_get_integer(struct ofono_modem *modem, const char *key);

int ofono_modem_set_boolean(struct ofono_modem *modem,
				const char *key, ofono_bool_t value);
ofono_bool_t ofono_modem_get_boolean(struct ofono_modem *modem,
					const char *key);

int ofono_modem_driver_register(const struct ofono_modem_driver *);
void ofono_modem_driver_unregister(const struct ofono_modem_driver *);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_MODEM_H */
