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

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "ofono.h"

#include "dbus-gsm.h"

#define SERVICE_NAME	"org.ofono"

#define RECONNECT_RETRY_TIMEOUT 2000

static DBusConnection *g_connection;

void dbus_gsm_free_string_array(char **array)
{
	int i;

	if (!array)
		return;

	for (i = 0; array[i]; i++)
		g_free(array[i]);

	g_free(array);
}

void dbus_gsm_append_variant(DBusMessageIter *iter,
				int type, void *value)
{
	char sig[2];
	DBusMessageIter valueiter;

	sig[0] = type;
	sig[1] = 0;

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						sig, &valueiter);

	dbus_message_iter_append_basic(&valueiter, type, value);

	dbus_message_iter_close_container(iter, &valueiter);
}

void dbus_gsm_dict_append(DBusMessageIter *dict,
			const char *key, int type, void *value)
{
	DBusMessageIter keyiter;

	if (type == DBUS_TYPE_STRING) {
		const char *str = *((const char **) value);
		if (str == NULL)
			return;
	}

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
							NULL, &keyiter);

	dbus_message_iter_append_basic(&keyiter, DBUS_TYPE_STRING, &key);

	dbus_gsm_append_variant(&keyiter, type, value);

	dbus_message_iter_close_container(dict, &keyiter);
}

void dbus_gsm_append_array_variant(DBusMessageIter *iter, int type, void *val)
{
	DBusMessageIter variant, array;
	char typesig[2];
	char arraysig[3];
	const char **str_array = *(const char ***)val;
	int i;

	arraysig[0] = DBUS_TYPE_ARRAY;
	arraysig[1] = typesig[0] = type;
	arraysig[2] = typesig[1] = '\0';

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						arraysig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						typesig, &array);

	for (i = 0; str_array[i]; i++)
		dbus_message_iter_append_basic(&array, type,
						&(str_array[i]));

	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

void dbus_gsm_dict_append_array(DBusMessageIter *dict, const char *key,
				int type, void *val)
{
	DBusMessageIter entry;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	dbus_gsm_append_array_variant(&entry, type, val);

	dbus_message_iter_close_container(dict, &entry);
}

int dbus_gsm_signal_property_changed(DBusConnection *conn,
					const char *path,
					const char *interface,
					const char *name,
					int type, void *value)
{
	DBusMessage *signal;
	DBusMessageIter iter;

	signal = dbus_message_new_signal(path, interface, "PropertyChanged");

	if (!signal) {
		ofono_error("Unable to allocate new %s.PropertyChanged signal",
				interface);
		return -1;
	}

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

	dbus_gsm_append_variant(&iter, type, value);

	return g_dbus_send_message(conn, signal);
}

int dbus_gsm_signal_array_property_changed(DBusConnection *conn,
						const char *path,
						const char *interface,
						const char *name,
						int type, void *value)

{
	DBusMessage *signal;
	DBusMessageIter iter;

	signal = dbus_message_new_signal(path, interface, "PropertyChanged");

	if (!signal) {
		ofono_error("Unable to allocate new %s.PropertyChanged signal",
				interface);
		return -1;
	}

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

	dbus_gsm_append_array_variant(&iter, type, value);

	return g_dbus_send_message(conn, signal);
}

DBusConnection *ofono_dbus_get_connection()
{
	return g_connection;
}

static void dbus_gsm_set_connection(DBusConnection *conn)
{
	if (conn && g_connection != NULL)
		ofono_error("Setting a connection when it is not NULL");

	g_connection = conn;
}

static gboolean system_bus_reconnect(void *user_data)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (!conn && (dbus_gsm_init() < 0))
		return TRUE;

	conn = ofono_dbus_get_connection();

	if (conn && dbus_connection_get_is_connected(conn))
		return FALSE;

	ofono_error("While attempting to reconnect, conn != NULL,"
			" but not connected");

	return TRUE;
}

static void system_bus_disconnected(DBusConnection *conn, void *user_data)
{
	ofono_error("System bus has disconnected!");

	dbus_gsm_set_connection(NULL);

	g_timeout_add(RECONNECT_RETRY_TIMEOUT,
				system_bus_reconnect, NULL);
}

int dbus_gsm_init()
{
	DBusConnection *conn;
	DBusError error;

	dbus_error_init(&error);

	conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, SERVICE_NAME, &error);
	if (!conn) {
		ofono_error("Unable to hop onto D-Bus: %s", error.message);
		return -1;
	}

	if (g_dbus_set_disconnect_function(conn, system_bus_disconnected,
						NULL, NULL) == FALSE) {
		dbus_connection_unref(conn);
		return -1;
	}

	dbus_gsm_set_connection(conn);

	return 0;
}

void dbus_gsm_exit()
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (!conn || !dbus_connection_get_is_connected(conn))
		return;

	dbus_gsm_set_connection(NULL);

	dbus_connection_unref(conn);
}
