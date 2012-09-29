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

#include "unit/stk-test-data.h"

#define OFONO_SERVICE	"org.ofono"
#define STKTEST_PATH	"/stktest"
#define STKTEST_ERROR	"org.ofono.stktest.Error"
#define OFONO_ERROR	"org.ofono.Error"
#define OFONO_MANAGER_INTERFACE		OFONO_SERVICE ".Manager"
#define OFONO_MODEM_INTERFACE		OFONO_SERVICE ".Modem"
#define OFONO_STK_INTERFACE		OFONO_SERVICE ".SimToolkit"
#define OFONO_STKAGENT_INTERFACE	OFONO_SERVICE ".SimToolkitAgent"

#define LISTEN_PORT	12765

enum test_state {
	TEST_STATE_POWERING_UP = 1,
	TEST_STATE_REGISTERING_AGENT,
	TEST_STATE_RUNNING,
	TEST_STATE_POWERING_DOWN,
};

enum test_result {
	TEST_RESULT_NOT_RUN = 0,
	TEST_RESULT_PASSED,
	TEST_RESULT_FAILED
};

typedef DBusMessage *(*display_text_cb_t)(DBusMessage *msg, const char *text,
						unsigned char icon_id,
						gboolean urgent);
typedef void (*terminal_response_func)(const unsigned char *pdu,
					unsigned int len);

struct test {
	char *name;
	char *method;
	unsigned char *req_pdu;
	unsigned int req_len;
	unsigned char *rsp_pdu;
	unsigned int rsp_len;
	void *agent_func;
	terminal_response_func tr_func;
	enum test_result result;
};

static GMainLoop *main_loop = NULL;
static volatile sig_atomic_t __terminated = 0;
GList *tests = NULL;
GList *cur_test = NULL;

/* DBus related */
static DBusConnection *conn;
static gboolean ofono_running = FALSE;
static guint modem_changed_watch;
enum test_state state;
DBusMessage *pending = NULL;

/* Emulator setup */
static guint server_watch;
static GAtServer *emulator;

/* Emulated modem state variables */
static int modem_mode = 0;

void __stktest_test_next();
void __stktest_test_finish(gboolean successful);
static gboolean create_tcp(void);

#define STKTEST_AGENT_ASSERT(expr)					\
	do {								\
		if (!(expr)) {						\
			g_printerr("Assertion Failed %s:%d %s\n",	\
					__FILE__, __LINE__, #expr);	\
			__stktest_test_finish(FALSE);			\
			return stktest_error_failed(msg);		\
		}							\
	} while (0)

#define STKTEST_RESPONSE_ASSERT(expect_pdu, expect_pdu_len,		\
				got_pdu, got_pdu_len)			\
	do {								\
		if ((expect_pdu_len) != (got_pdu_len)) {		\
			g_printerr("Assertion Failed %s:%d"		\
					" Wrong response len"		\
					" want: %d, got: %d\n",		\
					__FILE__, __LINE__,		\
					expect_pdu_len, got_pdu_len);	\
			__stktest_test_finish(FALSE);			\
			return;						\
		}							\
									\
		if (memcmp(expect_pdu, got_pdu, expect_pdu_len) != 0) {	\
			g_printerr("Assertion Failed %s:%d"		\
					"Wrong response\n",		\
					__FILE__, __LINE__);		\
			__stktest_test_finish(FALSE);			\
			return;						\
		}							\
	} while (0)

static const char *to_hex(const unsigned char *data, unsigned int len)
{
	static char buf[512+1];
	unsigned int i;

	for (i = 0; i < len; i++)
		sprintf(buf + i * 2, "%02hhX", data[i]);

	buf[i*2] = '\0';

	return buf;
}

static void send_proactive_command(const unsigned char *pdu, unsigned int len)
{
	char buf[1024];

	sprintf(buf, "+CUSATP: %s", to_hex(pdu, len));
	g_at_server_send_unsolicited(emulator, buf);
}

static DBusMessage *stktest_error_invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg, STKTEST_ERROR ".InvalidArguments",
					"Invalid arguments provided");
}

static DBusMessage *stktest_error_failed(DBusMessage *msg)
{
	return g_dbus_create_error(msg, STKTEST_ERROR ".Failed",
					"Operation failed");
}

static DBusMessage *stktest_error_end_session(DBusMessage *msg)
{
	return g_dbus_create_error(msg, OFONO_ERROR ".EndSession",
					"End Session Request");
}

static DBusMessage *stktest_error_go_back(DBusMessage *msg)
{
	return g_dbus_create_error(msg, OFONO_ERROR ".GoBack",
					"Go Back Request");
}

static DBusMessage *stktest_error_busy(DBusMessage *msg)
{
	return g_dbus_create_error(msg, OFONO_ERROR ".Busy",
					"UI Busy");
}

static DBusMessage *agent_release(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	g_print("Got Release\n");

	if (pending) {
		dbus_message_unref(pending);
		pending = NULL;
	}

	return dbus_message_new_method_return(msg);
}

static DBusMessage *agent_cancel(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	if (pending) {
		dbus_message_unref(pending);
		pending = NULL;
	}

	return NULL;
}

static DBusMessage *agent_display_text(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	const char *text;
	unsigned char icon_id;
	dbus_bool_t urgent;
	struct test *test;
	display_text_cb_t func;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &text,
						DBUS_TYPE_BYTE, &icon_id,
						DBUS_TYPE_BOOLEAN, &urgent,
						DBUS_TYPE_INVALID) == FALSE)
		return stktest_error_invalid_args(msg);

	if (cur_test == NULL)
		return stktest_error_failed(msg);

	test = cur_test->data;
	func = test->agent_func;

	if (strcmp(test->method, "DisplayText")) {
		g_printerr("Wrong method called!\n");
		__stktest_test_finish(FALSE);
		return stktest_error_failed(msg);
	}

	if (func == NULL) {
		g_printerr("DisplayText not expected to be called");
		__stktest_test_finish(FALSE);
		return stktest_error_failed(msg);
	}

	return func(msg, text, icon_id, urgent);
}

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

static void cusatt_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_ext_final(server, "+CME ERROR: 4");
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		const unsigned char *pdu;
		int len;
		struct test *test;
		terminal_response_func func;

		g_at_result_iter_init(&iter, cmd);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_hexstring(&iter, &pdu, &len) == FALSE)
			goto error;

		if (cur_test == NULL)
			goto error;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		test = cur_test->data;
		func = test->tr_func;
		func(pdu, len);
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
	g_at_server_unref(emulator);
	emulator = NULL;

	if (create_tcp() == TRUE)
		return;

	g_print("Error listening to socket\n");
	g_main_loop_quit(main_loop);
}

static void setup_emulator(GAtServer *server)
{
	g_at_server_set_debug(server, server_debug, "Server");

	g_at_server_register(server, "+CGMI", cgmi_cb, NULL, NULL);
	g_at_server_register(server, "+CGMM", cgmm_cb, NULL, NULL);
	g_at_server_register(server, "+CGMR", cgmr_cb, NULL, NULL);
	g_at_server_register(server, "+CGSN", cgsn_cb, NULL, NULL);
	g_at_server_register(server, "+CFUN", cfun_cb, NULL, NULL);
	g_at_server_register(server, "+CUSATT", cusatt_cb, NULL, NULL);

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
	g_at_server_set_echo(emulator, FALSE);
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

static int send_with_reply(const char *path, const char *interface,
				const char *method, DBusPendingCall **call,
				DBusPendingCallNotifyFunction cb,
				void *user_data, DBusFreeFunction free_func,
				int timeout, int type, ...)
{
	DBusMessage *msg;
	DBusPendingCall *c;
	va_list args;
	int err;

	msg = dbus_message_new_method_call(OFONO_SERVICE, path,
						interface, method);
	if (msg == NULL) {
		g_printerr("Unable to allocate new D-Bus %s message\n", method);
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
		timeout *= 1000;

	if (!dbus_connection_send_with_reply(conn, msg, &c, timeout)) {
		g_printerr("Sending %s failed\n", method);
		err = -EIO;
		goto fail;
	}

	if (call != NULL)
		*call = c;

	dbus_pending_call_set_notify(c, cb, user_data, free_func);
	dbus_pending_call_unref(c);

	dbus_message_unref(msg);

	return 0;

fail:
	if (free_func && user_data)
		free_func(user_data);

	if (msg)
		dbus_message_unref(msg);

	return err;
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

static int set_property(const char *path, const char *interface,
			const char *key, int type, const void *val,
			DBusPendingCallNotifyFunction notify,
			gpointer user_data,
			DBusFreeFunction destroy)
{
	DBusMessage *msg;
	DBusMessageIter iter, value;
	DBusPendingCall *call;
	const char *signature;

	msg = dbus_message_new_method_call(OFONO_SERVICE, path, interface,
						"SetProperty");
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

static void register_agent_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError err;
	struct test *test;

	dbus_error_init(&err);

	if (dbus_set_error_from_message(&err, reply) == TRUE) {
		g_printerr("%s: %s\n", err.name, err.message);
		dbus_error_free(&err);
	}

	dbus_message_unref(reply);

	state = TEST_STATE_RUNNING;
	test = cur_test->data;
	send_proactive_command(test->req_pdu, test->req_len);
}

static void register_agent()
{
	const char *path = "/default";
	int status;

	g_print("Gained STK interface, registering agent...\n");

	status = send_with_reply(STKTEST_PATH, OFONO_STK_INTERFACE,
					"RegisterAgent", NULL,
					register_agent_reply, NULL, NULL, 1,
					DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);

	if (status < 0) {
		g_printerr("Unable to register agent with oFono\n");
		g_main_loop_quit(main_loop);
		return;
	}

	state = TEST_STATE_REGISTERING_AGENT;
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

	switch (state) {
	case TEST_STATE_POWERING_UP:
		if (has_stk)
			register_agent();
		break;
	case TEST_STATE_REGISTERING_AGENT:
	case TEST_STATE_RUNNING:
		if (has_stk == FALSE)
			g_printerr("Unexpectedly lost STK interface\n");
		/* Fall through */
	case TEST_STATE_POWERING_DOWN:
		break;
	};

	return TRUE;
}

static void powerup(void)
{
	dbus_bool_t powered = TRUE;

	state = TEST_STATE_POWERING_UP;
	set_property(STKTEST_PATH, OFONO_MODEM_INTERFACE, "Powered",
			DBUS_TYPE_BOOLEAN, &powered,
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

	__stktest_test_next();
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

static const GDBusMethodTable agent_methods[] = {
	{ GDBUS_METHOD("Release", NULL, NULL, agent_release) },
	{ GDBUS_ASYNC_METHOD("DisplayText",
		GDBUS_ARGS({ "text", "s" }, { "icon_id", "y" },
				{ "urgent", "b" }), NULL,
				agent_display_text) },
	{ GDBUS_NOREPLY_METHOD("Cancel", NULL, NULL, agent_cancel) },
	{ },
};

static void ofono_connect(DBusConnection *conn, void *user_data)
{
	g_print("starting telephony interface\n");

	if (!g_dbus_register_interface(conn, "/default",
					OFONO_STKAGENT_INTERFACE,
					agent_methods, NULL, NULL,
					NULL, NULL)) {
		g_printerr("Unable to register local agent");
		g_main_loop_quit(main_loop);
	}

	ofono_running = TRUE;
	get_modems(conn);
}

static void ofono_disconnect(DBusConnection *conn, void *user_data)
{
	g_print("stopping telephony interface\n");

	g_dbus_unregister_interface(conn, "/default", OFONO_STKAGENT_INTERFACE);

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

static gboolean end_session_and_finish(gpointer user_data)
{
	g_at_server_send_unsolicited(emulator, "+CUSATEND");
	__stktest_test_finish(TRUE);

	return FALSE;
}

static void expect_response(const unsigned char *pdu, unsigned int len)
{
	struct test *test = cur_test->data;

	STKTEST_RESPONSE_ASSERT(test->rsp_pdu, test->rsp_len, pdu, len);

	g_idle_add(end_session_and_finish, NULL);
}

static DBusMessage *test_display_text_11(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 1"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_12(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 1"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return stktest_error_busy(msg);
}

static DBusMessage *test_display_text_13(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 2"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == TRUE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_14(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 3"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_15(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 4"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	pending = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *test_display_text_16(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "This command instructs the ME"
						" to display a text message. "
						"It allows the SIM to define "
						"the priority of that message, "
						"and the text string format. "
						"Two types of prio"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_17(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	/* oFono gives rich text formatting in HTML */
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;GO-BACKWARDS&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return stktest_error_go_back(msg);
}

static DBusMessage *test_display_text_18(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	/* oFono gives rich text formatting in HTML */
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;ABORT&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return stktest_error_end_session(msg);
}

static DBusMessage *test_display_text_21(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;TIME-OUT&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	pending = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *test_display_text_31(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	static const char *expected = "This command instructs the ME to display"
					" a text message, and/or an icon "
					"(see 6.5.4). It allows the "
					"SIM to define the priority of that "
					"message, and the text string format. "
					"Two types of priority are defined:- "
					"display normal priority text and/";
	STKTEST_AGENT_ASSERT(g_str_equal(text, expected));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static void power_down_reply(DBusPendingCall *call, void *user_data)
{
	__stktest_test_next();
}

void __stktest_test_finish(gboolean successful)
{
	struct test *test = cur_test->data;
	dbus_bool_t powered = FALSE;

	test->result = successful ? TEST_RESULT_PASSED : TEST_RESULT_FAILED;

	state = TEST_STATE_POWERING_DOWN;
	set_property(STKTEST_PATH, OFONO_MODEM_INTERFACE, "Powered",
			DBUS_TYPE_BOOLEAN, &powered,
			power_down_reply, NULL, NULL);
}

void __stktest_test_next()
{
	if (cur_test == NULL)
		cur_test = tests;
	else
		cur_test = cur_test->next;

	if (cur_test == NULL)
		g_main_loop_quit(main_loop);

	powerup();
}

static void stktest_add_test(const char *name, const char *method,
				const unsigned char *req, unsigned int req_len,
				const unsigned char *rsp, unsigned int rsp_len,
				void *agent_func,
				terminal_response_func tr_func)
{
	struct test *test = g_new0(struct test, 1);

	test->name = g_strdup(name);
	test->method = g_strdup(method);
	test->req_pdu = g_memdup(req, req_len);
	test->req_len = req_len;
	test->rsp_pdu = g_memdup(rsp, rsp_len);
	test->rsp_len = rsp_len;
	test->agent_func = agent_func;
	test->tr_func = tr_func;

	tests = g_list_append(tests, test);
}

static void __stktest_test_init(void)
{
	stktest_add_test("Display Text 1.1", "DisplayText",
				display_text_111, sizeof(display_text_111),
				display_text_response_111,
				sizeof(display_text_response_111),
				test_display_text_11, expect_response);
	stktest_add_test("Display Text 1.2", "DisplayText",
				display_text_111, sizeof(display_text_111),
				display_text_response_121,
				sizeof(display_text_response_121),
				test_display_text_12, expect_response);
	stktest_add_test("Display Text 1.3", "DisplayText",
				display_text_131, sizeof(display_text_131),
				display_text_response_131,
				sizeof(display_text_response_131),
				test_display_text_13, expect_response);
	stktest_add_test("Display Text 1.4", "DisplayText",
				display_text_141, sizeof(display_text_141),
				display_text_response_141,
				sizeof(display_text_response_141),
				test_display_text_14, expect_response);
	stktest_add_test("Display Text 1.5", "DisplayText",
				display_text_151, sizeof(display_text_151),
				display_text_response_151,
				sizeof(display_text_response_151),
				test_display_text_15, expect_response);
	stktest_add_test("Display Text 1.6", "DisplayText",
				display_text_161, sizeof(display_text_161),
				display_text_response_161,
				sizeof(display_text_response_161),
				test_display_text_16, expect_response);
	stktest_add_test("Display Text 1.7", "DisplayText",
				display_text_171, sizeof(display_text_171),
				display_text_response_171,
				sizeof(display_text_response_171),
				test_display_text_17, expect_response);
	stktest_add_test("Display Text 1.8", "DisplayText",
				display_text_181, sizeof(display_text_181),
				display_text_response_181,
				sizeof(display_text_response_181),
				test_display_text_18, expect_response);
	stktest_add_test("Display Text 1.9", "DisplayText",
				display_text_191, sizeof(display_text_191),
				display_text_response_191,
				sizeof(display_text_response_191),
				NULL, expect_response);
	stktest_add_test("Display Text 2.1", "DisplayText",
				display_text_211, sizeof(display_text_211),
				display_text_response_211,
				sizeof(display_text_response_211),
				test_display_text_21, expect_response);
	stktest_add_test("Display Text 3.1", "DisplayText",
				display_text_311, sizeof(display_text_311),
				display_text_response_311,
				sizeof(display_text_response_311),
				test_display_text_31, expect_response);
}

static void test_destroy(gpointer user_data)
{
	struct test *test = user_data;

	g_free(test->name);
	g_free(test->method);
	g_free(test->req_pdu);
	g_free(test->rsp_pdu);

	g_free(test);
}

static void __stktest_test_summarize(void)
{
	GList *l;

	g_print("\n\nTest Summary\n");
	g_print("============\n");

	for (l = tests; l; l = l->next) {
		struct test *test = l->data;

		g_print("%-60s", test->name);

		switch (test->result) {
		case TEST_RESULT_NOT_RUN:
			g_print("Not Run\n");
			break;
		case TEST_RESULT_PASSED:
			g_print("Passed\n");
			break;
		case TEST_RESULT_FAILED:
			g_print("Failed\n");
		break;
		}
	}
}

static void __stktest_test_cleanup(void)
{
	g_list_free_full(tests, test_destroy);
	tests = NULL;
	cur_test = NULL;
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

	__stktest_test_init();

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

	__stktest_test_summarize();
	__stktest_test_cleanup();

	return 0;
}
