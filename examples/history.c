/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/history.h>
#include <ofono/types.h>

#include "common.h"

static int example_history_probe(struct ofono_history_context *context)
{
	ofono_debug("Example History Probe for modem: %p", context->modem);
	return 0;
}

static void example_history_remove(struct ofono_history_context *context)
{
	ofono_debug("Example History Remove for modem: %p", context->modem);
}

static void example_history_call_ended(struct ofono_history_context *context,
					const struct ofono_call *call,
					time_t start, time_t end)
{
	const char *from = "Unknown";
	char buf[128];

	ofono_debug("Call Ended on modem: %p", context->modem);

	if (call->type != 0)
		return;

	ofono_debug("Voice Call, %s",
			call->direction ? "Incoming" : "Outgoing");

	if (call->clip_validity == 0)
		from = phone_number_to_string(&call->phone_number);

	if (call->direction == 0)
		ofono_debug("To: %s", from);
	else
		ofono_debug("From: %s", from);

	if (call->cnap_validity == 0)
		ofono_debug("Name from Network: %s\n", call->name);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime(&start));
	buf[127] = '\0';
	ofono_debug("StartTime: %s", buf);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime(&end));
	buf[127] = '\0';
	ofono_debug("EndTime: %s", buf);
}

static void example_history_call_missed(struct ofono_history_context *context,
					const struct ofono_call *call,
					time_t when)
{
	const char *from = "Unknown";
	char buf[128];

	ofono_debug("Call Missed on modem: %p", context->modem);

	if (call->type != 0)
		return;

	ofono_debug("Voice Call, %s",
			call->direction ? "Incoming" : "Outgoing");

	if (call->clip_validity == 0)
		from = phone_number_to_string(&call->phone_number);

	ofono_debug("From: %s", from);

	if (call->cnap_validity == 0)
		ofono_debug("Name from Network: %s\n", call->name);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime(&when));
	buf[127] = '\0';
	ofono_debug("When: %s", buf);
}

static void example_history_sms_received(struct ofono_history_context *context,
						const struct ofono_uuid *uuid,
						const char *from,
						const struct tm *remote,
						const struct tm *local,
						const char *text)
{
	char buf[128];

	ofono_debug("Incoming SMS on modem: %p", context->modem);
	ofono_debug("InternalMessageId: %s", ofono_uuid_to_str(uuid));
	ofono_debug("From: %s", from);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", local);
	buf[127] = '\0';
	ofono_debug("Local Sent Time: %s", buf);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", remote);
	buf[127] = '\0';
	ofono_debug("Remote Sent Time: %s", buf);

	ofono_debug("Text: %s", text);
}

static void example_history_sms_send_pending(struct ofono_history_context *context,
						const struct ofono_uuid *uuid,
						const char *to, time_t when,
						const char *text)
{
	char buf[128];

	ofono_debug("Sending SMS on modem: %p", context->modem);
	ofono_debug("InternalMessageId: %s", ofono_uuid_to_str(uuid));
	ofono_debug("To: %s:", to);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime(&when));
	buf[127] = '\0';
	ofono_debug("Local Time: %s", buf);
	ofono_debug("Text: %s", text);
}

static void example_history_sms_send_status(
					struct ofono_history_context *context,
					const struct ofono_uuid *uuid,
					time_t when,
					enum ofono_history_sms_status s)
{
	char buf[128];

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime(&when));
	buf[127] = '\0';

	switch (s) {
	case OFONO_HISTORY_SMS_STATUS_PENDING:
		break;
	case OFONO_HISTORY_SMS_STATUS_SUBMITTED:
		ofono_debug("SMS %s submitted successfully",
					ofono_uuid_to_str(uuid));
		ofono_debug("Submission Time: %s", buf);
		break;
	case OFONO_HISTORY_SMS_STATUS_SUBMIT_FAILED:
		ofono_debug("Sending SMS %s failed", ofono_uuid_to_str(uuid));
		ofono_debug("Failure Time: %s", buf);
		break;
	case OFONO_HISTORY_SMS_STATUS_SUBMIT_CANCELLED:
		ofono_debug("Submission of SMS %s was canceled",
					ofono_uuid_to_str(uuid));
		ofono_debug("Cancel time: %s", buf);
		break;
	case OFONO_HISTORY_SMS_STATUS_DELIVERED:
		ofono_debug("SMS delivered, msg_id: %s, time: %s",
					ofono_uuid_to_str(uuid), buf);
		break;
	case OFONO_HISTORY_SMS_STATUS_DELIVER_FAILED:
		ofono_debug("SMS undeliverable, msg_id: %s, time: %s",
					ofono_uuid_to_str(uuid), buf);
		break;
	default:
		break;
	}
}

static struct ofono_history_driver example_driver = {
	.name = "Example Call History",
	.probe = example_history_probe,
	.remove = example_history_remove,
	.call_ended = example_history_call_ended,
	.call_missed = example_history_call_missed,
	.sms_received = example_history_sms_received,
	.sms_send_pending = example_history_sms_send_pending,
	.sms_send_status = example_history_sms_send_status,
};

static int example_history_init(void)
{
	return ofono_history_driver_register(&example_driver);
}

static void example_history_exit(void)
{
	ofono_history_driver_unregister(&example_driver);
}

OFONO_PLUGIN_DEFINE(example_history, "Example Call History Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			example_history_init, example_history_exit)
