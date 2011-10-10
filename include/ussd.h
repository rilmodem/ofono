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

#ifndef __OFONO_USSD_H
#define __OFONO_USSD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

/* 3GPP TS 27.007 section 7.15, values for <m> */
enum ofono_ussd_status {
	OFONO_USSD_STATUS_NOTIFY = 0,
	OFONO_USSD_STATUS_ACTION_REQUIRED = 1,
	OFONO_USSD_STATUS_TERMINATED = 2,
	OFONO_USSD_STATUS_LOCAL_CLIENT_RESPONDED = 3,
	OFONO_USSD_STATUS_NOT_SUPPORTED = 4,
	OFONO_USSD_STATUS_TIMED_OUT = 5,
};

struct ofono_ussd;

typedef void (*ofono_ussd_cb_t)(const struct ofono_error *error, void *data);

struct ofono_ussd_driver {
	const char *name;
	int (*probe)(struct ofono_ussd *ussd, unsigned int vendor, void *data);
	void (*remove)(struct ofono_ussd *ussd);
	void (*request)(struct ofono_ussd *ussd, int dcs,
			const unsigned char *pdu, int len,
			ofono_ussd_cb_t, void *data);
	void (*cancel)(struct ofono_ussd *ussd,
				ofono_ussd_cb_t cb, void *data);
};

void ofono_ussd_notify(struct ofono_ussd *ussd, int status, int dcs,
			const unsigned char *data, int data_len);

int ofono_ussd_driver_register(const struct ofono_ussd_driver *d);
void ofono_ussd_driver_unregister(const struct ofono_ussd_driver *d);

struct ofono_ussd *ofono_ussd_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_ussd_register(struct ofono_ussd *ussd);
void ofono_ussd_remove(struct ofono_ussd *ussd);

void ofono_ussd_set_data(struct ofono_ussd *ussd, void *data);
void *ofono_ussd_get_data(struct ofono_ussd *ussd);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_USSD_H */
