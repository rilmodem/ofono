/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

	strftime(buf, 127, "%a, %d %b %Y %H:%M:%S %z", localtime(&start));
	buf[127] = '\0';
	ofono_debug("StartTime: %s", buf);

	strftime(buf, 127, "%a, %d %b %Y %H:%M:%S %z", localtime(&end));
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
	strftime(buf, 127, "%a, %d %b %Y %H:%M:%S %z", localtime(&when));
	buf[127] = '\0';
	ofono_debug("When: %s", buf);
}

static struct ofono_history_driver example_driver = {
	.name = "Example Call History",
	.probe = example_history_probe,
	.remove = example_history_remove,
	.call_ended = example_history_call_ended,
	.call_missed = example_history_call_missed,
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
