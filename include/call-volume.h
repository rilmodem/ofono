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

#ifndef __OFONO_CALL_VOLUME_H
#define __OFONO_CALL_VOLUME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>
#include <ofono/dbus.h>

struct ofono_call_volume;

typedef void (*ofono_call_volume_cb_t)(const struct ofono_error *error,
					void *data);

struct ofono_call_volume_driver {
	const char *name;
	int (*probe)(struct ofono_call_volume *cv, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_call_volume *cv);
	void (*speaker_volume)(struct ofono_call_volume *cv,
				unsigned char percent,
				ofono_call_volume_cb_t cb, void *data);
	void (*microphone_volume)(struct ofono_call_volume *cv,
					unsigned char percent,
					ofono_call_volume_cb_t cb, void *data);
	void (*mute)(struct ofono_call_volume *cv, int muted,
			ofono_call_volume_cb_t cb, void *data);
};

void ofono_call_volume_set_speaker_volume(struct ofono_call_volume *cv,
						unsigned char percent);
void ofono_call_volume_set_microphone_volume(struct ofono_call_volume *cv,
						unsigned char percent);
void ofono_call_volume_set_muted(struct ofono_call_volume *cv, int muted);

int ofono_call_volume_driver_register(const struct ofono_call_volume_driver *d);
void ofono_call_volume_driver_unregister(
			const struct ofono_call_volume_driver *d);

struct ofono_call_volume *ofono_call_volume_create(struct ofono_modem *modem,
			unsigned int vendor, const char *driver, void *data);

void ofono_call_volume_register(struct ofono_call_volume *cv);
void ofono_call_volume_remove(struct ofono_call_volume *cv);

void ofono_call_volume_set_data(struct ofono_call_volume *cv, void *data);
void *ofono_call_volume_get_data(struct ofono_call_volume *cv);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CALL_VOLUME_H */
