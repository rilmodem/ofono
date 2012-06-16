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

#ifndef __OFONO_SMS_H
#define __OFONO_SMS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_sms;

typedef void (*ofono_sms_sca_query_cb_t)(const struct ofono_error *error,
					const struct ofono_phone_number *ph,
					void *data);
typedef void (*ofono_sms_submit_cb_t)(const struct ofono_error *error, int mr,
					void *data);
typedef void (*ofono_sms_sca_set_cb_t)(const struct ofono_error *error,
					void *data);
typedef void (*ofono_sms_bearer_set_cb_t)(const struct ofono_error *error,
						void *data);
typedef void (*ofono_sms_bearer_query_cb_t)(const struct ofono_error *error,
						int bearer, void *data);

struct ofono_sms_driver {
	const char *name;
	int (*probe)(struct ofono_sms *sms, unsigned int vendor, void *data);
	void (*remove)(struct ofono_sms *sms);
	void (*sca_query)(struct ofono_sms *sms, ofono_sms_sca_query_cb_t cb,
				void *data);
	void (*sca_set)(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *data);
	void (*submit)(struct ofono_sms *sms, const unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *data);
	void (*bearer_query)(struct ofono_sms *sms,
				ofono_sms_bearer_query_cb_t, void *data);
	void (*bearer_set)(struct ofono_sms *sms, int bearer,
				ofono_sms_bearer_set_cb_t, void *data);
};

void ofono_sms_deliver_notify(struct ofono_sms *sms, const unsigned char *pdu,
				int len, int tpdu_len);
void ofono_sms_status_notify(struct ofono_sms *sms, const unsigned char *pdu,
				int len, int tpdu_len);

int ofono_sms_driver_register(const struct ofono_sms_driver *d);
void ofono_sms_driver_unregister(const struct ofono_sms_driver *d);

struct ofono_sms *ofono_sms_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_sms_register(struct ofono_sms *sms);
void ofono_sms_remove(struct ofono_sms *sms);

void ofono_sms_set_data(struct ofono_sms *sms, void *data);
void *ofono_sms_get_data(struct ofono_sms *sms);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SMS_H */
