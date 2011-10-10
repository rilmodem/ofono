/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __OFONO_CDMA_VOICECALL_H
#define __OFONO_CDMA_VOICECALL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_cdma_voicecall;

enum cdma_call_status {
	CDMA_CALL_STATUS_ACTIVE,
	CDMA_CALL_STATUS_DIALING,
	CDMA_CALL_STATUS_ALERTING,
	CDMA_CALL_STATUS_INCOMING,
	CDMA_CALL_STATUS_DISCONNECTED
};

typedef void (*ofono_cdma_voicecall_cb_t)(const struct ofono_error *error,
						void *data);

/* Voice call related functionality, including AT+CDV, AT+CHV */
struct ofono_cdma_voicecall_driver {
	const char *name;
	int (*probe)(struct ofono_cdma_voicecall *vc,
			unsigned int vendor, void *data);
	void (*remove)(struct ofono_cdma_voicecall *vc);

	void (*dial)(struct ofono_cdma_voicecall *vc,
			const struct ofono_cdma_phone_number *number,
			ofono_cdma_voicecall_cb_t cb, void *data);

	/* Hangs up active, dialing, alerting or incoming calls */
	void (*hangup)(struct ofono_cdma_voicecall *vc,
			ofono_cdma_voicecall_cb_t cb, void *data);

	void (*answer)(struct ofono_cdma_voicecall *vc,
			ofono_cdma_voicecall_cb_t cb, void *data);

	void (*send_flash)(struct ofono_cdma_voicecall *vc, const char *string,
			ofono_cdma_voicecall_cb_t cb, void *data);

	void (*send_tones)(struct ofono_cdma_voicecall *vc, const char *tones,
			ofono_cdma_voicecall_cb_t cb, void *data);
};

void ofono_cdma_voicecall_disconnected(struct ofono_cdma_voicecall *vc,
					enum ofono_disconnect_reason reason,
					const struct ofono_error *error);

int ofono_cdma_voicecall_driver_register(
			const struct ofono_cdma_voicecall_driver *d);
void ofono_cdma_voicecall_driver_unregister(
			const struct ofono_cdma_voicecall_driver *d);

struct ofono_cdma_voicecall *ofono_cdma_voicecall_create(
					struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_cdma_voicecall_register(struct ofono_cdma_voicecall *vc);
void ofono_cdma_voicecall_remove(struct ofono_cdma_voicecall *vc);

void ofono_cdma_voicecall_set_data(struct ofono_cdma_voicecall *vc,
					void *data);
void *ofono_cdma_voicecall_get_data(struct ofono_cdma_voicecall *vc);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CDMA_VOICECALL_H */
