/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2016  Canonical Ltd.
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

#ifndef OFONO_SYSTEM_SETTINGS_H
#define OFONO_SYSTEM_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Settings names */
#define PREFERRED_VOICE_MODEM "PreferredVoiceModem"

struct ofono_system_settings_driver {
	const char *name;
	/* The user must free the returned string */
	char *(*get_string_value)(const char *name);
};

/* The user must free the returned string */
char *__ofono_system_settings_get_string_value(const char *name);

int ofono_system_settings_driver_register(
			struct ofono_system_settings_driver *driver);

void ofono_system_settings_driver_unregister(
			const struct ofono_system_settings_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* OFONO_SYSTEM_SETTINGS_H */
