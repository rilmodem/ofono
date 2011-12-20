/*
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

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <netinet/ether.h>

#include <glib.h>
#include <gdbus.h>

#include "dundee.h"

static int next_device_id = 0;
static GHashTable *device_hash;

struct ipv4_settings {
	char *interface;
	char *ip;
	char **nameservers;
};

struct dundee_device {
	char *path;
	struct dundee_device_driver *driver;
	gboolean registered;

	char *name;
	gboolean active;
	struct ipv4_settings settings;

	void *data;
};

const char *__dundee_device_get_path(struct dundee_device *device)
{
	return device->path;
}

static void settings_append(struct dundee_device *device,
					DBusMessageIter *iter)
{
	DBusMessageIter variant;
	DBusMessageIter array;
	char typesig[5];
	char arraysig[6];

	arraysig[0] = DBUS_TYPE_ARRAY;
	arraysig[1] = typesig[0] = DBUS_DICT_ENTRY_BEGIN_CHAR;
	arraysig[2] = typesig[1] = DBUS_TYPE_STRING;
	arraysig[3] = typesig[2] = DBUS_TYPE_VARIANT;
	arraysig[4] = typesig[3] = DBUS_DICT_ENTRY_END_CHAR;
	arraysig[5] = typesig[4] = '\0';

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						arraysig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						typesig, &array);

	if (device->active == FALSE)
		goto out;

	if (device->settings.interface)
		ofono_dbus_dict_append(&array, "Interface",
				DBUS_TYPE_STRING, &device->settings.interface);

	if (device->settings.ip)
		ofono_dbus_dict_append(&array, "Address", DBUS_TYPE_STRING,
					&device->settings.ip);

	if (device->settings.nameservers)
		ofono_dbus_dict_append_array(&array, "DomainNameServers",
						DBUS_TYPE_STRING,
						&device->settings.nameservers);

out:
	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

static void settings_append_dict(struct dundee_device *device,
				DBusMessageIter *dict)
{
	DBusMessageIter entry;
	const char *key = "Settings";

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	settings_append(device, &entry);

	dbus_message_iter_close_container(dict, &entry);
}

void __dundee_device_append_properties(struct dundee_device *device,
					DBusMessageIter *dict)
{
	settings_append_dict(device, dict);

	ofono_dbus_dict_append(dict, "Name", DBUS_TYPE_STRING,
				&device->name);

	ofono_dbus_dict_append(dict, "Active", DBUS_TYPE_BOOLEAN,
				&device->active);
}

void __dundee_device_foreach(dundee_device_foreach_func func, void *userdata)
{
	GHashTableIter iter;
	gpointer key, value;

	DBG("");

	g_hash_table_iter_init(&iter, device_hash);

	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		struct dundee_device *device = value;

		func(device, userdata);
	}
}

static DBusMessage *device_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct dundee_device *device = data;
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

	__dundee_device_append_properties(device, &dict);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *set_property_active(struct dundee_device *device,
					DBusMessage *msg,
					DBusMessageIter *var)
{
	ofono_bool_t active;

	DBG("%p path %s", device, device->path);

	if (dbus_message_iter_get_arg_type(var) != DBUS_TYPE_BOOLEAN)
		return __dundee_error_invalid_args(msg);

	dbus_message_iter_get_basic(var, &active);

	device->active = active;

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *device_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct dundee_device *device = data;
	DBusMessageIter iter, var;
	const char *name;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __dundee_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __dundee_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __dundee_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_str_equal(name, "Active"))
		return set_property_active(device, msg, &var);

	return __dundee_error_invalid_args(msg);
}

static const GDBusMethodTable device_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			device_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, device_set_property) },
	{ }
};

static const GDBusSignalTable device_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static int register_device(struct dundee_device *device)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;

	DBG("%p path %s", device, device->path);

	if (!g_dbus_register_interface(conn, device->path,
					DUNDEE_DEVICE_INTERFACE,
					device_methods, device_signals,
					NULL, device, NULL)) {
		ofono_error("Could not register Device %s", device->path);
		return -EIO;
	}

	signal = dbus_message_new_signal(DUNDEE_MANAGER_PATH,
						DUNDEE_MANAGER_INTERFACE,
						"DeviceAdded");

	if (signal == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH,
					&device->path);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	__dundee_device_append_properties(device, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(conn, signal);

	return 0;
}

static int unregister_device(struct dundee_device *device)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("%p path %s", device, device->path);

	g_dbus_unregister_interface(conn, device->path,
					DUNDEE_DEVICE_INTERFACE);

	g_dbus_emit_signal(conn, DUNDEE_MANAGER_PATH,
				DUNDEE_MANAGER_INTERFACE, "DeviceRemoved",
				DBUS_TYPE_OBJECT_PATH, &device->path,
				DBUS_TYPE_INVALID);

	return 0;
}

static void destroy_device(gpointer user)
{
	struct dundee_device *device = user;

	g_free(device->settings.interface);
	g_free(device->settings.ip);
	g_strfreev(device->settings.nameservers);

	g_free(device->path);
	g_free(device->name);

	g_free(device);
}

struct dundee_device *dundee_device_create(struct dundee_device_driver *d)
{
	struct dundee_device *device;

	device = g_try_new0(struct dundee_device, 1);
	if (device == NULL)
		return NULL;

	device->driver = d;

	device->path = g_strdup_printf("/device%d", next_device_id);
	if (device->path == NULL) {
		g_free(device);
		return NULL;
	}

	next_device_id += 1;

	return device;
}

int dundee_device_register(struct dundee_device *device)
{
	int err;

	err = register_device(device);
	if (err < 0)
		return err;

	device->registered = TRUE;

	g_hash_table_insert(device_hash, g_strdup(device->path), device);

	return 0;
}

void dundee_device_unregister(struct dundee_device *device)
{
	DBG("%p", device);

	unregister_device(device);

	device->registered = FALSE;

	g_hash_table_remove(device_hash, device->path);
}

void dundee_device_set_data(struct dundee_device *device, void *data)
{
	device->data = data;
}

void *dundee_device_get_data(struct dundee_device *device)
{
	return device->data;
}

int dundee_device_set_name(struct dundee_device *device, const char *name)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("%p name %s", device, name);

	g_free(device->name);
	device->name = g_strdup(name);

	if (device->registered == FALSE)
		return 0;

	ofono_dbus_signal_property_changed(conn, device->path,
					DUNDEE_DEVICE_INTERFACE, "Name",
					DBUS_TYPE_STRING, &device->name);

	return 0;
}

static void device_shutdown(gpointer key, gpointer value, gpointer user_data)
{
	struct dundee_device *device = value;

	unregister_device(device);
}

void __dundee_device_shutdown(void)
{
	g_hash_table_foreach(device_hash, device_shutdown, NULL);

	__dundee_exit();
}

int __dundee_device_init(void)
{
	DBG("");

	device_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, destroy_device);

	return 0;
}

void __dundee_device_cleanup(void)
{
	DBG("");

	g_hash_table_destroy(device_hash);
}
