/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/dbus.h>
#include <ofono/plugin.h>
#include <ofono/log.h>

#include <gdbus/gdbus.h>
#include "bluez5.h"

#define BLUEZ_PROFILE_MGMT_INTERFACE   BLUEZ_SERVICE ".ProfileManager1"

struct finish_callback {
	bt_finish_cb cb;
	gpointer user_data;
	char *member;
};

static void profile_register_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;
	DBusError derr;

	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&derr);

	if (dbus_set_error_from_message(&derr, reply)) {
		ofono_error("RegisterProfile() replied an error: %s, %s",
						derr.name, derr.message);
		dbus_error_free(&derr);
		goto done;
	}

	DBG("");

done:
	dbus_message_unref(reply);
}

static void unregister_profile_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;
	DBusError derr;

	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&derr);

	if (dbus_set_error_from_message(&derr, reply)) {
		ofono_error("UnregisterProfile() replied an error: %s, %s",
						derr.name, derr.message);
		dbus_error_free(&derr);
		goto done;
	}

	DBG("");

done:
	dbus_message_unref(reply);
}

int bt_register_profile(DBusConnection *conn, const char *uuid,
					uint16_t version, const char *name,
					const char *object, const char *role,
					uint16_t features)
{
	DBusMessageIter iter, dict;
	DBusPendingCall *c;
	DBusMessage *msg;

	DBG("Bluetooth: Registering %s (%s) profile", uuid, name);

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_PROFILE_MGMT_INTERFACE, "RegisterProfile");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &object);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &uuid);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	ofono_dbus_dict_append(&dict, "Name", DBUS_TYPE_STRING, &name);
	ofono_dbus_dict_append(&dict, "Version", DBUS_TYPE_UINT16, &version);

	if (role)
		ofono_dbus_dict_append(&dict, "Role", DBUS_TYPE_STRING, &role);

	if (features)
		ofono_dbus_dict_append(&dict, "Features", DBUS_TYPE_UINT16,
								&features);

	dbus_message_iter_close_container(&iter, &dict);

	if (!dbus_connection_send_with_reply(conn, msg, &c, -1)) {
		ofono_error("Sending RegisterProfile failed");
		dbus_message_unref(msg);
		return -EIO;
	}

	dbus_pending_call_set_notify(c, profile_register_cb, NULL, NULL);
	dbus_pending_call_unref(c);

	dbus_message_unref(msg);

	return 0;
}

void bt_unregister_profile(DBusConnection *conn, const char *object)
{
	DBusMessageIter iter;
	DBusPendingCall *c;
	DBusMessage *msg;

	DBG("Bluetooth: Unregistering profile %s", object);

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_PROFILE_MGMT_INTERFACE, "UnregisterProfile");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &object);

	if (!dbus_connection_send_with_reply(conn, msg, &c, -1)) {
		ofono_error("Sending UnregisterProfile failed");
		dbus_message_unref(msg);
		return;
	}

	dbus_pending_call_set_notify(c, unregister_profile_cb, NULL, NULL);
	dbus_pending_call_unref(c);

	dbus_message_unref(msg);
}

static void finish_profile_cb(DBusPendingCall *call, gpointer user_data)
{
	struct finish_callback *callback = user_data;
	DBusMessage *reply;
	DBusError derr;
	gboolean success;

	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&derr);

	success = TRUE;

	if (dbus_set_error_from_message(&derr, reply)) {
		success = FALSE;

		ofono_error("%s() replied an error: %s, %s", callback->member,
						derr.name, derr.message);
		dbus_error_free(&derr);
	}

	if (callback->cb)
		callback->cb(success, callback->user_data);

	dbus_message_unref(reply);
}

static void finish_callback_free(void *data)
{
	struct finish_callback *callback = data;

	g_free(callback->member);
	g_free(callback);
}

static void device_send_message(DBusConnection *conn, const char *device,
				const char *member, const char *uuid,
				bt_finish_cb cb, gpointer user_data)
{
	struct finish_callback *callback;
	DBusMessageIter iter;
	DBusPendingCall *c;
	DBusMessage *msg;

	DBG("Bluetooth: sending %s for %s on %s", member, uuid, device);

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, device,
				BLUEZ_DEVICE_INTERFACE, member);

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &uuid);

	if (!dbus_connection_send_with_reply(conn, msg, &c, -1)) {
		ofono_error("Sending %s failed", member);
		dbus_message_unref(msg);
		return;
	}

	callback = g_new0(struct finish_callback, 1);
	callback->cb = cb;
	callback->user_data = user_data;
	callback->member = g_strdup(dbus_message_get_member(msg));

	dbus_pending_call_set_notify(c, finish_profile_cb, callback,
							finish_callback_free);
	dbus_pending_call_unref(c);

	dbus_message_unref(msg);
}

void bt_connect_profile(DBusConnection *conn,
				const char *device, const char *uuid,
				bt_finish_cb cb, gpointer user_data)
{
	device_send_message(conn, device, "ConnectProfile", uuid,
							cb, user_data);
}

void bt_disconnect_profile(DBusConnection *conn,
				const char *device, const char *uuid,
				bt_finish_cb cb, gpointer user_data)
{
	device_send_message(conn, device, "DisconnectProfile", uuid,
							cb, user_data);
}

OFONO_PLUGIN_DEFINE(bluez5, "BlueZ 5 Utils Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, NULL, NULL)
