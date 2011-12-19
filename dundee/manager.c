/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  BMW Car IT GmbH. All rights reserved.
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
#include <gdbus.h>

#include "dundee.h"

static void append_device(struct dundee_device *device, void *userdata)
{
	DBusMessageIter *array = userdata;
	const char *path = __dundee_device_get_path(device);
	DBusMessageIter entry, dict;

	dbus_message_iter_open_container(array, DBUS_TYPE_STRUCT,
						NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
					&path);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
				OFONO_PROPERTIES_ARRAY_SIGNATURE,
				&dict);

	__dundee_device_append_properties(device, &dict);

	dbus_message_iter_close_container(&entry, &dict);
	dbus_message_iter_close_container(array, &entry);
}

static DBusMessage *manager_get_devices(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;

	DBG("");

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_OBJECT_PATH_AS_STRING
					DBUS_TYPE_ARRAY_AS_STRING
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING
					DBUS_STRUCT_END_CHAR_AS_STRING,
					&array);

	__dundee_device_foreach(append_device, &array);

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static const GDBusMethodTable manager_methods[] = {
	{ GDBUS_METHOD("GetDevices", NULL,
		GDBUS_ARGS({ "devices", "a(oa{sv})" }), manager_get_devices) },
	{ }
};

static const GDBusSignalTable manager_signals[] = {
	{ GDBUS_SIGNAL("DevicesAdded",
		GDBUS_ARGS({ "path", "o"},{ "properties", "a{sv}" })) },
	{ GDBUS_SIGNAL("DeviceRemoved",
		GDBUS_ARGS({ "path", "o"})) },
	{ }
};

int __dundee_manager_init(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	gboolean ret;

	ret = g_dbus_register_interface(conn, DUNDEE_MANAGER_PATH,
					DUNDEE_MANAGER_INTERFACE,
					manager_methods, manager_signals,
					NULL, NULL, NULL);

	if (ret == FALSE)
		return -1;

	return 0;
}

void __dundee_manager_cleanup(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	g_dbus_unregister_interface(conn, DUNDEE_MANAGER_PATH,
					DUNDEE_MANAGER_INTERFACE);
}
