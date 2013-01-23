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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <glib.h>

#include <gdbus.h>
#include <gatchat.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/dbus.h>
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/call-volume.h>
#include <ofono/handsfree.h>

#include <drivers/hfpmodem/slc.h>

#include "bluez5.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define HFP_EXT_PROFILE_PATH   "/bluetooth/profile/hfp_hf"

struct hfp {
	struct hfp_slc_info info;
	DBusMessage *msg;
};

static GHashTable *modem_hash = NULL;
static GHashTable *devices_proxies = NULL;
static GDBusClient *bluez = NULL;

static void hfp_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void slc_established(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct hfp *hfp = ofono_modem_get_data(modem);
	DBusMessage *msg;

	ofono_modem_set_powered(modem, TRUE);

	msg = dbus_message_new_method_return(hfp->msg);
	g_dbus_send_message(ofono_dbus_get_connection(), msg);
	dbus_message_unref(hfp->msg);
	hfp->msg = NULL;

	ofono_info("Service level connection established");
}

static void slc_failed(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;
	DBusMessage *msg;

	msg = g_dbus_create_error(hfp->msg, BLUEZ_ERROR_INTERFACE
						".Failed",
						"HFP Handshake failed");

	g_dbus_send_message(ofono_dbus_get_connection(), msg);
	dbus_message_unref(hfp->msg);
	hfp->msg = NULL;

	ofono_error("Service level connection failed");
	ofono_modem_set_powered(modem, FALSE);

	g_at_chat_unref(info->chat);
	info->chat = NULL;
}

static void hfp_disconnected_cb(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;

	DBG("HFP disconnected");

	ofono_modem_set_powered(modem, FALSE);

	g_at_chat_unref(info->chat);
	info->chat = NULL;
}

static int service_level_connection(struct ofono_modem *modem,
						int fd, guint16 version)
{
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;
	GIOChannel *io;
	GAtSyntax *syntax;
	GAtChat *chat;

	io = g_io_channel_unix_new(fd);
	if (io == NULL) {
		ofono_error("Service level connection failed: %s (%d)",
						strerror(errno), errno);
		return -EIO;
	}

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_channel_unref(io);

	if (chat == NULL)
		return -ENOMEM;

	g_at_chat_set_disconnect_function(chat, hfp_disconnected_cb, modem);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, hfp_debug, "");

	hfp_slc_info_init(info, version);
	info->chat = chat;

	hfp_slc_establish(info, slc_established, slc_failed, modem);

	return -EINPROGRESS;
}

static struct ofono_modem *modem_register(const char *device,
				const char *device_address, const char *alias)
{
	struct ofono_modem *modem;
	char *path;

	modem = g_hash_table_lookup(modem_hash, device);
	if (modem != NULL)
		return modem;

	path = g_strconcat("hfp", device, NULL);

	modem = ofono_modem_create(path, "hfp");

	g_free(path);

	if (modem == NULL)
		return NULL;

	ofono_modem_set_string(modem, "Address", device_address);

	ofono_modem_set_name(modem, alias);
	ofono_modem_register(modem);

	g_hash_table_insert(modem_hash, g_strdup(device), modem);

	return modem;
}

static int hfp_probe(struct ofono_modem *modem)
{
	struct hfp *hfp;

	DBG("modem: %p", modem);

	hfp = g_new0(struct hfp, 1);

	ofono_modem_set_data(modem, hfp);

	return 0;
}

static void hfp_remove(struct ofono_modem *modem)
{
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;

	DBG("modem: %p", modem);

	if (hfp->msg)
		dbus_message_unref(hfp->msg);

	g_at_chat_unref(info->chat);

	g_free(hfp);

	ofono_modem_set_data(modem, NULL);
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
	struct hfp *hfp = ofono_modem_get_data(modem);
	char *address = (char *) ofono_modem_get_string(modem, "Address");

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "hfpmodem", address);
	ofono_voicecall_create(modem, 0, "hfpmodem", &hfp->info);
	ofono_netreg_create(modem, 0, "hfpmodem", &hfp->info);
	ofono_handsfree_create(modem, 0, "hfpmodem", &hfp->info);
	ofono_call_volume_create(modem, 0, "hfpmodem", &hfp->info);
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
	struct hfp *hfp;
	struct ofono_modem *modem;
	DBusMessageIter iter;
	GDBusProxy *proxy;
	DBusMessageIter entry;
	const char *device, *alias, *address;
	int fd, err;

	DBG("Profile handler NewConnection");

	if (dbus_message_iter_init(msg, &entry) == FALSE)
		goto invalid;

	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_OBJECT_PATH)
		goto invalid;

	dbus_message_iter_get_basic(&entry, &device);

	proxy = g_hash_table_lookup(devices_proxies, device);
	if (proxy == NULL)
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".Rejected",
					"Unknown Bluetooth device");

	g_dbus_proxy_get_property(proxy, "Alias", &iter);

	dbus_message_iter_get_basic(&iter, &alias);

	if (g_dbus_proxy_get_property(proxy, "Address", &iter) == FALSE)
		goto invalid;

	dbus_message_iter_get_basic(&iter, &address);

	dbus_message_iter_next(&entry);
	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_UNIX_FD)
		goto invalid;

	dbus_message_iter_get_basic(&entry, &fd);
	if (fd < 0)
		goto invalid;

	modem = modem_register(device, address, alias);
	if (modem == NULL) {
		close(fd);
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".Rejected",
					"Could not register HFP modem");
	}

	err = service_level_connection(modem, fd, HFP_VERSION_LATEST);
	if (err < 0 && err != -EINPROGRESS)
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".Rejected",
					"Not enough resources");

	hfp = ofono_modem_get_data(modem);
	hfp->msg = dbus_message_ref(msg);

	return NULL;

invalid:
	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE ".Rejected",
					"Invalid arguments in method call");
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

static gboolean has_hfp_ag_uuid(DBusMessageIter *array)
{
	DBusMessageIter value;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return FALSE;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
		const char *uuid;

		dbus_message_iter_get_basic(&value, &uuid);

		if (g_str_equal(uuid, HFP_AG_UUID) == TRUE)
			return TRUE;

		dbus_message_iter_next(&value);
	}

	return FALSE;
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *interface, *path, *alias, *address;
	DBusMessageIter iter;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	if (g_str_equal(BLUEZ_DEVICE_INTERFACE, interface) == FALSE)
		return;

	g_hash_table_insert(devices_proxies, g_strdup(path),
						g_dbus_proxy_ref(proxy));
	DBG("Device proxy: %s(%p)", path, proxy);

	if (g_dbus_proxy_get_property(proxy, "UUIDs", &iter) == FALSE)
		return;

	if (has_hfp_ag_uuid(&iter) == FALSE)
		return;

	if (g_dbus_proxy_get_property(proxy, "Alias", &iter) == FALSE)
		return;

	dbus_message_iter_get_basic(&iter, &alias);


	if (g_dbus_proxy_get_property(proxy, "Address", &iter) == FALSE)
		return;

	dbus_message_iter_get_basic(&iter, &address);

	modem_register(path, address, alias);
}

static void proxy_removed(GDBusProxy *proxy, void *user_data)
{
	const char *interface, *path;
	struct ofono_modem *modem;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	if (g_str_equal(BLUEZ_DEVICE_INTERFACE, interface)) {
		g_hash_table_remove(devices_proxies, path);
		DBG("Device proxy: %s(%p)", path, proxy);
	}

	modem = g_hash_table_lookup(modem_hash, path);
	if (modem == NULL)
		return;

	g_hash_table_remove(modem_hash, path);
	ofono_modem_remove(modem);
}

static void property_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	const char *interface, *path, *alias;
	struct ofono_modem *modem;
	DBusMessageIter alias_iter;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	DBG("path: %s interface: %s", path, interface);

	if (g_str_equal(BLUEZ_DEVICE_INTERFACE, interface) == FALSE)
		return;

	if (g_dbus_proxy_get_property(proxy, "Alias", &alias_iter) == FALSE)
		return;

	dbus_message_iter_get_basic(&alias_iter, &alias);

	modem = g_hash_table_lookup(modem_hash, path);
	if (modem == NULL)
		return;

	ofono_modem_set_name(modem, alias);
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

	modem_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
								NULL);

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

	g_hash_table_destroy(modem_hash);
	g_hash_table_destroy(devices_proxies);
}

OFONO_PLUGIN_DEFINE(hfp_bluez5, "External Hands-Free Profile Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
