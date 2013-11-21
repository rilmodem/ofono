/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013  Jolla Ltd. All rights reserved.
 *  Copyright (C) 2013 Canonical Ltd.
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
#include <stdio.h>
#include <glib.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/history.h>
#include <ofono/types.h>

#include "ofono.h"
#include "common.h"

static const char msg_prefix[] = "/message_";

static int sms_history_probe(struct ofono_history_context *context)
{
	ofono_debug("SMS History Probe for modem: %p", context->modem);
	return 0;
}

static void sms_history_remove(struct ofono_history_context *context)
{
	ofono_debug("SMS History Remove for modem: %p", context->modem);
}

static void sms_history_sms_send_status(
					struct ofono_history_context *context,
					const struct ofono_uuid *uuid,
					time_t when,
					enum ofono_history_sms_status s)
{
	if ((s == OFONO_HISTORY_SMS_STATUS_DELIVERED)
			|| (s == OFONO_HISTORY_SMS_STATUS_DELIVER_FAILED)) {

		DBusMessage *signal;
		DBusMessageIter iter;
		DBusMessageIter dict;
		const char *uuid_str;
		char *msg_uuid_str;
		size_t msg_len;
		struct ofono_atom *atom;
		const char *path;
		DBusConnection *conn;
		int delivered;

		atom = __ofono_modem_find_atom(context->modem,
						OFONO_ATOM_TYPE_SMS);
		if (atom == NULL)
			return;

		path = __ofono_atom_get_path(atom);
		if (path == NULL)
			return;

		conn = ofono_dbus_get_connection();
		if (conn == NULL)
			return;

		delivered = (s == OFONO_HISTORY_SMS_STATUS_DELIVERED);

		uuid_str = ofono_uuid_to_str(uuid);
		/* sizeof adds extra space for one '\0' */
		msg_len = strlen(path) + sizeof(msg_prefix)
				+ strlen(uuid_str);

		msg_uuid_str = g_try_malloc(msg_len);
		if (msg_uuid_str == NULL)
			return;

		/* modem path + msg_prefix + UUID as string */
		snprintf(msg_uuid_str, msg_len, "%s%s%s", path,
				msg_prefix, uuid_str);

		DBG("SMS %s delivery success: %d", msg_uuid_str, delivered);

		signal = dbus_message_new_signal(path,
					OFONO_MESSAGE_MANAGER_INTERFACE,
					"StatusReport");
		if (signal != NULL) {
			dbus_message_iter_init_append(signal, &iter);
			dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
							&msg_uuid_str);

			dbus_message_iter_open_container(
					&iter,
					DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
			ofono_dbus_dict_append(&dict, "Delivered",
						DBUS_TYPE_BOOLEAN,
						&delivered);
			dbus_message_iter_close_container(&iter, &dict);

			g_dbus_send_message(conn, signal);
		}

		g_free(msg_uuid_str);
	}
}

static struct ofono_history_driver smshistory_driver = {
	.name = "SMS History",
	.probe = sms_history_probe,
	.remove = sms_history_remove,
	.sms_send_status = sms_history_sms_send_status,
};

static int sms_history_init(void)
{
	DBG("");
	return ofono_history_driver_register(&smshistory_driver);
}

static void sms_history_exit(void)
{
	DBG("");
	ofono_history_driver_unregister(&smshistory_driver);
}

OFONO_PLUGIN_DEFINE(smshistory, "SMS History Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			sms_history_init, sms_history_exit)
