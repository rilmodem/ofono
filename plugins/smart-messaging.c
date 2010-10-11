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

#define SMART_MESSAGING_INTERFACE "org.ofono.SmartMessaging"

static unsigned int modemwatch_id;

struct smart_messaging {
	struct ofono_modem *modem;
	struct ofono_sms *sms;
};

static DBusMessage *smart_messaging_register_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return __ofono_error_not_implemented(msg);
}

static DBusMessage *smart_messaging_unregister_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return __ofono_error_not_implemented(msg);
}

static DBusMessage *smart_messaging_send_vcard(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return __ofono_error_not_implemented(msg);
}

static DBusMessage *smart_messaging_send_vcal(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return __ofono_error_not_implemented(msg);
}

static GDBusMethodTable smart_messaging_methods[] = {
	{ "RegisterAgent",    "o",     "",  smart_messaging_register_agent },
	{ "UnregisterAgent",  "o",     "",  smart_messaging_unregister_agent },
	{ "SendBusinessCard", "say",   "o", smart_messaging_send_vcard,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "SendAppointment",  "say",   "o", smart_messaging_send_vcal,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static void smart_messaging_cleanup(gpointer user)
{
	struct smart_messaging *sm = user;

	DBG("%p", sm);
}

static void sms_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct smart_messaging *sm = data;
	DBusConnection *conn = ofono_dbus_get_connection();

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG("unregistered");
		sm->sms = NULL;

		g_dbus_unregister_interface(conn,
					ofono_modem_get_path(sm->modem),
					SMART_MESSAGING_INTERFACE);

		ofono_modem_remove_interface(sm->modem,
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

static int smart_messaging_init()
{
	DBG("");

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);

	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void smart_messaging_exit()
{
	DBG("");

	__ofono_modemwatch_remove(modemwatch_id);
}

OFONO_PLUGIN_DEFINE(smart_messaging, "Smart Messaging Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			smart_messaging_init, smart_messaging_exit)
