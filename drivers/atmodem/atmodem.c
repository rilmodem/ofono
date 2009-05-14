/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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
#include <gatchat.h>

#include <ofono/plugin.h>
#include <ofono/log.h>
#include "driver.h"

#include "at.h"
#include "dbus-gsm.h"
#include "modem.h"
#include "session.h"

#define AT_MANAGER_INTERFACE "org.ofono.at.Manager"

static GSList *g_sessions = NULL;
static GSList *g_pending = NULL;

static void modem_list(char ***modems)
{
	GSList *l;
	int i;
	struct at_data *at;

	*modems = g_new0(char *, g_slist_length(g_sessions) + 1);

	for (l = g_sessions, i = 0; l; l = l->next, i++) {
		at = l->data;

		(*modems)[i] = at->modem->path;
	}
}

void dump_response(const char *func, gboolean ok, GAtResult *result)
{
	GSList *l;

	ofono_debug("%s got result: %d", func, ok);
	ofono_debug("Final response: %s", result->final_or_pdu);

	for (l = result->lines; l; l = l->next)
		ofono_debug("Response line: %s", (char *) l->data);
}

void decode_at_error(struct ofono_error *error, const char *final)
{
	if (!strcmp(final, "OK")) {
		error->type = OFONO_ERROR_TYPE_NO_ERROR;
		error->error = 0;
	} else {
		error->type = OFONO_ERROR_TYPE_FAILURE;
		error->error = 0;
	}
}

static void at_destroy(struct at_data *at)
{
	if (at->parser)
		g_at_chat_unref(at->parser);

	if (at->driver)
		g_free(at->driver);

	g_free(at);
}

static void manager_free(gpointer user)
{
	GSList *l;

	for (l = g_pending; l; l = l->next)
		g_io_channel_unref(l->data);

	g_slist_free(g_pending);

	for (l = g_sessions; l; l = l->next) {
		struct at_data *at = l->data;

		at_call_forwarding_exit(at->modem);
		at_call_waiting_exit(at->modem);
		at_call_settings_exit(at->modem);
		at_network_registration_exit(at->modem);
		at_voicecall_exit(at->modem);
		at_ussd_exit(at->modem);
		at_call_meter_exit(at->modem);
		at_call_barring_exit(at->modem);
		ofono_modem_unregister(at->modem);

		at_destroy(at);
	}

	g_slist_free(g_sessions);
}

struct attr_cb_info {
	ofono_modem_attribute_query_cb_t cb;
	void *data;
	const char *prefix;
};

static inline struct attr_cb_info *attr_cb_info_new(ofono_modem_attribute_query_cb_t cb,
							void *data,
							const char *prefix)
{
	struct attr_cb_info *ret;

	ret = g_try_new(struct attr_cb_info, 1);

	if (!ret)
		return ret;

	ret->cb = cb;
	ret->data = data;
	ret->prefix = prefix;

	return ret;
}

static const char *fixup_return(const char *line, const char *prefix)
{
	if (g_str_has_prefix(line, prefix) == FALSE)
		return line;

	line = line + strlen(prefix);

	while (line[0] == ' ')
		line++;

	return line;
}

static void attr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_error error;
	struct attr_cb_info *info = user_data;

	decode_at_error(&error, g_at_result_final_response(result));

	dump_response("attr_cb", ok, result);

	if (ok) {
		GAtResultIter iter;
		const char *line;
		int i;

		g_at_result_iter_init(&iter, result);

		/* We have to be careful here, sometimes a stray unsolicited
		 * notification will appear as part of the response and we
		 * cannot rely on having a prefix to recognize the actual
		 * response line.  So use the last line only as the response
		 */
		for (i = 0; i < g_at_result_num_response_lines(result); i++)
			g_at_result_iter_next(&iter, NULL);

		line = g_at_result_iter_raw_line(&iter);

		info->cb(&error, fixup_return(line, info->prefix), info->data);
	} else
		info->cb(&error, "", info->data);
}

static void at_query_manufacturer(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb, void *data)
{
	struct attr_cb_info *info = attr_cb_info_new(cb, data, "+CGMI:");
	struct at_data *at = ofono_modem_userdata(modem);

	if (!info)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CGMI", NULL,
				attr_cb, info, g_free) > 0)
		return;

error:
	if (info)
		g_free(info);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static void at_query_model(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb, void *data)
{
	struct attr_cb_info *info = attr_cb_info_new(cb, data, "+CGMM:");
	struct at_data *at = ofono_modem_userdata(modem);

	if (!info)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CGMM", NULL,
				attr_cb, info, g_free) > 0)
		return;

error:
	if (info)
		g_free(info);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static void at_query_revision(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb, void *data)
{
	struct attr_cb_info *info = attr_cb_info_new(cb, data, "+CGMR:");
	struct at_data *at = ofono_modem_userdata(modem);

	if (!info)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CGMR", NULL,
				attr_cb, info, g_free) > 0)
		return;

error:
	if (info)
		g_free(info);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static void at_query_serial(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb, void *data)
{
	struct attr_cb_info *info = attr_cb_info_new(cb, data, "+CGSN:");
	struct at_data *at = ofono_modem_userdata(modem);

	if (!info)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CGSN", NULL,
				attr_cb, info, g_free) > 0)
		return;

error:
	if (info)
		g_free(info);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static void send_init_commands(const char *vendor, GAtChat *parser)
{
	if (!strcmp(vendor, "ti_calypso")) {
		g_at_chat_set_wakeup_command(parser, "\r", 1000, 5000);

		g_at_chat_send(parser, "AT%CUNS=0", NULL,
					NULL, NULL, NULL);
	}
}

static struct ofono_modem_attribute_ops ops = {
	.query_manufacturer = at_query_manufacturer,
	.query_model = at_query_model,
	.query_revision = at_query_revision,
	.query_serial = at_query_serial
};

static void msg_destroy(gpointer user)
{
	DBusMessage *msg = user;

	dbus_message_unref(msg);
}

static void create_cb(GIOChannel *io, gboolean success, gpointer user)
{
	DBusConnection *conn = dbus_gsm_connection();
	DBusMessage *msg = user;
	DBusMessage *reply;
	struct at_data *at = NULL;
	const char *path;
	const char *target, *driver;
	char **modems;

	if (success == FALSE)
		goto out;

	at = g_new0(struct at_data, 1);

	at->parser = g_at_chat_new(io, 0);

	if (!at->parser)
		goto out;

	ofono_debug("Seting up AT channel");

	dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &target,
				DBUS_TYPE_STRING, &driver, DBUS_TYPE_INVALID);

	send_init_commands(driver, at->parser);

	at->modem = ofono_modem_register(&ops);

	if (!at->modem)
		goto out;

	ofono_modem_set_userdata(at->modem, at);

	at_ussd_init(at->modem);
	at_call_forwarding_init(at->modem);
	at_call_settings_init(at->modem);
	at_call_waiting_init(at->modem);
	at_network_registration_init(at->modem);
	at_voicecall_init(at->modem);
	at_call_meter_init(at->modem);
	at_call_barring_init(at->modem);

	at->io = io;
	at->driver = g_strdup(driver);

	g_pending = g_slist_remove(g_pending, io);
	g_sessions = g_slist_prepend(g_sessions, at);

	path = at->modem->path;

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);

	g_dbus_send_message(conn, reply);

	modem_list(&modems);
	dbus_gsm_signal_array_property_changed(conn, "/", AT_MANAGER_INTERFACE,
						"Modems", DBUS_TYPE_OBJECT_PATH,
						&modems);
	g_free(modems);

	return;

out:
	if (at)
		at_destroy(at);

	reply = dbus_gsm_failed(msg);
	g_dbus_send_message(conn, reply);
}

static DBusMessage *manager_create(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	const char *target;
	const char *driver;
	GIOChannel *io;

	if (!dbus_message_get_args(msg, NULL,
					DBUS_TYPE_STRING, &target,
					DBUS_TYPE_STRING, &driver,
					DBUS_TYPE_INVALID))
		return dbus_gsm_invalid_args(msg);

	io = modem_session_create(target, create_cb, msg, msg_destroy);

	if (!io)
		return dbus_gsm_invalid_format(msg);

	dbus_message_ref(msg);

	g_pending = g_slist_prepend(g_pending, io);

	return NULL;
}

static DBusMessage *manager_destroy(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	const char *path;
	GSList *l;

	if (!dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID))
		return dbus_gsm_invalid_args(msg);

	for (l = g_sessions; l; l = l->next) {
		struct at_data *at = l->data;
		char **modems;

		if (strcmp(at->modem->path, path))
			continue;

		at_network_registration_exit(at->modem);
		at_voicecall_exit(at->modem);
		ofono_modem_unregister(at->modem);

		g_sessions = g_slist_remove(g_sessions, at);
		at_destroy(at);

		modem_list(&modems);
		dbus_gsm_signal_array_property_changed(conn, "/",
						AT_MANAGER_INTERFACE,
						"Modems", DBUS_TYPE_OBJECT_PATH,
						&modems);
		g_free(modems);

		return dbus_message_new_method_return(msg);
	}

	return dbus_gsm_not_found(msg);
}

static DBusMessage *manager_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessage *reply;
	char **modems;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	modem_list(&modems);

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter,  DBUS_TYPE_ARRAY,
					PROPERTIES_ARRAY_SIGNATURE, &dict);

	dbus_gsm_dict_append_array(&dict, "Modems", DBUS_TYPE_OBJECT_PATH,
					&modems);

	g_free(modems);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable manager_methods[] = {
	{ "Create",		"ss",	"o",		manager_create,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "Destroy",		"o",	"",		manager_destroy },
	{ "GetProperties",	"",	"a{sv}",	manager_get_properties },
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static int manager_init(DBusConnection *conn)
{
	if (g_dbus_register_interface(conn, "/", AT_MANAGER_INTERFACE,
					manager_methods, manager_signals,
					NULL, NULL, manager_free) == FALSE)
		return -1;

	return 0;
}

static void manager_exit(DBusConnection *conn)
{
	g_dbus_unregister_interface(conn, "/", AT_MANAGER_INTERFACE);
}

static int atmodem_init(void)
{
	DBusConnection *conn = dbus_gsm_connection();

	manager_init(conn);

	return 0;
}

static void atmodem_exit(void)
{
	DBusConnection *conn = dbus_gsm_connection();

	manager_exit(conn);
}

OFONO_PLUGIN_DEFINE(atmodem, "AT modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, atmodem_init, atmodem_exit)
