/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013 Intel Corporation. All rights reserved.
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
#include <glib.h>

#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/dbus.h>
#include <ofono/plugin.h>
#include <ofono/log.h>

#include "bluez5.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define HFP_EXT_PROFILE_PATH   "/bluetooth/profile/hfp_hf"

static GHashTable *devices_proxies = NULL;
static GDBusClient *bluez = NULL;

static int hfp_probe(struct ofono_modem *modem)
{
	DBG("modem: %p", modem);

	return 0;
}

static void hfp_remove(struct ofono_modem *modem)
{
	DBG("modem: %p", modem);
}

/* power up hardware */
static int hfp_enable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static int hfp_disable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static void hfp_pre_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void hfp_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static struct ofono_modem_driver hfp_driver = {
	.name		= "hfp",
	.modem_type	= OFONO_MODEM_TYPE_HFP,
	.probe		= hfp_probe,
	.remove		= hfp_remove,
	.enable		= hfp_enable,
	.disable	= hfp_disable,
	.pre_sim	= hfp_pre_sim,
	.post_sim	= hfp_post_sim,
};

static DBusMessage *profile_new_connection(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBG("Profile handler NewConnection");

	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".NotImplemented",
					"Implementation not provided");
}

static DBusMessage *profile_release(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBG("Profile handler Release");

	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
						".NotImplemented",
						"Implementation not provided");
}

static DBusMessage *profile_cancel(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBG("Profile handler Cancel");

	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".NotImplemented",
					"Implementation not provided");
}

static DBusMessage *profile_disconnection(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBG("Profile handler RequestDisconnection");

	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".NotImplemented",
					"Implementation not provided");
}

static const GDBusMethodTable profile_methods[] = {
	{ GDBUS_ASYNC_METHOD("NewConnection",
				GDBUS_ARGS({ "device", "o"}, { "fd", "h"},
						{ "fd_properties", "a{sv}" }),
				NULL, profile_new_connection) },
	{ GDBUS_METHOD("Release", NULL, NULL, profile_release) },
	{ GDBUS_METHOD("Cancel", NULL, NULL, profile_cancel) },
	{ GDBUS_METHOD("RequestDisconnection",
				GDBUS_ARGS({"device", "o"}), NULL,
				profile_disconnection) },
	{ }
};

static void connect_handler(DBusConnection *conn, void *user_data)
{
	DBG("Registering External Profile handler ...");

	bluetooth_register_profile(conn, HFP_HS_UUID, "hfp_hf",
						HFP_EXT_PROFILE_PATH);
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	if (g_str_equal(BLUEZ_DEVICE_INTERFACE, interface) == FALSE)
		return;

	g_hash_table_insert(devices_proxies, g_strdup(path),
						g_dbus_proxy_ref(proxy));
	DBG("Device proxy: %s(%p)", path, proxy);
}

static void proxy_removed(GDBusProxy *proxy, void *user_data)
{
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	if (g_str_equal(BLUEZ_DEVICE_INTERFACE, interface)) {
		g_hash_table_remove(devices_proxies, path);
		DBG("Device proxy: %s(%p)", path, proxy);
	}

}

static void property_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	DBG("path: %s interface: %s", path, interface);
}

static int hfp_init(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	/* Registers External Profile handler */
	if (!g_dbus_register_interface(conn, HFP_EXT_PROFILE_PATH,
					BLUEZ_PROFILE_INTERFACE,
					profile_methods, NULL,
					NULL, NULL, NULL)) {
		ofono_error("Register Profile interface failed: %s",
						HFP_EXT_PROFILE_PATH);
		return -EIO;
	}

	err = ofono_modem_driver_register(&hfp_driver);
	if (err < 0) {
		g_dbus_unregister_interface(conn, HFP_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);
		return err;
	}

	bluez = g_dbus_client_new(conn, BLUEZ_SERVICE, BLUEZ_MANAGER_PATH);
	if (bluez == NULL) {
		g_dbus_unregister_interface(conn, HFP_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);
		ofono_modem_driver_unregister(&hfp_driver);
		return -ENOMEM;
	}

	devices_proxies = g_hash_table_new_full(g_str_hash, g_str_equal,
				g_free, (GDestroyNotify) g_dbus_proxy_unref);

	g_dbus_client_set_connect_watch(bluez, connect_handler, NULL);
	g_dbus_client_set_proxy_handlers(bluez, proxy_added, proxy_removed,
						property_changed, NULL);

	return 0;
}

static void hfp_exit(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	bluetooth_unregister_profile(conn, HFP_EXT_PROFILE_PATH);
	g_dbus_unregister_interface(conn, HFP_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);
	ofono_modem_driver_unregister(&hfp_driver);
	g_dbus_client_unref(bluez);

	g_hash_table_destroy(devices_proxies);
}

OFONO_PLUGIN_DEFINE(hfp_bluez5, "External Hands-Free Profile Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
