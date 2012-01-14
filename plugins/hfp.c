/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  Gustavo F. Padovan <padovan@profusion.mobi>
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

#define	BLUEZ_SERVICE "org.bluez"
#define	BLUEZ_MANAGER_INTERFACE		BLUEZ_SERVICE ".Manager"
#define	BLUEZ_ADAPTER_INTERFACE		BLUEZ_SERVICE ".Adapter"
#define	BLUEZ_DEVICE_INTERFACE		BLUEZ_SERVICE ".Device"
#define	BLUEZ_GATEWAY_INTERFACE		BLUEZ_SERVICE ".HandsfreeGateway"

#define HFP_AGENT_INTERFACE "org.bluez.HandsfreeAgent"
#define HFP_AGENT_ERROR_INTERFACE "org.bluez.Error"

#define HFP_AG_UUID	"0000111F-0000-1000-8000-00805F9B34FB"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

static const char *brsf_prefix[] = { "+BRSF:", NULL };
static const char *cind_prefix[] = { "+CIND:", NULL };
static const char *cmer_prefix[] = { "+CMER:", NULL };
static const char *chld_prefix[] = { "+CHLD:", NULL };

static DBusConnection *connection;
static GHashTable *uuid_hash = NULL;

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

static void sevice_level_conn_established(struct ofono_modem *modem)
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

	ofono_modem_set_powered(modem, FALSE);

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

	sevice_level_conn_established(modem);
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
		sevice_level_conn_established(modem);
}

static int send_method_call(const char *dest, const char *path,
				const char *interface, const char *method,
				DBusPendingCallNotifyFunction cb,
				void *user_data, int type, ...)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	va_list args;

	msg = dbus_message_new_method_call(dest, path, interface, method);
	if (!msg) {
		ofono_error("Unable to allocate new D-Bus %s message", method);
		return -ENOMEM;
	}

	va_start(args, type);

	if (!dbus_message_append_args_valist(msg, type, args)) {
		dbus_message_unref(msg);
		va_end(args);
		return -EIO;
	}

	va_end(args);

	if (!cb) {
		g_dbus_send_message(connection, msg);
		return 0;
	}

	if (!dbus_connection_send_with_reply(connection, msg, &call, -1)) {
		ofono_error("Sending %s failed", method);
		dbus_message_unref(msg);
		return -EIO;
	}

	dbus_pending_call_set_notify(call, cb, user_data, NULL);
	dbus_pending_call_unref(call);
	dbus_message_unref(msg);

	return 0;
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

	ofono_modem_remove(modem);

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable agent_methods[] = {
	{ "NewConnection", "h", "", hfp_agent_new_connection,
		G_DBUS_METHOD_FLAG_ASYNC },
	{ "Release", "", "", hfp_agent_release },
	{ NULL, NULL, NULL, NULL }
};

static int hfp_create_modem(const char *device)
{
	struct ofono_modem *modem;
	struct hfp_data *data;
	const char *path;

	ofono_info("Using device: %s", device);

	modem = ofono_modem_create(NULL, "hfp");
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
	ofono_modem_register(modem);

	path = ofono_modem_get_path(modem);
	g_hash_table_insert(uuid_hash, g_strdup(device), g_strdup(path));

	return 0;

free:
	g_free(data);
	ofono_modem_remove(modem);

	return -ENOMEM;
}

static void parse_uuids(DBusMessageIter *i, const char *device)
{
	DBusMessageIter variant, ai;
	const char *value;

	if (g_hash_table_lookup(uuid_hash, device))
		return;

	dbus_message_iter_recurse(i, &variant);
	dbus_message_iter_recurse(&variant, &ai);

	while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
		dbus_message_iter_get_basic(&ai, &value);

		if (!strcasecmp(value, HFP_AG_UUID))
			hfp_create_modem(device);

		if (!dbus_message_iter_next(&ai))
			return;
	}
}

static void parse_get_properties(DBusMessage *reply, const char *device)
{
	DBusMessageIter arg, element, variant;
	const char *key;

	if (!dbus_message_iter_init(reply, &arg)) {
		ofono_debug("GetProperties reply has no arguments.");
		return;
	}

	if (dbus_message_iter_get_arg_type(&arg) != DBUS_TYPE_ARRAY) {
		ofono_debug("GetProperties argument is not an array.");
		return;
	}

	dbus_message_iter_recurse(&arg, &element);

	while (dbus_message_iter_get_arg_type(&element) != DBUS_TYPE_INVALID) {
		if (dbus_message_iter_get_arg_type(&element) ==
				DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter dict;

			dbus_message_iter_recurse(&element, &dict);

			if (dbus_message_iter_get_arg_type(&dict) !=
					DBUS_TYPE_STRING) {
				ofono_debug("Property name not a string.");
				return;
			}

			dbus_message_iter_get_basic(&dict, &key);

			if (!dbus_message_iter_next(&dict))  {
				ofono_debug("Property value missing");
				return;
			}

			if (dbus_message_iter_get_arg_type(&dict) !=
					DBUS_TYPE_VARIANT) {
				ofono_debug("Property value not a variant.");
				return;
			}

			if (!strcmp(key, "UUIDs"))
				parse_uuids(&dict, device);

			dbus_message_iter_recurse(&dict, &variant);
		}

		if (!dbus_message_iter_next(&element))
			return;
	}
}

static void get_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;
	const char *device = user_data;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		ofono_debug("Bluetooth daemon is apparently not available.");
		goto done;
	}

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		if (!dbus_message_is_error(reply, DBUS_ERROR_UNKNOWN_METHOD))
			ofono_info("Error from GetProperties reply: %s",
					dbus_message_get_error_name(reply));

		goto done;
	}

	parse_get_properties(reply, device);

done:
	dbus_message_unref(reply);
}

static void list_devices_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusError err;
	DBusMessage *reply;
	const char **device = NULL;
	int num, ret, i;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		ofono_debug("Bluetooth daemon is apparently not available.");
		goto done;
	}

	dbus_error_init(&err);

	if (dbus_message_get_args(reply, &err, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH, &device,
				&num, DBUS_TYPE_INVALID) == FALSE) {
		if (device == NULL) {
			dbus_error_free(&err);
			goto done;
		}

		if (dbus_error_is_set(&err) == TRUE) {
			ofono_error("%s", err.message);
			dbus_error_free(&err);
		}

		goto done;
	}

	for (i = 0 ; i < num ; i++) {
		ret = send_method_call(BLUEZ_SERVICE, device[i],
				BLUEZ_DEVICE_INTERFACE, "GetProperties",
				get_properties_cb, (void *)device[i],
				DBUS_TYPE_INVALID);
		if (ret < 0)
			ofono_error("GetProperties failed(%d)", ret);
	}

done:
	dbus_message_unref(reply);
}

static gboolean adapter_added(DBusConnection *connection, DBusMessage *message,
				void *user_data)
{
	const char *path;
	int ret;

	dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	ret = send_method_call(BLUEZ_SERVICE, path,
			BLUEZ_ADAPTER_INTERFACE, "ListDevices",
			list_devices_cb, NULL,
			DBUS_TYPE_INVALID);

	if (ret < 0)
		ofono_error("ListDevices failed(%d)", ret);

	return TRUE;
}

static gboolean uuid_emitted(DBusConnection *connection, DBusMessage *message,
				void *user_data)
{
	const char *device, *property;
	DBusMessageIter iter;

	dbus_message_iter_init(message, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return FALSE;

	dbus_message_iter_get_basic(&iter, &property);
	if (g_str_equal(property, "UUIDs") == FALSE)
		return TRUE;

	if (!dbus_message_iter_next(&iter))
		return FALSE;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return FALSE;

	device = dbus_message_get_path(message);
	parse_uuids(&iter, device);

	return TRUE;
}

static void list_adapters_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusError err;
	DBusMessage *reply;
	char **adapter = NULL;
	int num, ret, i;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		ofono_debug("Bluetooth daemon is apparently not available.");
		goto done;
	}

	dbus_error_init(&err);

	if (dbus_message_get_args(reply, &err, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH, &adapter,
				&num, DBUS_TYPE_INVALID) == FALSE) {
		if (adapter == NULL) {
			dbus_error_free(&err);
			goto done;
		}

		if (dbus_error_is_set(&err) == TRUE) {
			ofono_error("%s", err.message);
			dbus_error_free(&err);
		}

		goto done;
	}

	for (i = 0 ; i < num ; i++) {
		ret = send_method_call(BLUEZ_SERVICE, adapter[i],
				BLUEZ_ADAPTER_INTERFACE, "ListDevices",
				list_devices_cb, NULL,
				DBUS_TYPE_INVALID);

		if (ret < 0)
			ofono_error("ListDevices failed(%d)", ret);
	}

	g_strfreev(adapter);

done:
	dbus_message_unref(reply);
}

static int hfp_load_modems()
{
	return send_method_call(BLUEZ_SERVICE, "/",
				BLUEZ_MANAGER_INTERFACE, "ListAdapters",
				list_adapters_cb, NULL,
				DBUS_TYPE_INVALID);
}

static int hfp_register_ofono_handsfree(struct ofono_modem *modem)
{
	const char *obj_path = ofono_modem_get_path(modem);
	struct hfp_data *data = ofono_modem_get_data(modem);

	ofono_debug("Registering oFono Agent to bluetooth daemon");

	return send_method_call(BLUEZ_SERVICE, data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "RegisterAgent",
				NULL, NULL, DBUS_TYPE_OBJECT_PATH,
				&obj_path, DBUS_TYPE_INVALID);
}

static int hfp_unregister_ofono_handsfree(struct ofono_modem *modem)
{
	const char *obj_path = ofono_modem_get_path(modem);
	struct hfp_data *data = ofono_modem_get_data(modem);

	ofono_debug("Unregistering oFono Agent from bluetooth daemon");

	return send_method_call(BLUEZ_SERVICE, data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "UnregisterAgent",
				NULL, NULL, DBUS_TYPE_OBJECT_PATH,
				&obj_path, DBUS_TYPE_INVALID);
}

static int hfp_probe(struct ofono_modem *modem)
{
	const char *obj_path = ofono_modem_get_path(modem);

	g_dbus_register_interface(connection, obj_path, HFP_AGENT_INTERFACE,
			agent_methods, NULL, NULL, modem, NULL);

	if (hfp_register_ofono_handsfree(modem) != 0)
		return -EINVAL;

	return 0;
}

static void hfp_remove(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);

	hfp_unregister_ofono_handsfree(modem);

	g_hash_table_remove(uuid_hash, data->handsfree_path);

	g_free(data->handsfree_path);
	g_free(data);

	ofono_modem_set_data(modem, NULL);
}

static int hfp_connect_ofono_handsfree(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);

	ofono_debug("Connect to bluetooth daemon");

	return send_method_call(BLUEZ_SERVICE, data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "Connect",
				NULL, NULL, DBUS_TYPE_INVALID);
}

/* power up hardware */
static int hfp_enable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	if (hfp_connect_ofono_handsfree(modem) < 0)
		return -EINVAL;

	return -EINPROGRESS;
}

static int hfp_disconnect_ofono_handsfree(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);

	return send_method_call(BLUEZ_SERVICE, data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "Disconnect",
				NULL, NULL, DBUS_TYPE_INVALID);
}

static int hfp_disable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	clear_data(modem);

	hfp_disconnect_ofono_handsfree(modem);
	return 0;
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

static guint adapter_watch;
static guint uuid_watch;

static int hfp_init(void)
{
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	connection = ofono_dbus_get_connection();

	adapter_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_MANAGER_INTERFACE,
						"AdapterAdded",
						adapter_added, NULL, NULL);

	uuid_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_DEVICE_INTERFACE,
						"PropertyChanged",
						uuid_emitted, NULL, NULL);


	uuid_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, g_free);

	if (adapter_watch == 0 || uuid_watch == 0) {
		err = -EIO;
		goto remove;
	}

	err = ofono_modem_driver_register(&hfp_driver);
	if (err < 0)
		goto remove;

	hfp_load_modems();

	return 0;

remove:
	g_dbus_remove_watch(connection, adapter_watch);
	g_dbus_remove_watch(connection, uuid_watch);
	g_hash_table_destroy(uuid_hash);

	dbus_connection_unref(connection);

	return err;
}

static void hfp_exit(void)
{
	g_dbus_remove_watch(connection, adapter_watch);
	g_dbus_remove_watch(connection, uuid_watch);

	ofono_modem_driver_unregister(&hfp_driver);

	g_hash_table_destroy(uuid_hash);
}

OFONO_PLUGIN_DEFINE(hfp, "Hands-Free Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
