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

#ifndef __OFONO_CBS_H
#define __OFONO_CBS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_cbs;

typedef void (*ofono_cbs_set_cb_t)(const struct ofono_error *error,
					void *data);

struct ofono_cbs_driver {
	const char *name;
	int (*probe)(struct ofono_cbs *cbs, unsigned int vendor, void *data);
	void (*remove)(struct ofono_cbs *cbs);
	void (*set_topics)(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *data);
	void (*clear_topics)(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *data);
};

void ofono_cbs_notify(struct ofono_cbs *cbs, const unsigned char *pdu, int len);

int ofono_cbs_driver_register(const struct ofono_cbs_driver *d);
void ofono_cbs_driver_unregister(const struct ofono_cbs_driver *d);

struct ofono_cbs *ofono_cbs_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_cbs_register(struct ofono_cbs *cbs);
void ofono_cbs_remove(struct ofono_cbs *cbs);

void ofono_cbs_set_data(struct ofono_cbs *cbs, void *data);
void *ofono_cbs_get_data(struct ofono_cbs *cbs);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CBS_H */
