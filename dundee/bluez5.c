/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Instituto Nokia de Tecnologia - INdT
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

#include <stdint.h>
#include <sys/socket.h>
#include <gdbus.h>

#include "dundee.h"
#include "plugins/bluez5.h"

#define DUN_DT_PROFILE_PATH   "/bluetooth/profile/dun_dt"

static GDBusClient *bluez;
static GHashTable *registered_devices;

struct bluetooth_device {
	char *path;
	char *address;
	char *name;
};

static void bluetooth_device_destroy(gpointer user_data)
{
	struct bluetooth_device *bt_device = user_data;

	DBG("%s", bt_device->path);

	g_free(bt_device->path);
	g_free(bt_device->address);
	g_free(bt_device->name);
	g_free(bt_device);
}

static void bluetooth_device_connect(struct dundee_device *device,
			dundee_device_connect_cb_t cb, void *data)
{
	DBG("");
}

static void bluetooth_device_disconnect(struct dundee_device *device,
				dundee_device_disconnect_cb_t cb, void *data)
{
	DBG("");
}

struct dundee_device_driver bluetooth_driver = {
	.name = "bluetooth",
	.connect = bluetooth_device_connect,
	.disconnect = bluetooth_device_disconnect,
};

static struct bluetooth_device *bluetooth_device_create(const char *path,
					const char *address, const char *alias)
{
	struct bluetooth_device *bt_device;

	DBG("%s %s %s", path, address, alias);

	bt_device = g_try_new0(struct bluetooth_device, 1);
	if (bt_device == NULL)
		return NULL;

	bt_device->path = g_strdup(path);
	bt_device->address = g_strdup(address);
	bt_device->name = g_strdup(alias);

	return bt_device;
}

static struct bluetooth_device *bluetooth_device_register(GDBusProxy *proxy)
{
	const char *path = g_dbus_proxy_get_path(proxy);
	const char *alias, *address;
	struct bluetooth_device *bt_device;
	DBusMessageIter iter;

	DBG("%s", path);

	if (g_hash_table_lookup(registered_devices, path) != NULL)
		return NULL;

	if (!g_dbus_proxy_get_property(proxy, "Address", &iter))
		return NULL;

	dbus_message_iter_get_basic(&iter, &address);

	if (!g_dbus_proxy_get_property(proxy, "Alias", &iter))
		return NULL;

	dbus_message_iter_get_basic(&iter, &alias);

	bt_device = bluetooth_device_create(path, address, alias);
	if (bt_device == NULL) {
		ofono_error("Register bluetooth device failed");
		return NULL;
	}

	g_hash_table_insert(registered_devices, g_strdup(path), bt_device);

	return bt_device;
}

static void bluetooth_device_unregister(const char *path)
{
	DBG("");

	g_hash_table_remove(registered_devices, path);
}

static gboolean has_dun_uuid(DBusMessageIter *array)
{
	DBusMessageIter value;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return FALSE;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
		const char *uuid;

		dbus_message_iter_get_basic(&value, &uuid);

		if (g_str_equal(uuid, DUN_GW_UUID))
			return TRUE;

		dbus_message_iter_next(&value);
	}

	return FALSE;
}

static void alias_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	const char *alias;
	struct bluetooth_device *bt_device = user_data;

	if (!g_str_equal("Alias", name))
		return;

	dbus_message_iter_get_basic(iter, &alias);

	bt_device->name = g_strdup(alias);
}

static void bluetooth_device_removed(GDBusProxy *proxy, void *user_data)
{
	struct bluetooth_device *bt_device = user_data;

	DBG("%s", bt_device->path);

	bluetooth_device_unregister(bt_device->path);
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *path = g_dbus_proxy_get_path(proxy);
	const char *interface = g_dbus_proxy_get_interface(proxy);
	struct bluetooth_device *bt_device;
	DBusMessageIter iter;

	if (!g_str_equal(BLUEZ_DEVICE_INTERFACE, interface))
		return;

	if (!g_dbus_proxy_get_property(proxy, "UUIDs", &iter))
		return;

	DBG("%s %s", path, interface);

	if (!has_dun_uuid(&iter))
		return;

	bt_device = bluetooth_device_register(proxy);
	g_dbus_proxy_set_property_watch(proxy, alias_changed, bt_device);
	g_dbus_proxy_set_removed_watch(proxy, bluetooth_device_removed,
								bt_device);
}

static void connect_handler(DBusConnection *conn, void *user_data)
{
	DBG("");

	bt_register_profile_with_role(conn, DUN_GW_UUID, DUN_VERSION_1_2,
				"dun_dt", DUN_DT_PROFILE_PATH, "client");
}

int __dundee_bluetooth_init(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("");

	bluez = g_dbus_client_new(conn, BLUEZ_SERVICE, BLUEZ_MANAGER_PATH);
	g_dbus_client_set_connect_watch(bluez, connect_handler, NULL);
	g_dbus_client_set_proxy_handlers(bluez, proxy_added, NULL, NULL, NULL);

	registered_devices = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, bluetooth_device_destroy);

	return 0;
}

void __dundee_bluetooth_cleanup(void)
{
	DBG("");

	g_dbus_client_unref(bluez);
	g_hash_table_destroy(registered_devices);
}
