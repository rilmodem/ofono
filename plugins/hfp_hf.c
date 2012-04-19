/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ProFUSION embedded systems
 *  Copyright (C) 2011  BMW Car IT GmbH. All rights reserved.
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
#include <unistd.h>
#include <glib.h>
#include <gatchat.h>
#include <gattty.h>
#include <gdbus.h>
#include <ofono.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/call-volume.h>
#include <ofono/handsfree.h>

#include <drivers/hfpmodem/slc.h>

#include "bluetooth.h"

#define	BLUEZ_GATEWAY_INTERFACE		BLUEZ_SERVICE ".HandsfreeGateway"

#define HFP_AGENT_INTERFACE "org.bluez.HandsfreeAgent"
#define HFP_AGENT_ERROR_INTERFACE "org.bluez.Error"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

static DBusConnection *connection;
static GHashTable *modem_hash = NULL;

struct hfp_data {
	struct hfp_slc_info info;
	char *handsfree_path;
	char *handsfree_address;
	DBusMessage *slc_msg;
	gboolean agent_registered;
	DBusPendingCall *call;
};

static void hfp_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void slc_established(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct hfp_data *data = ofono_modem_get_data(modem);
	DBusMessage *msg;

	ofono_modem_set_powered(modem, TRUE);

	msg = dbus_message_new_method_return(data->slc_msg);
	g_dbus_send_message(connection, msg);
	dbus_message_unref(data->slc_msg);
	data->slc_msg = NULL;

	ofono_info("Service level connection established");
}

static void slc_failed(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct hfp_data *data = ofono_modem_get_data(modem);
	DBusMessage *msg;

	msg = g_dbus_create_error(data->slc_msg, HFP_AGENT_ERROR_INTERFACE
					".Failed",
					"HFP Handshake failed");
	g_dbus_send_message(connection, msg);
	dbus_message_unref(data->slc_msg);
	data->slc_msg = NULL;

	ofono_error("Service level connection failed");
	ofono_modem_set_powered(modem, FALSE);

	g_at_chat_unref(data->info.chat);
	data->info.chat = NULL;
}

static void hfp_disconnected_cb(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);

	ofono_modem_set_powered(modem, FALSE);

	g_at_chat_unref(data->info.chat);
	data->info.chat = NULL;
}

/* either oFono or Phone could request SLC connection */
static int service_level_connection(struct ofono_modem *modem, int fd)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
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
	g_io_channel_unref(io);

	if (chat == NULL)
		return -ENOMEM;

	g_at_chat_set_disconnect_function(chat, hfp_disconnected_cb, modem);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, hfp_debug, "");

	data->info.chat = chat;
	hfp_slc_establish(&data->info, slc_established, slc_failed, modem);

	return -EINPROGRESS;
}

static DBusMessage *hfp_agent_new_connection(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	int fd, err;
	struct ofono_modem *modem = data;
	struct hfp_data *hfp_data = ofono_modem_get_data(modem);
	guint16 version;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UNIX_FD, &fd,
				DBUS_TYPE_UINT16, &version, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	hfp_slc_info_init(&hfp_data->info, version);

	err = service_level_connection(modem, fd);
	if (err < 0 && err != -EINPROGRESS)
		return __ofono_error_failed(msg);

	hfp_data->slc_msg = msg;
	dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *hfp_agent_release(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct hfp_data *hfp_data = ofono_modem_get_data(modem);
	const char *obj_path = ofono_modem_get_path(modem);

	g_dbus_unregister_interface(connection, obj_path, HFP_AGENT_INTERFACE);
	hfp_data->agent_registered = FALSE;

	g_hash_table_remove(modem_hash, hfp_data->handsfree_path);
	ofono_modem_remove(modem);

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable agent_methods[] = {
	{ "NewConnection", "hq", "", hfp_agent_new_connection,
		G_DBUS_METHOD_FLAG_ASYNC },
	{ "Release", "", "", hfp_agent_release },
	{ NULL, NULL, NULL, NULL }
};

static int hfp_hf_probe(const char *device, const char *dev_addr,
				const char *adapter_addr, const char *alias)
{
	struct ofono_modem *modem;
	struct hfp_data *data;
	char buf[256];

	/* We already have this device in our hash, ignore */
	if (g_hash_table_lookup(modem_hash, device) != NULL)
		return -EALREADY;

	ofono_info("Using device: %s, devaddr: %s, adapter: %s",
			device, dev_addr, adapter_addr);

	strcpy(buf, "hfp/");
	bluetooth_create_path(dev_addr, adapter_addr, buf + 4, sizeof(buf) - 4);

	modem = ofono_modem_create(buf, "hfp");
	if (modem == NULL)
		return -ENOMEM;

	data = g_try_new0(struct hfp_data, 1);
	if (data == NULL)
		goto free;

	data->handsfree_path = g_strdup(device);
	if (data->handsfree_path == NULL)
		goto free;

	data->handsfree_address = g_strdup(dev_addr);
	if (data->handsfree_address == NULL)
		goto free;

	ofono_modem_set_data(modem, data);
	ofono_modem_set_name(modem, alias);
	ofono_modem_register(modem);

	g_hash_table_insert(modem_hash, g_strdup(device), modem);

	return 0;

free:
	if (data != NULL)
		g_free(data->handsfree_path);

	g_free(data);
	ofono_modem_remove(modem);

	return -ENOMEM;
}

static gboolean hfp_remove_modem(gpointer key, gpointer value,
					gpointer user_data)
{
	struct ofono_modem *modem = value;
	const char *device = key;
	const char *prefix = user_data;

	if (prefix && g_str_has_prefix(device, prefix) == FALSE)
		return FALSE;

	ofono_modem_remove(modem);

	return TRUE;
}

static void hfp_hf_remove(const char *prefix)
{
	DBG("%s", prefix);

	if (modem_hash == NULL)
		return;

	g_hash_table_foreach_remove(modem_hash, hfp_remove_modem,
							(gpointer) prefix);
}

static void hfp_hf_set_alias(const char *device, const char *alias)
{
	struct ofono_modem *modem;

	if (device == NULL || alias == NULL)
		return;

	modem =	g_hash_table_lookup(modem_hash, device);
	if (modem == NULL)
		return;

	ofono_modem_set_name(modem, alias);
}

static int hfp_register_ofono_handsfree(struct ofono_modem *modem)
{
	const char *obj_path = ofono_modem_get_path(modem);
	struct hfp_data *data = ofono_modem_get_data(modem);
	DBusMessage *msg;

	DBG("Registering oFono Agent to bluetooth daemon");

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "RegisterAgent");
	if (msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &obj_path,
				DBUS_TYPE_INVALID);

	g_dbus_send_message(connection, msg);
	return 0;
}

static int hfp_unregister_ofono_handsfree(struct ofono_modem *modem)
{
	const char *obj_path = ofono_modem_get_path(modem);
	struct hfp_data *data = ofono_modem_get_data(modem);
	DBusMessage *msg;

	DBG("Unregistering oFono Agent from bluetooth daemon");

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "UnregisterAgent");
	if (msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &obj_path,
				DBUS_TYPE_INVALID);

	g_dbus_send_message(connection, msg);
	return 0;
}

static int hfp_probe(struct ofono_modem *modem)
{
	const char *obj_path = ofono_modem_get_path(modem);
	struct hfp_data *data = ofono_modem_get_data(modem);

	if (data == NULL)
		return -EINVAL;

	g_dbus_register_interface(connection, obj_path, HFP_AGENT_INTERFACE,
			agent_methods, NULL, NULL, modem, NULL);

	data->agent_registered = TRUE;

	if (hfp_register_ofono_handsfree(modem) != 0)
		return -EINVAL;

	return 0;
}

static void hfp_remove(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
	const char *obj_path = ofono_modem_get_path(modem);

	if (data->call != NULL)
		dbus_pending_call_cancel(data->call);

	if (g_dbus_unregister_interface(connection, obj_path,
					HFP_AGENT_INTERFACE))
		hfp_unregister_ofono_handsfree(modem);

	g_free(data->handsfree_address);
	g_free(data->handsfree_path);
	g_free(data);

	ofono_modem_set_data(modem, NULL);
}

static void hfp_connect_reply(DBusPendingCall *call, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	DBusError derr;
	DBusMessage *reply, *msg;

	reply = dbus_pending_call_steal_reply(call);

	if (ofono_modem_get_powered(modem))
		goto done;

	dbus_error_init(&derr);
	if (!dbus_set_error_from_message(&derr, reply))
		goto done;

	DBG("Connect reply: %s", derr.message);

	if (dbus_error_has_name(&derr, DBUS_ERROR_NO_REPLY)) {
		msg = dbus_message_new_method_call(BLUEZ_SERVICE,
				data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "Disconnect");
		if (msg == NULL)
			ofono_error("Disconnect failed");
		else
			g_dbus_send_message(connection, msg);
	}

	ofono_modem_set_powered(modem, FALSE);

	dbus_error_free(&derr);

done:
	dbus_message_unref(reply);
	data->call = NULL;
}

/* power up hardware */
static int hfp_enable(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
	int status;

	DBG("%p", modem);

	status = bluetooth_send_with_reply(data->handsfree_path,
					BLUEZ_GATEWAY_INTERFACE, "Connect",
					&data->call, hfp_connect_reply,
					modem, NULL,
					DBUS_TIMEOUT, DBUS_TYPE_INVALID);

	if (status < 0)
		return -EINVAL;

	return -EINPROGRESS;
}

static void hfp_power_down(DBusPendingCall *call, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	DBusMessage *reply;
	DBusError derr;

	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		DBG("Disconnect reply: %s", derr.message);
		dbus_error_free(&derr);
		goto done;
	}

	ofono_modem_set_powered(modem, FALSE);

done:
	dbus_message_unref(reply);
	data->call = NULL;
}

static int hfp_disable(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
	int status;

	DBG("%p", modem);

	g_at_chat_unref(data->info.chat);
	data->info.chat = NULL;

	if (data->agent_registered) {
		status = bluetooth_send_with_reply(data->handsfree_path,
					BLUEZ_GATEWAY_INTERFACE, "Disconnect",
					&data->call, hfp_power_down,
					modem, NULL,
					DBUS_TIMEOUT, DBUS_TYPE_INVALID);

		if (status < 0)
			return -EINVAL;
	}

	return -EINPROGRESS;
}

static void hfp_pre_sim(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "hfpmodem", data->handsfree_address);
	ofono_voicecall_create(modem, 0, "hfpmodem", &data->info);
	ofono_netreg_create(modem, 0, "hfpmodem", &data->info);
	ofono_call_volume_create(modem, 0, "hfpmodem", &data->info);
	ofono_handsfree_create(modem, 0, "hfpmodem", &data->info);
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

static struct bluetooth_profile hfp_hf = {
	.name		= "hfp_hf",
	.probe		= hfp_hf_probe,
	.remove		= hfp_hf_remove,
	.set_alias	= hfp_hf_set_alias,
};

static int hfp_init(void)
{
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	connection = ofono_dbus_get_connection();

	err = ofono_modem_driver_register(&hfp_driver);
	if (err < 0)
		return err;

	err = bluetooth_register_uuid(HFP_AG_UUID, &hfp_hf);
	if (err < 0) {
		ofono_modem_driver_unregister(&hfp_driver);
		return err;
	}

	modem_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	return 0;
}

static void hfp_exit(void)
{
	bluetooth_unregister_uuid(HFP_AG_UUID);
	ofono_modem_driver_unregister(&hfp_driver);

	g_hash_table_destroy(modem_hash);
}

OFONO_PLUGIN_DEFINE(hfp, "Hands-Free Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
