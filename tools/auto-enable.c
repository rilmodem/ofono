/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <gdbus.h>

#define OFONO_SERVICE	"org.ofono"

#define OFONO_MANAGER_INTERFACE		OFONO_SERVICE ".Manager"
#define OFONO_MODEM_INTERFACE		OFONO_SERVICE ".Modem"
#define OFONO_SIM_INTERFACE		OFONO_SERVICE ".SimManager"

struct modem_data {
	char *path;
	DBusConnection *conn;
	guint sim_changed_watch;
	dbus_bool_t has_powered;
	dbus_bool_t has_online;
	dbus_bool_t has_sim;
};

static GHashTable *modem_list;

static gboolean option_online = FALSE;

static void set_property_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError err;

	dbus_error_init(&err);

	if (dbus_set_error_from_message(&err, reply) == TRUE) {
		g_printerr("%s: %s\n", err.name, err.message);
		dbus_error_free(&err);
	}

	dbus_message_unref(reply);
}

static int set_property(struct modem_data *modem, const char *key,
						int type, const void *val)
{
	DBusConnection *conn = modem->conn;
	DBusMessage *msg;
	DBusMessageIter iter, value;
	DBusPendingCall *call;
	const char *signature;

	msg = dbus_message_new_method_call(OFONO_SERVICE, modem->path,
					OFONO_MODEM_INTERFACE, "SetProperty");
	if (msg == NULL)
		return -ENOMEM;

	dbus_message_set_auto_start(msg, FALSE);

	dbus_message_iter_init_append(msg, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key);

	switch (type) {
	case DBUS_TYPE_BOOLEAN:
		signature = DBUS_TYPE_BOOLEAN_AS_STRING;
		break;
	default:
		dbus_message_unref(msg);
		return -EINVAL;
	}

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
							signature, &value);
	dbus_message_iter_append_basic(&value, type, val);
	dbus_message_iter_close_container(&iter, &value);

	if (dbus_connection_send_with_reply(conn, msg, &call, -1) == FALSE) {
		dbus_message_unref(msg);
		return -EIO;
	}

	dbus_message_unref(msg);

	if (call == NULL)
		return -EINVAL;

	dbus_pending_call_set_notify(call, set_property_reply, modem, NULL);

	dbus_pending_call_unref(call);

	return 0;
}

static gboolean sim_changed(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	struct modem_data *modem = user_data;
	DBusMessageIter iter, value;
	const char *key;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &key);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	if (g_str_equal(key, "SubscriberIdentity") == FALSE)
		return TRUE;

	if (modem->has_online == FALSE) {
		dbus_bool_t online = TRUE;
		set_property(modem, "Online", DBUS_TYPE_BOOLEAN, &online);
	}

	return TRUE;
}

static void check_interfaces(struct modem_data *modem, DBusMessageIter *iter)
{
	DBusMessageIter entry;
	dbus_bool_t has_sim = FALSE;

	dbus_message_iter_recurse(iter, &entry);

	while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
		const char *interface;

		dbus_message_iter_get_basic(&entry, &interface);

		if (g_str_equal(interface, OFONO_SIM_INTERFACE) == TRUE)
			has_sim = TRUE;

		dbus_message_iter_next(&entry);
	}

	if (modem->has_sim == has_sim)
		return;

	modem->has_sim = has_sim;
}

static void check_property(struct modem_data *modem, const char *key,
						DBusMessageIter *value)
{
	if (g_str_equal(key, "Interfaces") == TRUE) {
		check_interfaces(modem, value);
		return;
	}

	if (g_str_equal(key, "Powered") == TRUE) {
		dbus_bool_t powered;

		dbus_message_iter_get_basic(value, &powered);

		if (powered == TRUE) {
			g_print("modem enabled (%s)\n", modem->path);

			modem->has_powered = TRUE;
		} else {
			g_print("modem disabled (%s)\n", modem->path);

			if (modem->has_powered == FALSE) {
				powered = TRUE;

				set_property(modem, "Powered",
						DBUS_TYPE_BOOLEAN, &powered);
			}
		}
	} else if (g_str_equal(key, "Online") == TRUE) {
		dbus_bool_t online;

		dbus_message_iter_get_basic(value, &online);

		if (online == TRUE) {
			g_print("modem online (%s)\n", modem->path);

			modem->has_online = TRUE;
		} else
			g_print("modem offline (%s)\n", modem->path);
	} else if (g_str_equal(key, "Lockdown") == TRUE) {
		dbus_bool_t lockdown;

		dbus_message_iter_get_basic(value, &lockdown);

		if (lockdown == TRUE)
			g_print("modem locked (%s)\n", modem->path);
		else
			g_print("modem unlocked (%s)\n", modem->path);
	}
}

static void destroy_modem(gpointer data)
{
	struct modem_data *modem = data;

	g_print("modem removed (%s)\n", modem->path);

	g_dbus_remove_watch(modem->conn, modem->sim_changed_watch);

	dbus_connection_unref(modem->conn);

	g_free(modem->path);
	g_free(modem);
}

static void create_modem(DBusConnection *conn,
				const char *path, DBusMessageIter *iter)
{
	struct modem_data *modem;
	DBusMessageIter dict;

	modem = g_try_new0(struct modem_data, 1);
	if (modem == NULL)
		return;

	modem->path = g_strdup(path);
	modem->conn = dbus_connection_ref(conn);

	modem->sim_changed_watch = g_dbus_add_signal_watch(conn,
				OFONO_SERVICE, NULL, OFONO_SIM_INTERFACE,
				"PropertyChanged", sim_changed, modem, NULL);

	g_hash_table_replace(modem_list, modem->path, modem);

	g_print("modem added (%s)\n", modem->path);

	dbus_message_iter_recurse(iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		check_property(modem, key, &value);

		dbus_message_iter_next(&dict);
	}
}

static gboolean modem_added(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	DBusMessageIter iter, dict;
	const char *path;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &path);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &dict);

	create_modem(conn, path, &iter);

	return TRUE;
}

static gboolean modem_removed(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	DBusMessageIter iter;
	const char *path;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &path);

	g_hash_table_remove(modem_list, path);

	return TRUE;
}

static gboolean modem_changed(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	struct modem_data *modem;
	DBusMessageIter iter, value;
	const char *path, *key;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	path = dbus_message_get_path(msg);

	modem = g_hash_table_lookup(modem_list, path);
	if (modem == NULL)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &key);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	check_property(modem, key, &value);

	return TRUE;
}

static void get_modems_reply(DBusPendingCall *call, void *user_data)
{
	DBusConnection *conn = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter iter, list;
	DBusError err;

	dbus_error_init(&err);

	if (dbus_set_error_from_message(&err, reply) == TRUE) {
		g_printerr("%s: %s\n", err.name, err.message);
		dbus_error_free(&err);
		goto done;
	}

	if (dbus_message_has_signature(reply, "a(oa{sv})") == FALSE)
		goto done;

	if (dbus_message_iter_init(reply, &iter) == FALSE)
		goto done;

	dbus_message_iter_recurse(&iter, &list);

	while (dbus_message_iter_get_arg_type(&list) == DBUS_TYPE_STRUCT) {
		DBusMessageIter entry, dict;
		const char *path;

		dbus_message_iter_recurse(&list, &entry);
		dbus_message_iter_get_basic(&entry, &path);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &dict);

		create_modem(conn, path, &entry);

		dbus_message_iter_next(&list);
	}

done:
	dbus_message_unref(reply);
}

static int get_modems(DBusConnection *conn)
{
	DBusMessage *msg;
	DBusPendingCall *call;

	msg = dbus_message_new_method_call(OFONO_SERVICE, "/",
					OFONO_MANAGER_INTERFACE, "GetModems");
	if (msg == NULL)
		return -ENOMEM;

	dbus_message_set_auto_start(msg, FALSE);

	g_print("getting modems\n");

	if (dbus_connection_send_with_reply(conn, msg, &call, -1) == FALSE) {
		dbus_message_unref(msg);
		return -EIO;
	}

	dbus_message_unref(msg);

	if (call == NULL)
		return -EINVAL;

	dbus_pending_call_set_notify(call, get_modems_reply, conn, NULL);

	dbus_pending_call_unref(call);

	return 0;
}

static gboolean ofono_running = FALSE;

static guint modem_added_watch;
static guint modem_removed_watch;
static guint modem_changed_watch;

static void ofono_connect(DBusConnection *conn, void *user_data)
{
	g_print("starting telephony interface\n");

	ofono_running = TRUE;

	modem_list = g_hash_table_new_full(g_str_hash, g_str_equal,
						NULL, destroy_modem);

	modem_added_watch = g_dbus_add_signal_watch(conn, OFONO_SERVICE,
				NULL, OFONO_MANAGER_INTERFACE, "ModemAdded",
				modem_added, NULL, NULL);
	modem_removed_watch = g_dbus_add_signal_watch(conn, OFONO_SERVICE,
				NULL, OFONO_MANAGER_INTERFACE, "ModemRemoved",
				modem_removed, NULL, NULL);
	modem_changed_watch = g_dbus_add_signal_watch(conn, OFONO_SERVICE,
				NULL, OFONO_MODEM_INTERFACE, "PropertyChanged",
				modem_changed, NULL, NULL);

	get_modems(conn);
}

static void ofono_disconnect(DBusConnection *conn, void *user_data)
{
	g_print("stopping telephony interface\n");

	ofono_running = FALSE;

	g_dbus_remove_watch(conn, modem_added_watch);
	modem_added_watch = 0;
	g_dbus_remove_watch(conn, modem_removed_watch);
	modem_removed_watch = 0;
	g_dbus_remove_watch(conn, modem_changed_watch);
	modem_changed_watch = 0;

	g_hash_table_destroy(modem_list);
	modem_list = NULL;
}

static GMainLoop *main_loop = NULL;

static volatile sig_atomic_t __terminated = 0;

static void sig_term(int sig)
{
	if (__terminated > 0)
		return;

	__terminated = 1;

	g_print("Terminating\n");

	g_main_loop_quit(main_loop);
}

static void disconnect_callback(DBusConnection *conn, void *user_data)
{
	g_printerr("D-Bus disconnect\n");

	g_main_loop_quit(main_loop);
}

static gboolean option_version = FALSE;

static GOptionEntry options[] = {
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &option_version,
				"Show version information and exit" },
	{ "online", 'o', 0, G_OPTION_ARG_NONE, &option_online,
				"Bring device online if possible" },
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	DBusConnection *conn;
	DBusError err;
	guint watch;
	struct sigaction sa;

#ifdef NEED_THREADS
	if (g_thread_supported() == FALSE)
		g_thread_init(NULL);
#endif

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &error) == FALSE) {
		if (error != NULL) {
			g_printerr("%s\n", error->message);
			g_error_free(error);
		} else
			g_printerr("An unknown error occurred\n");
		exit(1);
	}

	g_option_context_free(context);

	if (option_version == TRUE) {
		printf("%s\n", VERSION);
		exit(0);
	}

	main_loop = g_main_loop_new(NULL, FALSE);

#ifdef NEED_THREADS
	if (dbus_threads_init_default() == FALSE) {
		fprintf(stderr, "Can't init usage of threads\n");
		exit(1);
	}
#endif

	dbus_error_init(&err);

	conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, &err);
	if (conn == NULL) {
		if (dbus_error_is_set(&err) == TRUE) {
			fprintf(stderr, "%s\n", err.message);
			dbus_error_free(&err);
		} else
			fprintf(stderr, "Can't register with system bus\n");
		exit(1);
	}

	g_dbus_set_disconnect_function(conn, disconnect_callback, NULL, NULL);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_term;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	watch = g_dbus_add_service_watch(conn, OFONO_SERVICE,
				ofono_connect, ofono_disconnect, NULL, NULL);

	g_main_loop_run(main_loop);

	g_dbus_remove_watch(conn, watch);

	if (ofono_running == TRUE)
		ofono_disconnect(conn, NULL);

	dbus_connection_unref(conn);

	g_main_loop_unref(main_loop);

	return 0;
}
