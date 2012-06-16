/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __OFONO_CDMA_SMS_H
#define __OFONO_CDMA_SMS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_cdma_sms;

typedef void (*ofono_cdma_sms_submit_cb_t)(const struct ofono_error *error,
						void *data);

struct ofono_cdma_sms_driver {
	const char *name;
	int (*probe)(struct ofono_cdma_sms *cdma_sms, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_cdma_sms *cdma_sms);
	void (*submit)(struct ofono_cdma_sms *cdma_sms,
			const unsigned char *tpdu,
			int tpdu_len, ofono_cdma_sms_submit_cb_t cb,
			void *data);
};

void ofono_cdma_sms_deliver_notify(struct ofono_cdma_sms *cdma_sms,
					const unsigned char *pdu, int tpdu_len);

int ofono_cdma_sms_driver_register(const struct ofono_cdma_sms_driver *d);
void ofono_cdma_sms_driver_unregister(const struct ofono_cdma_sms_driver *d);

struct ofono_cdma_sms *ofono_cdma_sms_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data);

void ofono_cdma_sms_register(struct ofono_cdma_sms *cdma_sms);
void ofono_cdma_sms_remove(struct ofono_cdma_sms *cdma_sms);

void ofono_cdma_sms_set_data(struct ofono_cdma_sms *cdma_sms, void *data);
void *ofono_cdma_sms_get_data(struct ofono_cdma_sms *cdma_sms);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CDMA_SMS_H */
