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
static GHashTable *uuid_hash = NULL;
static GHashTable *adapter_address_hash;

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

/*
 * FIXME: Group all agent stuff DBus calls in bluetooth.c
 * then we can reuse common agent code for all Bluetooth plugins.
 * That will remove this function from hfp.c
 */
static int send_method_call_with_reply(const char *dest, const char *path,
				const char *interface, const char *method,
				DBusPendingCallNotifyFunction cb,
				void *user_data, DBusFreeFunction free_func,
				int timeout, int type, ...)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	va_list args;
	int err;

	msg = dbus_message_new_method_call(dest, path, interface, method);
	if (!msg) {
		ofono_error("Unable to allocate new D-Bus %s message", method);
		err = -ENOMEM;
		goto fail;
	}

	va_start(args, type);

	if (!dbus_message_append_args_valist(msg, type, args)) {
		va_end(args);
		err = -EIO;
		goto fail;
	}

	va_end(args);

	if (timeout > 0)
		timeout *=1000;

	if (!dbus_connection_send_with_reply(connection, msg, &call, timeout)) {
		ofono_error("Sending %s failed", method);
		err = -EIO;
		goto fail;
	}

	dbus_pending_call_set_notify(call, cb, user_data, free_func);
	dbus_pending_call_unref(call);
	dbus_message_unref(msg);

	return 0;

fail:
	if (free_func && user_data)
		free_func(user_data);

	if (msg)
		dbus_message_unref(msg);

	return err;
}

typedef void (*PropertyHandler)(DBusMessageIter *iter, gpointer user_data);

struct property_handler {
	const char *property;
	PropertyHandler callback;
	gpointer user_data;
};

static gint property_handler_compare(gconstpointer a, gconstpointer b)
{
	const struct property_handler *handler = a;
	const char *property = b;

	return strcmp(handler->property, property);
}

static void parse_properties_reply(DBusMessage *reply,
					const char *property, ...)
{
	va_list args;
	GSList *prop_handlers = NULL;
	DBusMessageIter array, dict;

	va_start(args, property);

	while (property != NULL) {
		struct property_handler *handler =
					g_new0(struct property_handler, 1);

		handler->property = property;
		handler->callback = va_arg(args, PropertyHandler);
		handler->user_data = va_arg(args, gpointer);

		property = va_arg(args, const char *);

		prop_handlers = g_slist_prepend(prop_handlers, handler);
	}

	va_end(args);

	if (dbus_message_iter_init(reply, &array) == FALSE)
		goto done;

	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY)
		goto done;

	dbus_message_iter_recurse(&array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;
		GSList *l;

		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			goto done;

		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			goto done;

		dbus_message_iter_recurse(&entry, &value);

		l = g_slist_find_custom(prop_handlers, key,
					property_handler_compare);

		if (l) {
			struct property_handler *handler = l->data;

			handler->callback(&value, handler->user_data);
		}

		dbus_message_iter_next(&dict);
	}

done:
	g_slist_foreach(prop_handlers, (GFunc)g_free, NULL);
	g_slist_free(prop_handlers);
}

static void parse_string(DBusMessageIter *iter, gpointer user_data)
{
	char **str = user_data;
	int arg_type = dbus_message_iter_get_arg_type(iter);

	if (arg_type != DBUS_TYPE_OBJECT_PATH && arg_type != DBUS_TYPE_STRING)
		return;

	dbus_message_iter_get_basic(iter, str);
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

	g_hash_table_insert(uuid_hash, g_strdup(device), modem);

	return 0;

free:
	g_free(data);
	ofono_modem_remove(modem);

	return -ENOMEM;
}

static void has_hfp_uuid(DBusMessageIter *array, gpointer user_data)
{
	gboolean *hfp = user_data;
	DBusMessageIter value;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
		const char *uuid;

		dbus_message_iter_get_basic(&value, &uuid);

		if (!strcasecmp(uuid, HFP_AG_UUID)) {
			*hfp = TRUE;
			return;
		}

		dbus_message_iter_next(&value);
	}
}

static void device_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;
	char *path = user_data;
	gboolean have_hfp = FALSE;
	const char *adapter = NULL;
	const char *adapter_addr = NULL;
	const char *device_addr = NULL;
	const char *alias = NULL;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		if (!dbus_message_is_error(reply, DBUS_ERROR_UNKNOWN_METHOD))
			ofono_info("Error from GetProperties reply: %s",
					dbus_message_get_error_name(reply));

		goto done;
	}

	parse_properties_reply(reply, "UUIDs", has_hfp_uuid, &have_hfp,
				"Adapter", parse_string, &adapter,
				"Address", parse_string, &device_addr,
				"Alias", parse_string, &alias, NULL);

	if (adapter)
		adapter_addr = g_hash_table_lookup(adapter_address_hash,
							adapter);

	if (have_hfp && device_addr && adapter_addr)
		hfp_create_modem(path, device_addr, adapter_addr, alias);

done:
	dbus_message_unref(reply);
}

static void parse_devices(DBusMessageIter *array, gpointer user_data)
{
	DBusMessageIter value;
	GSList **device_list = user_data;

	DBG("");

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value)
			== DBUS_TYPE_OBJECT_PATH) {
		const char *path;

		dbus_message_iter_get_basic(&value, &path);

		*device_list = g_slist_prepend(*device_list, (gpointer) path);

		dbus_message_iter_next(&value);
	}
}

static void adapter_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	const char *path = user_data;
	DBusMessage *reply;
	GSList *device_list = NULL;
	GSList *l;
	const char *addr;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	parse_properties_reply(reply, "Devices", parse_devices, &device_list,
				"Address", parse_string, &addr, NULL);

	DBG("Adapter Address: %s, Path: %s", addr, path);
	g_hash_table_insert(adapter_address_hash,
				g_strdup(path), g_strdup(addr));

	for (l = device_list; l; l = l->next) {
		const char *device = l->data;

		send_method_call_with_reply(BLUEZ_SERVICE, device,
				BLUEZ_DEVICE_INTERFACE, "GetProperties",
				device_properties_cb, g_strdup(device), g_free,
				-1, DBUS_TYPE_INVALID);
	}

done:
	g_slist_free(device_list);
	dbus_message_unref(reply);
}

static gboolean adapter_added(DBusConnection *connection, DBusMessage *message,
				void *user_data)
{
	const char *path;
	int ret;

	dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	ret = send_method_call_with_reply(BLUEZ_SERVICE, path,
			BLUEZ_ADAPTER_INTERFACE, "GetProperties",
			adapter_properties_cb, g_strdup(path), g_free,
			-1, DBUS_TYPE_INVALID);

	return TRUE;
}

static gboolean adapter_removed(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	const char *path;

	if (dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID) == TRUE)
		g_hash_table_remove(adapter_address_hash, path);

	return TRUE;
}

static gboolean property_changed(DBusConnection *connection, DBusMessage *msg,
				void *user_data)
{
	const char *property;
	DBusMessageIter iter;

	dbus_message_iter_init(msg, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return FALSE;

	dbus_message_iter_get_basic(&iter, &property);
	if (g_str_equal(property, "UUIDs") == TRUE) {
		gboolean have_hfp = FALSE;
		const char *path = dbus_message_get_path(msg);
		DBusMessageIter variant;

		/* We already have this device in our hash, ignore */
		if (g_hash_table_lookup(uuid_hash, path) != NULL)
			return TRUE;

		if (!dbus_message_iter_next(&iter))
			return FALSE;

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return FALSE;

		dbus_message_iter_recurse(&iter, &variant);

		has_hfp_uuid(&variant, &have_hfp);

		/* We need the full set of properties to be able to create
		 * the modem properly, including Adapter and Alias, so
		 * refetch everything again
		 */
		if (have_hfp)
			send_method_call_with_reply(BLUEZ_SERVICE, path,
				BLUEZ_DEVICE_INTERFACE, "GetProperties",
				device_properties_cb, g_strdup(path), g_free,
				-1, DBUS_TYPE_INVALID);
	} else if (g_str_equal(property, "Alias") == TRUE) {
		const char *path = dbus_message_get_path(msg);
		struct ofono_modem *modem =
			g_hash_table_lookup(uuid_hash, path);
		const char *alias = NULL;
		DBusMessageIter variant;

		if (modem == NULL)
			return TRUE;

		if (!dbus_message_iter_next(&iter))
			return FALSE;

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return FALSE;

		dbus_message_iter_recurse(&iter, &variant);

		parse_string(&variant, &alias);

		ofono_modem_set_name(modem, alias);
	}

	return TRUE;
}

static void parse_adapters(DBusMessageIter *array, gpointer user_data)
{
	DBusMessageIter value;

	DBG("");

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value)
			== DBUS_TYPE_OBJECT_PATH) {
		const char *path;

		dbus_message_iter_get_basic(&value, &path);

		DBG("Calling GetProperties on %s", path);

		send_method_call_with_reply(BLUEZ_SERVICE, path,
				BLUEZ_ADAPTER_INTERFACE, "GetProperties",
				adapter_properties_cb, g_strdup(path), g_free,
				-1, DBUS_TYPE_INVALID);

		dbus_message_iter_next(&value);
	}
}

static void manager_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	parse_properties_reply(reply, "Adapters", parse_adapters, NULL, NULL);

done:
	dbus_message_unref(reply);
}

static gboolean hfp_remove_each_modem(gpointer key, gpointer value, gpointer user_data)
{
	struct ofono_modem *modem = value;

	ofono_modem_remove(modem);

	return TRUE;
}

static void bluetooth_disconnect(DBusConnection *connection, void *user_data)
{
	if (uuid_hash == NULL)
		return;

	g_hash_table_foreach_remove(uuid_hash, hfp_remove_each_modem, NULL);
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

	g_hash_table_remove(uuid_hash, data->handsfree_path);

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

	status = send_method_call_with_reply(BLUEZ_SERVICE,
				data->handsfree_path,
				BLUEZ_GATEWAY_INTERFACE, "Connect",
				hfp_connect_reply, modem, NULL,
				15, DBUS_TYPE_INVALID);

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
		status = send_method_call_with_reply(BLUEZ_SERVICE,
					data->handsfree_path,
					BLUEZ_GATEWAY_INTERFACE, "Disconnect",
					hfp_power_down, modem, NULL, 15,
					DBUS_TYPE_INVALID);

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

static guint bluetooth_exit_watch;
static guint adapter_added_watch;
static guint adapter_removed_watch;
static guint uuid_watch;

static int hfp_init()
{
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	connection = ofono_dbus_get_connection();

	bluetooth_exit_watch = g_dbus_add_service_watch(connection, BLUEZ_SERVICE,
			NULL, bluetooth_disconnect, NULL, NULL);

	adapter_added_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_MANAGER_INTERFACE,
						"AdapterAdded",
						adapter_added, NULL, NULL);

	adapter_removed_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_MANAGER_INTERFACE,
						"AdapterRemoved",
						adapter_removed, NULL, NULL);

	uuid_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_DEVICE_INTERFACE,
						"PropertyChanged",
						property_changed, NULL, NULL);

	if (bluetooth_exit_watch == 0 || adapter_added_watch == 0 ||
			adapter_removed_watch == 0|| uuid_watch == 0) {
		err = -EIO;
		goto remove;
	}

	uuid_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	adapter_address_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);

	err = ofono_modem_driver_register(&hfp_driver);
	if (err < 0)
		goto remove;

	send_method_call_with_reply(BLUEZ_SERVICE, "/",
				BLUEZ_MANAGER_INTERFACE, "GetProperties",
				manager_properties_cb, NULL, NULL, -1,
				DBUS_TYPE_INVALID);

	return 0;

remove:
	g_dbus_remove_watch(connection, bluetooth_exit_watch);
	g_dbus_remove_watch(connection, adapter_added_watch);
	g_dbus_remove_watch(connection, adapter_removed_watch);
	g_dbus_remove_watch(connection, uuid_watch);

	if (uuid_hash)
		g_hash_table_destroy(uuid_hash);

	if (adapter_address_hash)
		g_hash_table_destroy(adapter_address_hash);

	return err;
}

static void hfp_exit()
{
	g_dbus_remove_watch(connection, bluetooth_exit_watch);
	g_dbus_remove_watch(connection, adapter_added_watch);
	g_dbus_remove_watch(connection, adapter_removed_watch);
	g_dbus_remove_watch(connection, uuid_watch);

	ofono_modem_driver_unregister(&hfp_driver);

	g_hash_table_destroy(uuid_hash);
	g_hash_table_destroy(adapter_address_hash);
}

OFONO_PLUGIN_DEFINE(hfp, "Hands-Free Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
