/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_CTM_H
#define __OFONO_CTM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_ctm;

typedef void (*ofono_ctm_set_cb_t)(const struct ofono_error *error,
					void *data);
typedef void (*ofono_ctm_query_cb_t)(const struct ofono_error *error,
					ofono_bool_t enable, void *data);

struct ofono_ctm_driver {
	const char *name;
	int (*probe)(struct ofono_ctm *ctm, unsigned int vendor, void *data);
	void (*remove)(struct ofono_ctm *ctm);
	void (*query_tty)(struct ofono_ctm *ctm,
				ofono_ctm_query_cb_t cb, void *data);
	void (*set_tty)(struct ofono_ctm *ctm, ofono_bool_t enable,
				ofono_ctm_set_cb_t cb, void *data);
};

int ofono_ctm_driver_register(const struct ofono_ctm_driver *d);
void ofono_ctm_driver_unregister(const struct ofono_ctm_driver *d);

struct ofono_ctm *ofono_ctm_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_ctm_register(struct ofono_ctm *ctm);
void ofono_ctm_remove(struct ofono_ctm *ctm);

void ofono_ctm_set_data(struct ofono_ctm *ctm, void *data);
void *ofono_ctm_get_data(struct ofono_ctm *ctm);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CTM_H */
