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
#include <gdbus.h>
#include <stdio.h>
#include <errno.h>

#include "ofono.h"
#include "message.h"

struct message {
	struct ofono_uuid uuid;
	enum message_state state;
	struct ofono_atom *atom;
	void *data;
};

static const char *message_state_to_string(enum message_state s)
{
	switch (s) {
	case MESSAGE_STATE_PENDING:
		return "pending";
	case MESSAGE_STATE_SENT:
		return "sent";
	case MESSAGE_STATE_FAILED:
		return "failed";
	case MESSAGE_STATE_CANCELLED:
		return "cancelled";
	}

	return NULL;
}

static DBusMessage *message_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct message *m = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	message_append_properties(m, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *message_cancel(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct message *m = data;
	int res;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (m->state != MESSAGE_STATE_PENDING)
		return __ofono_error_not_available(msg);

	res = __ofono_sms_txq_cancel(__ofono_atom_get_data(m->atom), &m->uuid);

	switch (res) {
	case -ENOENT:
		return __ofono_error_not_found(msg);
	case -EPERM:
		return __ofono_error_access_denied(msg);
	case 0:
		return dbus_message_new_method_return(msg);
	default:
		return __ofono_error_failed(msg);
	}
}

static GDBusMethodTable message_methods[] = {
	{ "GetProperties",  "",    "a{sv}",   message_get_properties },
	{ "Cancel",         "",    "",        message_cancel },
	{ }
};

static GDBusSignalTable message_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

struct message *message_create(const struct ofono_uuid *uuid,
							struct ofono_atom *atom)
{
	struct message *v;

	v = g_try_new0(struct message, 1);
	if (v == NULL)
		return NULL;

	memcpy(&v->uuid, uuid, sizeof(*uuid));

	v->atom = atom;

	return v;
}

static void message_destroy(gpointer userdata)
{
	struct message *m = userdata;

	g_free(m);
}

gboolean message_dbus_register(struct message *m)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = message_path_from_uuid(m->atom, &m->uuid);

	if (!g_dbus_register_interface(conn, path, OFONO_MESSAGE_INTERFACE,
					message_methods, message_signals,
					NULL, m, message_destroy)) {
		ofono_error("Could not register Message %s", path);
		message_destroy(m);

		return FALSE;
	}

	return TRUE;
}

void message_dbus_unregister(struct message *m)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = message_path_from_uuid(m->atom, &m->uuid);

	g_dbus_unregister_interface(conn, path, OFONO_MESSAGE_INTERFACE);

	return;
}

const struct ofono_uuid *message_get_uuid(const struct message *m)
{
	return &m->uuid;
}

void message_set_state(struct message *m, enum message_state new_state)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *state;

	if (m->state == new_state)
		return;

	path = message_path_from_uuid(m->atom, &m->uuid);

	m->state = new_state;
	state = message_state_to_string(m->state);

	ofono_dbus_signal_property_changed(conn, path, OFONO_MESSAGE_INTERFACE,
							"State",
							DBUS_TYPE_STRING,
							&state);
}

void message_append_properties(struct message *m, DBusMessageIter *dict)
{
	const char *state = message_state_to_string(m->state);

	ofono_dbus_dict_append(dict, "State", DBUS_TYPE_STRING, &state);
}

void message_emit_added(struct message *m, const char *interface)
{
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *path;
	const char *atompath = __ofono_atom_get_path(m->atom);

	signal = dbus_message_new_signal(atompath, interface, "MessageAdded");
	if (signal == NULL)
		return;

	path = message_path_from_uuid(m->atom, &m->uuid);

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	message_append_properties(m, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(ofono_dbus_get_connection(), signal);
}

void message_emit_removed(struct message *m, const char *interface)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *atompath = __ofono_atom_get_path(m->atom);
	const char *path = message_path_from_uuid(m->atom, &m->uuid);

	g_dbus_emit_signal(conn, atompath, interface, "MessageRemoved",
							DBUS_TYPE_OBJECT_PATH,
							&path,
							DBUS_TYPE_INVALID);
}

const char *message_path_from_uuid(struct ofono_atom *atom,
						const struct ofono_uuid *uuid)
{
	static char path[256];
	const char *atompath = __ofono_atom_get_path(atom);

	snprintf(path, sizeof(path), "%s/message_%s", atompath,
						ofono_uuid_to_str(uuid));

	return path;
}

void *message_get_data(struct message *m)
{
	return m->data;
}

void message_set_data(struct message *m, void *data)
{
	m->data = data;
}
