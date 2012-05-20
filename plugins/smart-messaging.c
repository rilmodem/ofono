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
#include "smsutil.h"
#include "common.h"

#define SMART_MESSAGING_INTERFACE "org.ofono.SmartMessaging"
#define AGENT_INTERFACE "org.ofono.SmartMessagingAgent"

#define VCARD_SRC_PORT -1
#define VCARD_DST_PORT 9204

#define VCAL_SRC_PORT -1
#define VCAL_DST_PORT 9205

static unsigned int modemwatch_id;

struct smart_messaging {
	struct ofono_modem *modem;
	struct ofono_sms *sms;
	struct sms_agent *agent;
	unsigned int vcard_watch;
	unsigned int vcal_watch;
};

static void agent_exited(void *userdata)
{
	struct smart_messaging *sm = userdata;

	if (sm->vcard_watch > 0) {
		__ofono_sms_datagram_watch_remove(sm->sms, sm->vcard_watch);
		sm->vcard_watch = 0;
	}

	if (sm->vcal_watch > 0) {
		__ofono_sms_datagram_watch_remove(sm->sms, sm->vcal_watch);
		sm->vcal_watch = 0;
	}

	sm->agent = NULL;
}

static void vcard_received(const char *from, const struct tm *remote,
				const struct tm *local, int dst, int src,
				const unsigned char *buffer,
				unsigned int len, void *data)
{
	struct smart_messaging *sm = data;

	if (sm->agent == NULL)
		return;

	sms_agent_dispatch_datagram(sm->agent, "ReceiveBusinessCard",
					from, remote, local, buffer, len,
					NULL, NULL, NULL);
}

static void vcal_received(const char *from, const struct tm *remote,
				const struct tm *local, int dst, int src,
				const unsigned char *buffer,
				unsigned int len, void *data)
{
	struct smart_messaging *sm = data;

	if (sm->agent == NULL)
		return;

	sms_agent_dispatch_datagram(sm->agent, "ReceiveAppointment",
					from, remote, local, buffer, len,
					NULL, NULL, NULL);
}

static DBusMessage *smart_messaging_register_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct smart_messaging *sm = data;
	const char *agent_path;

	if (sm->agent)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_dbus_valid_object_path(agent_path))
		return __ofono_error_invalid_format(msg);

	sm->agent = sms_agent_new(AGENT_INTERFACE,
					dbus_message_get_sender(msg),
					agent_path);

	if (sm->agent == NULL)
		return __ofono_error_failed(msg);

	sms_agent_set_removed_notify(sm->agent, agent_exited, sm);

	sm->vcard_watch = __ofono_sms_datagram_watch_add(sm->sms,
							vcard_received,
							VCARD_DST_PORT,
							VCARD_SRC_PORT,
							sm, NULL);

	sm->vcal_watch = __ofono_sms_datagram_watch_add(sm->sms,
							vcal_received,
							VCAL_DST_PORT,
							VCAL_SRC_PORT,
							sm, NULL);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *smart_messaging_unregister_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct smart_messaging *sm = data;
	const char *agent_path;
	const char *agent_bus = dbus_message_get_sender(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (sm->agent == NULL)
		return __ofono_error_failed(msg);

	if (sms_agent_matches(sm->agent, agent_bus, agent_path) == FALSE)
		return __ofono_error_failed(msg);

	sms_agent_free(sm->agent);
	sm->agent = NULL;

	return dbus_message_new_method_return(msg);
}

static void message_queued(struct ofono_sms *sms,
				const struct ofono_uuid *uuid, void *data)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *msg = data;
	const char *path;

	path = __ofono_sms_message_path_from_uuid(sms, uuid);
	g_dbus_send_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);
}

static DBusMessage *smart_messaging_send_vcard(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct smart_messaging *sm = data;
	const char *to;
	unsigned char *bytes;
	int len;
	GSList *msg_list;
	unsigned int flags;
	gboolean use_16bit_ref = FALSE;
	int err;
	struct ofono_uuid uuid;
	unsigned short ref;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &to,
					DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
					&bytes, &len, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (valid_phone_number_format(to) == FALSE)
		return __ofono_error_invalid_format(msg);

	ref = __ofono_sms_get_next_ref(sm->sms);
	msg_list = sms_datagram_prepare(to, bytes, len, ref, use_16bit_ref,
						0, VCARD_DST_PORT, TRUE, FALSE);

	if (msg_list == NULL)
		return __ofono_error_invalid_format(msg);

	flags = OFONO_SMS_SUBMIT_FLAG_RETRY | OFONO_SMS_SUBMIT_FLAG_EXPOSE_DBUS;

	err = __ofono_sms_txq_submit(sm->sms, msg_list, flags, &uuid,
					message_queued, msg);

	g_slist_foreach(msg_list, (GFunc)g_free, NULL);
	g_slist_free(msg_list);

	if (err < 0)
		return __ofono_error_failed(msg);

	return NULL;
}

static DBusMessage *smart_messaging_send_vcal(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct smart_messaging *sm = data;
	const char *to;
	unsigned char *bytes;
	int len;
	GSList *msg_list;
	unsigned int flags;
	gboolean use_16bit_ref = FALSE;
	int err;
	struct ofono_uuid uuid;
	unsigned short ref;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &to,
					DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
					&bytes, &len, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (valid_phone_number_format(to) == FALSE)
		return __ofono_error_invalid_format(msg);

	ref = __ofono_sms_get_next_ref(sm->sms);
	msg_list = sms_datagram_prepare(to, bytes, len, ref, use_16bit_ref,
						0, VCAL_DST_PORT, TRUE, FALSE);

	if (msg_list == NULL)
		return __ofono_error_invalid_format(msg);

	flags = OFONO_SMS_SUBMIT_FLAG_RETRY | OFONO_SMS_SUBMIT_FLAG_EXPOSE_DBUS;

	err = __ofono_sms_txq_submit(sm->sms, msg_list, flags, &uuid,
					message_queued, msg);

	g_slist_foreach(msg_list, (GFunc)g_free, NULL);
	g_slist_free(msg_list);

	if (err < 0)
		return __ofono_error_failed(msg);

	return NULL;
}

static const GDBusMethodTable smart_messaging_methods[] = {
	{ GDBUS_METHOD("RegisterAgent", GDBUS_ARGS({ "path", "o" }), NULL,
			smart_messaging_register_agent) },
	{ GDBUS_METHOD("UnregisterAgent", GDBUS_ARGS({ "path", "o" }), NULL,
			smart_messaging_unregister_agent) },
	{ GDBUS_ASYNC_METHOD("SendBusinessCard",
				GDBUS_ARGS({ "to", "s" }, { "card", "ay" }),
				GDBUS_ARGS({ "path", "o" }),
				smart_messaging_send_vcard) },
	{ GDBUS_ASYNC_METHOD("SendAppointment",
			GDBUS_ARGS({ "to", "s" }, { "appointment", "ay" }),
			GDBUS_ARGS({ "path", "o" }),
			smart_messaging_send_vcal) },
	{ }
};

static void smart_messaging_cleanup(gpointer user)
{
	struct smart_messaging *sm = user;

	DBG("%p", sm);

	sm->vcard_watch = 0;
	sm->vcal_watch = 0;
	sm->sms = NULL;

	sms_agent_free(sm->agent);

	ofono_modem_remove_interface(sm->modem, SMART_MESSAGING_INTERFACE);
}

static void sms_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct smart_messaging *sm = data;
	DBusConnection *conn = ofono_dbus_get_connection();

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		g_dbus_unregister_interface(conn,
					ofono_modem_get_path(sm->modem),
					SMART_MESSAGING_INTERFACE);

		return;
	}

	DBG("registered");
	sm->sms = __ofono_atom_get_data(atom);

	if (!g_dbus_register_interface(conn, ofono_modem_get_path(sm->modem),
					SMART_MESSAGING_INTERFACE,
					smart_messaging_methods, NULL, NULL,
					sm, smart_messaging_cleanup)) {
		ofono_error("Could not create %s interface",
				SMART_MESSAGING_INTERFACE);

		return;
	}

	ofono_modem_add_interface(sm->modem, SMART_MESSAGING_INTERFACE);
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	struct smart_messaging *sm;
	DBG("modem: %p, added: %d", modem, added);

	if (added == FALSE)
		return;

	sm = g_try_new0(struct smart_messaging, 1);
	if (sm == NULL)
		return;

	sm->modem = modem;
	__ofono_modem_add_atom_watch(modem, OFONO_ATOM_TYPE_SMS,
					sms_watch, sm, g_free);
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int smart_messaging_init(void)
{
	DBG("");

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);

	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void smart_messaging_exit(void)
{
	DBG("");

	__ofono_modemwatch_remove(modemwatch_id);
}

OFONO_PLUGIN_DEFINE(smart_messaging, "Smart Messaging Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			smart_messaging_init, smart_messaging_exit)
