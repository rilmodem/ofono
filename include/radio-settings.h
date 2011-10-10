/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __OFONO_RADIO_SETTINGS_H
#define __OFONO_RADIO_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

enum ofono_radio_access_mode {
	OFONO_RADIO_ACCESS_MODE_ANY	= 0,
	OFONO_RADIO_ACCESS_MODE_GSM	= 1,
	OFONO_RADIO_ACCESS_MODE_UMTS	= 2,
	OFONO_RADIO_ACCESS_MODE_LTE	= 3,
};

enum ofono_radio_band_gsm {
	OFONO_RADIO_BAND_GSM_ANY,
	OFONO_RADIO_BAND_GSM_850,
	OFONO_RADIO_BAND_GSM_900P,
	OFONO_RADIO_BAND_GSM_900E,
	OFONO_RADIO_BAND_GSM_1800,
	OFONO_RADIO_BAND_GSM_1900,
};

enum ofono_radio_band_umts {
	OFONO_RADIO_BAND_UMTS_ANY,
	OFONO_RADIO_BAND_UMTS_850,
	OFONO_RADIO_BAND_UMTS_900,
	OFONO_RADIO_BAND_UMTS_1700AWS,
	OFONO_RADIO_BAND_UMTS_1900,
	OFONO_RADIO_BAND_UMTS_2100,
};

struct ofono_radio_settings;

typedef void (*ofono_radio_settings_rat_mode_set_cb_t)(const struct ofono_error *error,
							void *data);
typedef void (*ofono_radio_settings_rat_mode_query_cb_t)(const struct ofono_error *error,
					enum ofono_radio_access_mode mode,
					void *data);

typedef void (*ofono_radio_settings_band_set_cb_t)(const struct ofono_error *error,
							void *data);
typedef void (*ofono_radio_settings_band_query_cb_t)(const struct ofono_error *error,
					enum ofono_radio_band_gsm band_gsm,
					enum ofono_radio_band_umts band_umts,
					void *data);

typedef void (*ofono_radio_settings_fast_dormancy_set_cb_t)(const struct ofono_error *error,
							void *data);
typedef void (*ofono_radio_settings_fast_dormancy_query_cb_t)(const struct ofono_error *error,
							ofono_bool_t enable,
							void *data);

struct ofono_radio_settings_driver {
	const char *name;
	int (*probe)(struct ofono_radio_settings *rs, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_radio_settings *rs);
	void (*query_rat_mode)(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data);
	void (*set_rat_mode)(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data);
	void (*query_band)(struct ofono_radio_settings *rs,
				ofono_radio_settings_band_query_cb_t cb,
				void *data);
	void (*set_band)(struct ofono_radio_settings *rs,
				enum ofono_radio_band_gsm band_gsm,
				enum ofono_radio_band_umts band_umts,
				ofono_radio_settings_band_set_cb_t cb,
				void *data);
	void (*query_fast_dormancy)(struct ofono_radio_settings *rs,
			ofono_radio_settings_fast_dormancy_query_cb_t cb,
			void *data);
	void (*set_fast_dormancy)(struct ofono_radio_settings *rs,
				ofono_bool_t enable,
				ofono_radio_settings_fast_dormancy_set_cb_t,
				void *data);
};

int ofono_radio_settings_driver_register(const struct ofono_radio_settings_driver *d);
void ofono_radio_settings_driver_unregister(const struct ofono_radio_settings_driver *d);

struct ofono_radio_settings *ofono_radio_settings_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data);

void ofono_radio_settings_register(struct ofono_radio_settings *rs);
void ofono_radio_settings_remove(struct ofono_radio_settings *rs);

void ofono_radio_settings_set_data(struct ofono_radio_settings *rs, void *data);
void *ofono_radio_settings_get_data(struct ofono_radio_settings *rs);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_RADIO_SETTINGS_H */
