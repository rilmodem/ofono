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

#ifndef __OFONO_AUDIO_SETTINGS_H
#define __OFONO_AUDIO_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_audio_settings;

struct ofono_audio_settings_driver {
	const char *name;
	int (*probe)(struct ofono_audio_settings *as,
				unsigned int vendor, void *data);
	void (*remove)(struct ofono_audio_settings *as);
};

void ofono_audio_settings_active_notify(struct ofono_audio_settings *as,
						ofono_bool_t active);
void ofono_audio_settings_mode_notify(struct ofono_audio_settings *as,
						const char *mode);

int ofono_audio_settings_driver_register(const struct ofono_audio_settings_driver *d);
void ofono_audio_settings_driver_unregister(const struct ofono_audio_settings_driver *d);

struct ofono_audio_settings *ofono_audio_settings_create(struct ofono_modem *modem,
			unsigned int vendor, const char *driver, void *data);

void ofono_audio_settings_register(struct ofono_audio_settings *as);
void ofono_audio_settings_remove(struct ofono_audio_settings *as);

void ofono_audio_settings_set_data(struct ofono_audio_settings *as, void *data);
void *ofono_audio_settings_get_data(struct ofono_audio_settings *as);

struct ofono_modem *ofono_audio_settings_get_modem(struct ofono_audio_settings *as);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_AUDIO_SETTINGS_H */
