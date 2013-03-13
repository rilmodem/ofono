/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_HANDSFREE_AUDIO_H
#define __OFONO_HANDSFREE_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_handsfree_card;

typedef void (*ofono_handsfree_card_connect_cb_t)(
				const struct ofono_error *error, void *data);

struct ofono_handsfree_card_driver {
	const char *name;
	int (*probe)(struct ofono_handsfree_card *card, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_handsfree_card *card);
	int (*connect)(struct ofono_handsfree_card *card,
				ofono_handsfree_card_connect_cb_t cb,
				void *data);
};

struct ofono_handsfree_card *ofono_handsfree_card_create(const char *remote,
							const char *local);
int ofono_handsfree_card_register(struct ofono_handsfree_card *card);
void ofono_handsfree_card_remove(struct ofono_handsfree_card *card);

void ofono_handsfree_card_set_data(struct ofono_handsfree_card *card,
					void *data);
void *ofono_handsfree_card_get_data(struct ofono_handsfree_card *card);

void ofono_handsfree_audio_ref(void);
void ofono_handsfree_audio_unref(void);

int ofono_handsfree_card_driver_register(
				const struct ofono_handsfree_card_driver *d);
void ofono_handsfree_card_driver_unregister(
				const struct ofono_handsfree_card_driver *d);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_HANDSFREE_AUDIO_H */
