/*
 *
 *  oFono - Open Telephony stack for Linux
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

#ifndef __OFONO_HISTORY_H
#define __OFONO_HISTORY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

enum ofono_disconnect_reason;
struct ofono_call;

enum ofono_history_sms_status {
	OFONO_HISTORY_SMS_STATUS_PENDING,
	OFONO_HISTORY_SMS_STATUS_SUBMITTED,
	OFONO_HISTORY_SMS_STATUS_SUBMIT_FAILED,
	OFONO_HISTORY_SMS_STATUS_SUBMIT_CANCELLED,
	OFONO_HISTORY_SMS_STATUS_DELIVERED,
	OFONO_HISTORY_SMS_STATUS_DELIVER_FAILED,
};

struct ofono_history_context {
	struct ofono_history_driver *driver;
	struct ofono_modem *modem;
	void *data;
};

struct ofono_history_driver {
	const char *name;
	int (*probe)(struct ofono_history_context *context);
	void (*remove)(struct ofono_history_context *context);
	void (*call_ended)(struct ofono_history_context *context,
				const struct ofono_call *call,
				time_t start, time_t end);
	void (*call_missed)(struct ofono_history_context *context,
				const struct ofono_call *call, time_t when);
	void (*sms_received)(struct ofono_history_context *context,
				const struct ofono_uuid *uuid, const char *from,
				const struct tm *remote, const struct tm *local,
				const char *text);
	void (*sms_send_pending)(struct ofono_history_context *context,
					const struct ofono_uuid *uuid,
					const char *to,
					time_t when, const char *text);
	void (*sms_send_status)(struct ofono_history_context *context,
					const struct ofono_uuid *uuid,
					time_t when,
					enum ofono_history_sms_status status);
};

int ofono_history_driver_register(const struct ofono_history_driver *driver);
void ofono_history_driver_unregister(const struct ofono_history_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_HISTORY_H */
