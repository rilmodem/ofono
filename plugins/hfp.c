/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ProFUSION embedded systems
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
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/call-volume.h>

#include <drivers/hfpmodem/hfpmodem.h>

#include <ofono/dbus.h>

#include "bluetooth.h"

#define	BLUEZ_GATEWAY_INTERFACE		BLUEZ_SERVICE ".HandsfreeGateway"

#define HFP_AGENT_INTERFACE "org.bluez.HandsfreeAgent"
#define HFP_AGENT_ERROR_INTERFACE "org.bluez.Error"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

static const char *brsf_prefix[] = { "+BRSF:", NULL };
static const char *cind_prefix[] = { "+CIND:", NULL };
static const char *cmer_prefix[] = { "+CMER:", NULL };
static const char *chld_prefix[] = { "+CHLD:", NULL };

static DBusConnection *connection;
static GHashTable *modem_hash = NULL;

static void hfp_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static void clear_data(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);

	if (!data->chat)
		return;

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	memset(data->cind_val, 0, sizeof(data->cind_val));
	memset(data->cind_pos, 0, sizeof(data->cind_pos));
}

static void service_level_conn_established(struct ofono_modem *modem)
{
	DBusMessage *msg;
	struct hfp_data *data = ofono_modem_get_data(modem);

	ofono_modem_set_powered(modem, TRUE);

	msg = dbus_message_new_method_return(data->slc_msg);
	g_dbus_send_message(connection, msg);
	dbus_message_unref(data->slc_msg);
	data->slc_msg = NULL;

	ofono_info("Service level connection established");

	g_at_chat_send(data->chat, "AT+CMEE=1", NULL, NULL, NULL, NULL);
}

static void service_level_conn_failed(struct ofono_modem *modem)
{
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
	clear_data(modem);
}

static void chld_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	unsigned int ag_mpty_feature = 0;
	GAtResultIter iter;
	const char *str;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CHLD:"))
		return;

	if (!g_at_result_iter_open_list(&iter))
		return;

	while (g_at_result_iter_next_unquoted_string(&iter, &str)) {
		if (!strcmp(str, "0"))
			ag_mpty_feature |= AG_CHLD_0;
		else if (!strcmp(str, "1"))
			ag_mpty_feature |= AG_CHLD_1;
		else if (!strcmp(str, "1x"))
			ag_mpty_feature |= AG_CHLD_1x;
		else if (!strcmp(str, "2"))
			ag_mpty_feature |= AG_CHLD_2;
		else if (!strcmp(str, "2x"))
			ag_mpty_feature |= AG_CHLD_2x;
		else if (!strcmp(str, "3"))
			ag_mpty_feature |= AG_CHLD_3;
		else if (!strcmp(str, "4"))
			ag_mpty_feature |= AG_CHLD_4;
	}

	if (!g_at_result_iter_close_list(&iter))
		return;

	data->ag_mpty_features = ag_mpty_feature;

	service_level_conn_established(modem);
}

static void cmer_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);

	if (!ok) {
		service_level_conn_failed(modem);
		return;
	}

	if (data->ag_features & AG_FEATURE_3WAY)
		g_at_chat_send(data->chat, "AT+CHLD=?", chld_prefix,
			chld_cb, modem, NULL);
	else
		service_level_conn_established(modem);
}

static void cind_status_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int index;
	int value;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	while (g_at_result_iter_next_number(&iter, &value)) {
		int i;

		for (i = 0; i < HFP_INDICATOR_LAST; i++) {
			if (index != data->cind_pos[i])
				continue;

			data->cind_val[i] = value;
		}

		index += 1;
	}

	g_at_chat_send(data->chat, "AT+CMER=3,0,0,1", cmer_prefix,
				cmer_cb, modem, NULL);
	return;

error:
	service_level_conn_failed(modem);
}

static void cind_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	const char *str;
	int index;
	int min, max;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	while (g_at_result_iter_open_list(&iter)) {
		if (!g_at_result_iter_next_string(&iter, &str))
			goto error;

		if (!g_at_result_iter_open_list(&iter))
			goto error;

		while (g_at_result_iter_next_range(&iter, &min, &max))
			;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (g_str_equal("service", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_SERVICE] = index;
		else if (g_str_equal("call", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_CALL] = index;
		else if (g_str_equal("callsetup", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_CALLSETUP] = index;
		else if (g_str_equal("callheld", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_CALLHELD] = index;
		else if (g_str_equal("signal", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_SIGNAL] = index;
		else if (g_str_equal("roam", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_ROAM] = index;
		else if (g_str_equal("battchg", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_BATTCHG] = index;

		index += 1;
	}

	g_at_chat_send(data->chat, "AT+CIND?", cind_prefix,
			cind_status_cb, modem, NULL);
	return;

error:
	service_level_conn_failed(modem);
}

static void brsf_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+BRSF:"))
		goto error;

	g_at_result_iter_next_number(&iter, (gint *)&data->ag_features);

	g_at_chat_send(data->chat, "AT+CIND=?", cind_prefix,
				cind_cb, modem, NULL);
	return;

error:
	service_level_conn_failed(modem);
}

static void hfp_disconnected_cb(gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	ofono_modem_set_powered(modem, FALSE);
	clear_data(modem);
}

/* either oFono or Phone could request SLC connection */
static int service_level_connection(struct ofono_modem *modem, int fd)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
	GIOChannel *io;
	GAtSyntax *syntax;
	GAtChat *chat;
	char buf[64];

	io = g_io_channel_unix_new(fd);
	if (!io) {
		ofono_error("Service level connection failed: %s (%d)",
			strerror(errno), errno);
		return -EIO;
	}

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	if (!chat)
		return -ENOMEM;

	g_at_chat_set_disconnect_function(chat, hfp_disconnected_cb, modem);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, hfp_debug, NULL);

	snprintf(buf, sizeof(buf), "AT+BRSF=%d", data->hf_features);
	g_at_chat_send(chat, buf, brsf_prefix,
				brsf_cb, modem, NULL);
	data->chat = chat;

	return -EINPROGRESS;
}

static DBusMessage *hfp_agent_new_connection(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	int fd, err;
	struct ofono_modem *modem = data;
	struct hfp_data *hfp_data = ofono_modem_get_data(modem);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UNIX_FD, &fd,
				DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

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

	ofono_modem_remove(modem);

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable agent_methods[] = {
	{ "NewConnection", "h", "", hfp_agent_new_connection,
		G_DBUS_METHOD_FLAG_ASYNC },
	{ "Release", "", "", hfp_agent_release },
	{ NULL, NULL, NULL, NULL }
};

static int hfp_create_modem(const char *device, const char *dev_addr,
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
	if (!data)
		goto free;

	data->hf_features |= HF_FEATURE_3WAY;
	data->hf_features |= HF_FEATURE_CLIP;
	data->hf_features |= HF_FEATURE_REMOTE_VOLUME_CONTROL;
	data->hf_features |= HF_FEATURE_ENHANCED_CALL_STATUS;
	data->hf_features |= HF_FEATURE_ENHANCED_CALL_CONTROL;

	data->handsfree_path = g_strdup(device);
	if (data->handsfree_path == NULL)
		goto free;

	ofono_modem_set_data(modem, data);
	ofono_modem_set_name(modem, alias);
	ofono_modem_register(modem);

	g_hash_table_insert(modem_hash, g_strdup(device), modem);

	return 0;

free:
	g_free(data);
	ofono_modem_remove(modem);

	return -ENOMEM;
}

static gboolean hfp_remove_each_modem(gpointer key, gpointer value,
					gpointer user_data)
{
	struct ofono_modem *modem = value;

	ofono_modem_remove(modem);

	return TRUE;
}

static void hfp_remove_all_modem()
{
	if (modem_hash == NULL)
		return;

	g_hash_table_foreach_remove(modem_hash, hfp_remove_each_modem, NULL);
}

static void hfp_set_alias(const char *device, const char *alias)
{
	struct ofono_modem *modem;

	if (!device || !alias)
		return;

	modem =	g_hash_table_lookup(modem_hash, device);
	if (!modem)
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
	if (!msg)
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
	if (!msg)
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

	if (!data)
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

	if (g_dbus_unregister_interface(connection, obj_path,
					HFP_AGENT_INTERFACE))
		hfp_unregister_ofono_handsfree(modem);

	g_hash_table_remove(modem_hash, data->handsfree_path);

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
		if (!msg)
			ofono_error("Disconnect failed");
		else
			g_dbus_send_message(connection, msg);
	}

	ofono_modem_set_powered(modem, FALSE);

	dbus_error_free(&derr);

done:
	dbus_message_unref(reply);
}

/* power up hardware */
static int hfp_enable(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
	int status;

	DBG("%p", modem);

	status = bluetooth_send_with_reply(data->handsfree_path,
					BLUEZ_GATEWAY_INTERFACE, "Connect",
					hfp_connect_reply, modem, NULL,
					DBUS_TIMEOUT, DBUS_TYPE_INVALID);

	if (status < 0)
		return -EINVAL;

	return -EINPROGRESS;
}

static void hfp_power_down(DBusPendingCall *call, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
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
}

static int hfp_disable(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
	int status;

	DBG("%p", modem);

	clear_data(modem);

	if (data->agent_registered) {
		status = bluetooth_send_with_reply(data->handsfree_path,
					BLUEZ_GATEWAY_INTERFACE, "Disconnect",
					hfp_power_down, modem, NULL,
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

	ofono_voicecall_create(modem, 0, "hfpmodem", data);
	ofono_netreg_create(modem, 0, "hfpmodem", data);
	ofono_call_volume_create(modem, 0, "hfpmodem", data);
}

static void hfp_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static struct ofono_modem_driver hfp_driver = {
	.name		= "hfp",
	.probe		= hfp_probe,
	.remove		= hfp_remove,
	.enable		= hfp_enable,
	.disable	= hfp_disable,
	.pre_sim	= hfp_pre_sim,
	.post_sim	= hfp_post_sim,
};

static struct bluetooth_profile hfp_profile = {
	.name		= "hfp",
	.create		= hfp_create_modem,
	.remove_all	= hfp_remove_all_modem,
	.set_alias	= hfp_set_alias,
};

static int hfp_init()
{
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	connection = ofono_dbus_get_connection();

	err = ofono_modem_driver_register(&hfp_driver);
	if (err < 0)
		return err;

	err = bluetooth_register_uuid(HFP_AG_UUID, &hfp_profile);
	if (err < 0) {
		ofono_modem_driver_unregister(&hfp_driver);
		return err;
	}

	modem_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	return 0;
}

static void hfp_exit()
{
	bluetooth_unregister_uuid(HFP_AG_UUID);
	ofono_modem_driver_unregister(&hfp_driver);

	g_hash_table_destroy(modem_hash);
}

OFONO_PLUGIN_DEFINE(hfp, "Hands-Free Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
