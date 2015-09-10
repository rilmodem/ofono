/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  BMW Car IT GmbH. All rights reserved.
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

#ifndef __OFONO_HANDSFREE_H
#define __OFONO_HANDSFREE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_handsfree;

typedef void (*ofono_handsfree_cb_t)(const struct ofono_error *error,
					void *data);
typedef void (*ofono_handsfree_phone_cb_t)(const struct ofono_error *error,
					const struct ofono_phone_number *number,
					void *data);
typedef void (*ofono_handsfree_cnum_query_cb_t)(const struct ofono_error *error,
				int total,
				const struct ofono_phone_number *numbers,
				void *data);

struct ofono_handsfree_driver {
	const char *name;
	int (*probe)(struct ofono_handsfree *hf, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_handsfree *hf);
	void (*cnum_query)(struct ofono_handsfree *hf,
				ofono_handsfree_cnum_query_cb_t cb, void *data);
	void (*request_phone_number) (struct ofono_handsfree *hf,
					ofono_handsfree_phone_cb_t cb,
					void *data);
	void (*voice_recognition)(struct ofono_handsfree *hf,
					ofono_bool_t enabled,
					ofono_handsfree_cb_t cb, void *data);
	void (*disable_nrec)(struct ofono_handsfree *hf,
					ofono_handsfree_cb_t cb, void *data);
	void (*hf_indicator)(struct ofono_handsfree *hf,
				unsigned short indicator, unsigned int value,
				ofono_handsfree_cb_t cb, void *data);
};

void ofono_handsfree_set_ag_features(struct ofono_handsfree *hf,
					unsigned int ag_features);
void ofono_handsfree_set_ag_chld_features(struct ofono_handsfree *hf,
					unsigned int ag_chld_features);
void ofono_handsfree_set_inband_ringing(struct ofono_handsfree *hf,
						ofono_bool_t enabled);
void ofono_handsfree_voice_recognition_notify(struct ofono_handsfree *hf,
						ofono_bool_t enabled);

void ofono_handsfree_set_hf_indicators(struct ofono_handsfree *hf,
					const unsigned short *indicators,
					unsigned int num);
void ofono_handsfree_hf_indicator_active_notify(struct ofono_handsfree *hf,
						unsigned int indicator,
						ofono_bool_t active);

void ofono_handsfree_battchg_notify(struct ofono_handsfree *hf,
					unsigned char level);

int ofono_handsfree_driver_register(const struct ofono_handsfree_driver *d);
void ofono_handsfree_driver_unregister(
			const struct ofono_handsfree_driver *d);

struct ofono_handsfree *ofono_handsfree_create(struct ofono_modem *modem,
			unsigned int vendor, const char *driver, void *data);

void ofono_handsfree_register(struct ofono_handsfree *hf);
void ofono_handsfree_remove(struct ofono_handsfree *hf);

void ofono_handsfree_set_data(struct ofono_handsfree *hf, void *data);
void *ofono_handsfree_get_data(struct ofono_handsfree *hf);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_HANDSFREE_H */
