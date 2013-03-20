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

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *path = g_dbus_proxy_get_path(proxy);
	const char *interface = g_dbus_proxy_get_interface(proxy);

	if (!g_str_equal(BLUEZ_DEVICE_INTERFACE, interface))
		return;

	DBG("%s %s", path, interface);
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
