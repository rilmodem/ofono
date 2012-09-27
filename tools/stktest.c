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
#include <netinet/in.h>
#include <arpa/inet.h>

#include <gdbus.h>
#include <gatchat/gatserver.h>

#define OFONO_SERVICE	"org.ofono"
#define STKTEST_PATH	"/stktest"
#define OFONO_MANAGER_INTERFACE		OFONO_SERVICE ".Manager"
#define OFONO_MODEM_INTERFACE		OFONO_SERVICE ".Modem"
#define OFONO_STK_INTERFACE		OFONO_SERVICE ".SimToolkit"

#define LISTEN_PORT	12765

static GMainLoop *main_loop = NULL;
static volatile sig_atomic_t __terminated = 0;

/* DBus related */
static DBusConnection *conn;
static gboolean ofono_running = FALSE;
static guint modem_changed_watch;
static gboolean stk_ready = FALSE;

/* Emulator setup */
static guint server_watch;
static GAtServer *emulator;

/* Emulated modem state variables */
static int modem_mode = 0;

static gboolean create_tcp(void);

static void server_debug(const char *str, void *data)
{
	g_print("%s: %s\n", (char *) data, str);
}

static void cgmi_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_info(server, "oFono", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cgmm_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_info(server, "oFono pre-1.0", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cgmr_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[256];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		sprintf(buf, "oFono pre-1.0 version: %s", VERSION);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cgsn_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_info(server, "123456789", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static gboolean send_ok(gpointer user)
{
	GAtServer *server = user;

	g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

	return FALSE;
}

static void cfun_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[12];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CFUN: (0-1,4)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		snprintf(buf, sizeof(buf), "+CFUN: %d", modem_mode);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int mode;

		g_at_result_iter_init(&iter, cmd);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &mode) == FALSE)
			goto error;

		if (mode != 0 && mode != 1)
			goto error;

		if (modem_mode == mode) {
			g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
			break;
		}

		modem_mode = mode;
		g_timeout_add_seconds(1, send_ok, server);
		break;
	}
	default:
		goto error;
	};

	return;

error:
	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void listen_again(gpointer user_data)
{
	if (create_tcp() == TRUE)
		return;

	g_print("Error listening to socket\n");
	g_main_loop_quit(main_loop);
}

static void setup_emulator(GAtServer *server)
{
	g_at_server_set_debug(server, server_debug, "Server");

	g_at_server_register(server, "+CGMI",    cgmi_cb,    NULL, NULL);
	g_at_server_register(server, "+CGMM",    cgmm_cb,    NULL, NULL);
	g_at_server_register(server, "+CGMR",    cgmr_cb,    NULL, NULL);
	g_at_server_register(server, "+CGSN",    cgsn_cb,    NULL, NULL);
	g_at_server_register(server, "+CFUN",    cfun_cb,    NULL, NULL);

	g_at_server_set_disconnect_function(server, listen_again, NULL);
}

static gboolean on_socket_connected(GIOChannel *chan, GIOCondition cond,
							gpointer user)
{
	struct sockaddr saddr;
	unsigned int len = sizeof(saddr);
	int fd;
	GIOChannel *client_io = NULL;

	if (cond != G_IO_IN)
		goto error;

	fd = accept(g_io_channel_unix_get_fd(chan), &saddr, &len);
	if (fd == -1)
		goto error;

	client_io = g_io_channel_unix_new(fd);

	emulator = g_at_server_new(client_io);
	g_io_channel_unref(client_io);

	if (emulator == NULL)
		goto error;

	setup_emulator(emulator);

error:
	server_watch = 0;
	return FALSE;
}

static gboolean create_tcp(void)
{
	struct sockaddr_in addr;
	int sk;
	int reuseaddr = 1;
	GIOChannel *server_io;

	sk = socket(PF_INET, SOCK_STREAM, 0);
	if (sk < 0) {
		g_print("Can't create tcp/ip socket: %s (%d)\n",
						strerror(errno), errno);
		return FALSE;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(LISTEN_PORT);

	setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
	if (bind(sk, (struct sockaddr *) &addr, sizeof(struct sockaddr)) < 0) {
		g_print("Can't bind socket: %s (%d)", strerror(errno), errno);
		close(sk);
		return FALSE;
	}

	if (listen(sk, 1) < 0) {
		g_print("Can't listen on socket: %s (%d)",
						strerror(errno), errno);
		close(sk);
		return FALSE;
	}

	g_print("new tcp is created at tcp port %d\n", LISTEN_PORT);

	server_io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(server_io, TRUE);

	server_watch = g_io_add_watch_full(server_io,
				G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, NULL, NULL);

	g_io_channel_unref(server_io);

	return TRUE;
}

static gboolean has_stk_interface(DBusMessageIter *iter)
{
	DBusMessageIter entry;

	dbus_message_iter_recurse(iter, &entry);

	while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
		const char *interface;

		dbus_message_iter_get_basic(&entry, &interface);

		if (g_str_equal(interface, OFONO_STK_INTERFACE) == TRUE)
			return TRUE;

		dbus_message_iter_next(&entry);
	}

	return FALSE;
}

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

static int set_property(const char *key, int type, const void *val,
				DBusPendingCallNotifyFunction notify,
				gpointer user_data,
				DBusFreeFunction destroy)
{
	DBusMessage *msg;
	DBusMessageIter iter, value;
	DBusPendingCall *call;
	const char *signature;

	msg = dbus_message_new_method_call(OFONO_SERVICE, STKTEST_PATH,
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

	dbus_pending_call_set_notify(call, notify, user_data, destroy);

	dbus_pending_call_unref(call);

	return 0;
}

static gboolean modem_changed(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	DBusMessageIter iter, value;
	const char *path, *key;
	gboolean has_stk;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	path = dbus_message_get_path(msg);

	if (g_str_equal(STKTEST_PATH, path) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &key);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	if (g_str_equal(key, "Interfaces") == FALSE)
		return TRUE;

	has_stk = has_stk_interface(&value);

	if (stk_ready && has_stk == FALSE) {
		stk_ready = FALSE;
		g_print("Lost STK interface\n");
	} else if (stk_ready == FALSE && has_stk == TRUE) {
		stk_ready = TRUE;
		g_print("Gained STK interface\n");
	}

	return TRUE;
}

static void powerup(void)
{
	dbus_bool_t powered = TRUE;
	set_property("Powered", DBUS_TYPE_BOOLEAN, &powered,
			set_property_reply, NULL, NULL);
}

static void get_modems_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter iter, list;
	DBusError err;
	gboolean found = FALSE;

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
		DBusMessageIter entry;
		const char *path;

		dbus_message_iter_recurse(&list, &entry);
		dbus_message_iter_get_basic(&entry, &path);

		if (g_str_equal(path, STKTEST_PATH))
			found = TRUE;

		dbus_message_iter_next(&list);
	}

done:
	dbus_message_unref(reply);

	if (found == FALSE) {
		g_printerr("STK Test modem not found\n");
		g_main_loop_quit(main_loop);
		return;
	}

	g_print("Test modem found\n");

	modem_changed_watch = g_dbus_add_signal_watch(conn, OFONO_SERVICE,
							STKTEST_PATH,
							OFONO_MODEM_INTERFACE,
							"PropertyChanged",
							modem_changed,
							NULL, NULL);

	if (create_tcp() == FALSE) {
		g_printerr("Unable to listen on modem emulator socket\n");
		g_main_loop_quit(main_loop);
	}

	powerup();
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

static void ofono_connect(DBusConnection *conn, void *user_data)
{
	g_print("starting telephony interface\n");

	ofono_running = TRUE;

	get_modems(conn);
}

static void ofono_disconnect(DBusConnection *conn, void *user_data)
{
	g_print("stopping telephony interface\n");

	ofono_running = FALSE;

	g_dbus_remove_watch(conn, modem_changed_watch);
	modem_changed_watch = 0;

	if (server_watch) {
		g_source_remove(server_watch);
		server_watch = 0;
	}

	g_at_server_unref(emulator);
	emulator = NULL;
}

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
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	DBusError err;
	guint watch;
	struct sigaction sa;

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
