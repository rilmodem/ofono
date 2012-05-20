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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <glib.h>
#include <gdbus.h>
#include <ofono.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/dbus.h>

#include "smsagent.h"

#define PUSH_NOTIFICATION_INTERFACE "org.ofono.PushNotification"
#define AGENT_INTERFACE "org.ofono.PushNotificationAgent"

#define WAP_PUSH_SRC_PORT 9200
#define WAP_PUSH_DST_PORT 2948

static unsigned int modemwatch_id;

struct push_notification {
	struct ofono_modem *modem;
	struct ofono_sms *sms;
	struct sms_agent *agent;
	unsigned int push_watch[2];
};

static void agent_exited(void *userdata)
{
	struct push_notification *pn = userdata;

	if (pn->push_watch[0] > 0) {
		__ofono_sms_datagram_watch_remove(pn->sms, pn->push_watch[0]);
		pn->push_watch[0] = 0;
	}

	if (pn->push_watch[1] > 0) {
		__ofono_sms_datagram_watch_remove(pn->sms, pn->push_watch[1]);
		pn->push_watch[1] = 0;
	}

	pn->agent = NULL;
}

static void push_received(const char *from, const struct tm *remote,
				const struct tm *local, int dst, int src,
				const unsigned char *buffer,
				unsigned int len, void *data)
{
	struct push_notification *pn = data;

	DBG("Received push of size: %u", len);

	if (pn->agent == NULL)
		return;

	sms_agent_dispatch_datagram(pn->agent, "ReceiveNotification",
					from, remote, local, buffer, len,
					NULL, NULL, NULL);
}

static DBusMessage *push_notification_register_agent(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct push_notification *pn = data;
	const char *agent_path;

	if (pn->agent)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_dbus_valid_object_path(agent_path))
		return __ofono_error_invalid_format(msg);

	pn->agent = sms_agent_new(AGENT_INTERFACE,
					dbus_message_get_sender(msg),
					agent_path);

	if (pn->agent == NULL)
		return __ofono_error_failed(msg);

	sms_agent_set_removed_notify(pn->agent, agent_exited, pn);

	pn->push_watch[0] = __ofono_sms_datagram_watch_add(pn->sms,
							push_received,
							WAP_PUSH_DST_PORT,
							WAP_PUSH_SRC_PORT,
							pn, NULL);

	pn->push_watch[1] = __ofono_sms_datagram_watch_add(pn->sms,
							push_received,
							WAP_PUSH_DST_PORT,
							0, pn, NULL);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *push_notification_unregister_agent(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct push_notification *pn = data;
	const char *agent_path;
	const char *agent_bus = dbus_message_get_sender(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (pn->agent == NULL)
		return __ofono_error_failed(msg);

	if (sms_agent_matches(pn->agent, agent_bus, agent_path) == FALSE)
		return __ofono_error_failed(msg);

	sms_agent_free(pn->agent);
	pn->agent = NULL;

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable push_notification_methods[] = {
	{ GDBUS_METHOD("RegisterAgent",	GDBUS_ARGS({ "path", "o" }), NULL,
			push_notification_register_agent) },
	{ GDBUS_METHOD("UnregisterAgent", GDBUS_ARGS({ "path", "o" }), NULL,
			push_notification_unregister_agent) },
	{ }
};

static void push_notification_cleanup(gpointer user)
{
	struct push_notification *pn = user;

	DBG("%p", pn);

	/* The push watch was already cleaned up */
	pn->push_watch[0] = 0;
	pn->push_watch[1] = 0;
	pn->sms = NULL;

	sms_agent_free(pn->agent);

	ofono_modem_remove_interface(pn->modem, PUSH_NOTIFICATION_INTERFACE);
}

static void sms_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct push_notification *pn = data;
	DBusConnection *conn = ofono_dbus_get_connection();

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		g_dbus_unregister_interface(conn,
					ofono_modem_get_path(pn->modem),
					PUSH_NOTIFICATION_INTERFACE);
		return;
	}

	DBG("registered");
	pn->sms = __ofono_atom_get_data(atom);

	if (!g_dbus_register_interface(conn, ofono_modem_get_path(pn->modem),
					PUSH_NOTIFICATION_INTERFACE,
					push_notification_methods, NULL, NULL,
					pn, push_notification_cleanup)) {
		ofono_error("Could not create %s interface",
				PUSH_NOTIFICATION_INTERFACE);

		return;
	}

	ofono_modem_add_interface(pn->modem, PUSH_NOTIFICATION_INTERFACE);
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	struct push_notification *pn;
	DBG("modem: %p, added: %d", modem, added);

	if (added == FALSE)
		return;

	pn = g_try_new0(struct push_notification, 1);
	if (pn == NULL)
		return;

	pn->modem = modem;
	__ofono_modem_add_atom_watch(modem, OFONO_ATOM_TYPE_SMS,
					sms_watch, pn, g_free);
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int push_notification_init(void)
{
	DBG("");

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);

	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void push_notification_exit(void)
{
	DBG("");

	__ofono_modemwatch_remove(modemwatch_id);
}

OFONO_PLUGIN_DEFINE(push_notification, "Push Notification Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			push_notification_init, push_notification_exit)
