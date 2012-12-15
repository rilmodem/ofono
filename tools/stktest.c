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

#define CYRILLIC "ЗДРАВСТВУЙТЕ"

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
typedef DBusMessage *(*get_inkey_cb_t)(DBusMessage *msg, const char *alpha,
						unsigned char icon_id);
typedef DBusMessage *(*get_input_cb_t)(DBusMessage *msg, const char *alpha,
						unsigned char icon_id,
						const char *def_input,
						unsigned char min_chars,
						unsigned char max_chars,
						gboolean hide_typing);
typedef DBusMessage *(*play_tone_cb_t)(DBusMessage *msg, const char *tone,
						const char *text,
						unsigned char icon_id);
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
	gdouble min_time;
	gdouble max_time;
};

static GMainLoop *main_loop = NULL;
static volatile sig_atomic_t __terminated = 0;
static GList *tests = NULL;
static GList *cur_test = NULL;
static GTimer *timer = NULL;

/* DBus related */
static DBusConnection *conn;
static gboolean ofono_running = FALSE;
static guint modem_changed_watch;
static enum test_state state;
static DBusMessage *pending = NULL;

/* Emulator setup */
static guint server_watch;
static GAtServer *emulator;

/* Emulated modem state variables */
static int modem_mode = 0;

static void __stktest_test_next();
static void __stktest_test_finish(gboolean successful);
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
	DBusMessage *reply;

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

	reply = func(msg, text, icon_id, urgent);
	if (reply == NULL)
		pending = dbus_message_ref(msg);

	return reply;
}

#define GET_INKEY_TEMPLATE(func, method_name)				\
static DBusMessage *func(DBusConnection *conn, DBusMessage *msg,	\
				void *data)				\
{									\
	const char *alpha;						\
	unsigned char icon_id;						\
	struct test *test;						\
	get_inkey_cb_t func;						\
	DBusMessage *reply;						\
									\
	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &alpha,	\
					DBUS_TYPE_BYTE, &icon_id,	\
					DBUS_TYPE_INVALID) == FALSE)	\
		return stktest_error_invalid_args(msg);			\
									\
	if (cur_test == NULL)						\
		return stktest_error_failed(msg);			\
									\
	test = cur_test->data;						\
	func = test->agent_func;					\
									\
	if (strcmp(test->method, method_name)) {			\
		g_printerr("Wrong method called!"			\
				"  Expected: %s, Got: %s\n",		\
				test->method, method_name);		\
		__stktest_test_finish(FALSE);				\
		return stktest_error_failed(msg);			\
	}								\
									\
	if (func == NULL) {						\
		g_printerr(method_name " not expected to be called");	\
		__stktest_test_finish(FALSE);				\
		return stktest_error_failed(msg);			\
	}								\
									\
	reply = func(msg, alpha, icon_id);				\
	if (reply == NULL)						\
		pending = dbus_message_ref(msg);			\
									\
	return reply;							\
}									\

GET_INKEY_TEMPLATE(agent_request_key, "RequestKey")
GET_INKEY_TEMPLATE(agent_request_digit, "RequestDigit")
GET_INKEY_TEMPLATE(agent_request_confirmation, "RequestConfirmation")

#define GET_INPUT_TEMPLATE(func, method_name)				\
static DBusMessage *func(DBusConnection *conn, DBusMessage *msg,	\
				void *data)				\
{									\
	const char *alpha;						\
	const char *def_input;						\
	unsigned char icon_id;						\
	unsigned char min_chars;					\
	unsigned char max_chars;					\
	gboolean hide_typing;						\
	struct test *test;						\
	get_input_cb_t func;						\
	DBusMessage *reply;						\
									\
	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &alpha,	\
					DBUS_TYPE_BYTE, &icon_id,	\
					DBUS_TYPE_STRING, &def_input,	\
					DBUS_TYPE_BYTE, &min_chars,	\
					DBUS_TYPE_BYTE, &max_chars,	\
					DBUS_TYPE_BOOLEAN,		\
					&hide_typing,			\
					DBUS_TYPE_INVALID) == FALSE)	\
		return stktest_error_invalid_args(msg);			\
									\
	if (cur_test == NULL)						\
		return stktest_error_failed(msg);			\
									\
	test = cur_test->data;						\
	func = test->agent_func;					\
									\
	if (strcmp(test->method, method_name)) {			\
		g_printerr("Wrong method called!"			\
				"  Expected: %s, Got: %s\n",		\
				test->method, method_name);		\
		__stktest_test_finish(FALSE);				\
		return stktest_error_failed(msg);			\
	}								\
									\
	if (func == NULL) {						\
		g_printerr(method_name " not expected to be called");	\
		__stktest_test_finish(FALSE);				\
		return stktest_error_failed(msg);			\
	}								\
									\
	reply = func(msg, alpha, icon_id, def_input,			\
			min_chars, max_chars, hide_typing);		\
	if (reply == NULL)						\
		pending = dbus_message_ref(msg);			\
									\
	return reply;							\
}									\

GET_INPUT_TEMPLATE(agent_request_input, "RequestInput")
GET_INPUT_TEMPLATE(agent_request_digits, "RequestDigits")

#define PLAY_TONE_TEMPLATE(func, method_name)				\
static DBusMessage *func(DBusConnection *conn, DBusMessage *msg,	\
				void *data)				\
{									\
	const char *tone;						\
	const char *text;						\
	unsigned char icon_id;						\
	struct test *test;						\
	play_tone_cb_t func;						\
	DBusMessage *reply;						\
									\
	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &tone,	\
					DBUS_TYPE_STRING, &text,	\
					DBUS_TYPE_BYTE, &icon_id,	\
					DBUS_TYPE_INVALID) == FALSE)	\
		return stktest_error_invalid_args(msg);			\
									\
	if (cur_test == NULL)						\
		return stktest_error_failed(msg);			\
									\
	test = cur_test->data;						\
	func = test->agent_func;					\
									\
	if (strcmp(test->method, method_name)) {			\
		g_printerr("Wrong method called!"			\
				"  Expected: %s, Got: %s\n",		\
				test->method, method_name);		\
		__stktest_test_finish(FALSE);				\
		return stktest_error_failed(msg);			\
	}								\
									\
	if (func == NULL) {						\
		g_printerr(method_name " not expected to be called");	\
		__stktest_test_finish(FALSE);				\
		return stktest_error_failed(msg);			\
	}								\
									\
	reply = func(msg, tone, text, icon_id);				\
	if (reply == NULL)						\
		pending = dbus_message_ref(msg);			\
									\
	return reply;							\
}									\

PLAY_TONE_TEMPLATE(agent_play_tone, "PlayTone")
PLAY_TONE_TEMPLATE(agent_loop_tone, "LoopTone")

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

	if (test->min_time != 0.0 || test->max_time != 0.0)
		g_timer_start(timer);
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
	{ GDBUS_ASYNC_METHOD("RequestDigit",
		GDBUS_ARGS({ "alpha", "s" }, { "icon_id", "y" }),
		GDBUS_ARGS({ "digit", "s" }),
				agent_request_digit) },
	{ GDBUS_ASYNC_METHOD("RequestKey",
		GDBUS_ARGS({ "alpha", "s" }, { "icon_id", "y" }),
		GDBUS_ARGS({ "key", "s" }),
				agent_request_key) },
	{ GDBUS_ASYNC_METHOD("RequestConfirmation",
		GDBUS_ARGS({ "alpha", "s" }, { "icon_id", "y" }),
		GDBUS_ARGS({ "confirmation", "b" }),
				agent_request_confirmation) },
	{ GDBUS_ASYNC_METHOD("RequestInput",
		GDBUS_ARGS({ "alpha", "s" }, { "icon_id", "y" },
				{ "default", "s" }, { "min_chars", "y" },
				{ "max_chars", "y" }, { "hide_typing", "b" }),
		GDBUS_ARGS({ "input", "s" }), agent_request_input) },
	{ GDBUS_ASYNC_METHOD("RequestDigits",
		GDBUS_ARGS({ "alpha", "s" }, { "icon_id", "y" },
				{ "default", "s" }, { "min_chars", "y" },
				{ "max_chars", "y" }, { "hide_typing", "b" }),
		GDBUS_ARGS({ "digits", "s" }), agent_request_digits) },
	{ GDBUS_ASYNC_METHOD("PlayTone",
		GDBUS_ARGS({ "tone", "s" }, { "text", "s" },
				{ "icon_id", "y" }),
		NULL, agent_play_tone) },
	{ GDBUS_ASYNC_METHOD("LoopTone",
		GDBUS_ARGS({ "tone", "s" }, { "text", "s" },
				{ "icon_id", "y" }),
		NULL, agent_loop_tone) },
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

static void expect_response_and_finish(const unsigned char *pdu,
					unsigned int len)
{
	struct test *test = cur_test->data;

	STKTEST_RESPONSE_ASSERT(test->rsp_pdu, test->rsp_len, pdu, len);

	if (test->min_time != 0.0 || test->max_time != 0.0) {
		gdouble elapsed = g_timer_elapsed(timer, NULL);

		if (elapsed < test->min_time) {
			g_printerr("Response received too soon, elapsed:%.2f,"
					" expected: %.2f\n", elapsed,
					test->min_time);
			__stktest_test_finish(FALSE);
			return;
		}

		if (elapsed > test->max_time) {
			g_printerr("Response received too late, elapsed: %.2f,"
					" expected: %.2f\n", elapsed,
					test->max_time);
			__stktest_test_finish(FALSE);
			return;
		}
	}

	g_idle_add(end_session_and_finish, NULL);
}

static void expect_response(const unsigned char *pdu, unsigned int len)
{
	struct test *test = cur_test->data;

	STKTEST_RESPONSE_ASSERT(test->rsp_pdu, test->rsp_len, pdu, len);
}

static gboolean poweroff_not_canceled_after_3(gpointer user_data)
{
	__stktest_test_finish(pending != NULL);
	return FALSE;
}

static gboolean end_session_and_not_canceled_after_3(gpointer user_data)
{
	g_at_server_send_unsolicited(emulator, "+CUSATEND");
	g_timeout_add_seconds(3, poweroff_not_canceled_after_3, NULL);

	return FALSE;
}

static void expect_response_and_not_canceled_after_3(const unsigned char *pdu,
							unsigned int len)
{
	struct test *test = cur_test->data;

	STKTEST_RESPONSE_ASSERT(test->rsp_pdu, test->rsp_len, pdu, len);

	g_idle_add(end_session_and_not_canceled_after_3, NULL);
}

static gboolean poweroff_and_canceled_after_21(gpointer user_data)
{
	__stktest_test_finish(pending == NULL);
	return FALSE;
}

static gboolean end_session_and_canceled_after_21(gpointer user_data)
{
	g_at_server_send_unsolicited(emulator, "+CUSATEND");
	g_timeout_add_seconds(21, poweroff_and_canceled_after_21, NULL);

	return FALSE;
}

static void expect_response_and_canceled_after_21(const unsigned char *pdu,
							unsigned int len)
{
	struct test *test = cur_test->data;

	STKTEST_RESPONSE_ASSERT(test->rsp_pdu, test->rsp_len, pdu, len);

	g_idle_add(end_session_and_canceled_after_21, NULL);
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

static DBusMessage *test_display_text_41(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 1"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return NULL;
}

static DBusMessage *test_display_text_42(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 2"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return NULL;
}

static gboolean user_response(gpointer user_data)
{
	if (pending == NULL) {
		__stktest_test_finish(FALSE);
		return FALSE;
	}

	g_dbus_send_reply(conn, pending, DBUS_TYPE_INVALID);
	dbus_message_unref(pending);
	pending = NULL;

	__stktest_test_finish(TRUE);

	return FALSE;
}

static DBusMessage *test_display_text_43(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Toolkit Test 3"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	g_timeout_add_seconds(3, user_response, NULL);
	return NULL;
}

static DBusMessage *test_display_text_51(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Basic Icon"));
	STKTEST_AGENT_ASSERT(icon_id == 1);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_52(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Colour Icon"));
	STKTEST_AGENT_ASSERT(icon_id == 2);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_53(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Basic Icon"));
	STKTEST_AGENT_ASSERT(icon_id == 1);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_61(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, CYRILLIC));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_71(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "10 Second"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return NULL;
}

static DBusMessage *test_display_text_81(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_82(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: center;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_83(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: right;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_84(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"big;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute 1</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_85(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"small;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute 1</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_86(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-weight: "
		"bold;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute 1</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_87(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-style: "
		"italic;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute 1</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_88(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\""
		"text-decoration: underline;color: #347235;"
		"background-color: #FFFF00;\">Text Attribute 1</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_89(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\""
		"text-decoration: line-through;color: #347235;"
		"background-color: #FFFF00;\">Text Attribute 1</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_810(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_91(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect = "你好";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_display_text_101(DBusMessage *msg,
						const char *text,
						unsigned char icon_id,
						gboolean urgent)
{
	const char *expect = "80ル";

	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(urgent == FALSE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_get_inkey_11(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter \"+\""));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_12(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "0";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter \"0\""));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_13(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;GO-BACKWARDS&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return stktest_error_go_back(msg);
}

static DBusMessage *test_get_inkey_14(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;ABORT&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return stktest_error_end_session(msg);
}

static DBusMessage *test_get_inkey_15(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "q";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter \"q\""));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_16(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "x";
	const char *expected =
		"Enter \"x\". This command instructs the ME to display text, "
		"and to expect the user to enter a single character. Any "
		"response entered by the user shall be passed t";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expected));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_21(DBusMessage *msg,
					const char *text, unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;TIME-OUT&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_get_inkey_31(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, CYRILLIC));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_32(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect = "ЗДРАВСТВУЙТЕЗДРАВСТВУЙТЕ"
				"ЗДРАВСТВУЙТЕЗДРАВСТВУЙТЕ"
				"ЗДРАВСТВУЙТЕЗДРАВСТВУЙ";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_41(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "Д";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_51a(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	dbus_bool_t ret = 1;

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter YES"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_51b(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	dbus_bool_t ret = 0;

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter NO"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_61(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;NO-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 1);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_62(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;BASIC-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 1);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_63(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;NO-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 2);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_64(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;COLOUR-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 2);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_81(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter \"+\""));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_get_inkey_91(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter \"+\"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_92(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: center;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter \"+\"</span>"
		"</div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_93(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: right;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter \"+\"</span>"
		"</div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_94(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"big;color: #347235;background-color: #FFFF00;\">Enter \"+\""
		"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_95(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"small;color: #347235;background-color: #FFFF00;\">"
		"Enter \"+\"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_96(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-weight: "
		"bold;color: #347235;background-color: #FFFF00;\">Enter \"+\""
		"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_97(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"font-style: "
		"italic;color: #347235;background-color: #FFFF00;\">"
		"Enter \"+\"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_98(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\""
		"text-decoration: underline;color: #347235;"
		"background-color: #FFFF00;\">Enter \"+\"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_99(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\""
		"text-decoration: line-through;color: #347235;"
		"background-color: #FFFF00;\">Enter \"+\"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_910(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter \"+\"</span></div>";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_101(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect = "你好";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_102(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"你好你好你好你好你好你好你好你好你好你好"
		"你好你好你好你好你好你好你好你好你好你好"
		"你好你好你好你好你好你好你好你好你好你好"
		"你好你好你好你好你好";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_111(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "好";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_121(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect = "ル";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_122(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "+";
	const char *expect =
		"ルルルルルルルルルルルルルルルルルルルル"
		"ルルルルルルルルルルルルルルルルルルルル"
		"ルルルルルルルルルルルルルルルルルルルル"
		"ルルルルルルルルルル";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_inkey_131(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id)
{
	DBusMessage *reply;
	const char *ret = "ル";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_11(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "12345";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter 12345"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 5);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_12(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "67*#+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter 67*#+"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 5);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_13(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "AbCdE";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter AbCdE"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 5);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_14(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "2345678";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha,
					"Password 1&lt;SEND&gt;2345678"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 4);
	STKTEST_AGENT_ASSERT(max == 8);
	STKTEST_AGENT_ASSERT(hide_typing == TRUE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_15(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "12345678901234567890";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha,
					"Enter 1..9,0..9,0(1)"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 1);
	STKTEST_AGENT_ASSERT(max == 20);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_16(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;GO-BACKWARDS&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 8);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	return stktest_error_go_back(msg);
}

static DBusMessage *test_get_input_17(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;ABORT&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 8);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	return stktest_error_end_session(msg);
}

static DBusMessage *test_get_input_18(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *expect =
	"***1111111111###***2222222222###***3333333333###***4444444444###***"
	"5555555555###***6666666666###***7777777777###***8888888888###***9999"
	"999999###***0000000000###";
	const char *ret = "***1111111111###***2222222222###***"
				"3333333333###***4444444444###"
				"***5555555555###***6666666666###"
				"***7777777777###***8888888888###"
				"***9999999999###***0000000000###";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 160);
	STKTEST_AGENT_ASSERT(max == 160);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_19(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;SEND&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 1);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_110(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "12345";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, ""));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 1);
	STKTEST_AGENT_ASSERT(max == 5);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_21(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;TIME-OUT&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 10);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	return NULL;
}

static DBusMessage *test_get_input_31(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *expect = CYRILLIC;
	const char *ret = "HELLO";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 5);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_32(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *expect = "ЗДРАВСТВУЙТЕЗДРАВСТВУЙТЕ"
		"ЗДРАВСТВУЙТЕЗДРАВСТВУЙТЕ"
		"ЗДРАВСТВУЙТЕЗДРАВСТВУЙ";
	const char *ret = "HELLO";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 5);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_41(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = CYRILLIC;

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter Hello"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 12);
	STKTEST_AGENT_ASSERT(max == 12);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_42(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "ЗДРАВСТВУЙТЕЗДРАВСТВУЙТЕ"
				"ЗДРАВСТВУЙТЕЗДРАВСТВУЙТЕ"
				"ЗДРАВСТВУЙТЕЗДРАВСТВУЙ";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter Hello"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 255);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_51(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter 12345"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, "12345"));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 5);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &def_input,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_52(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *def_expect =
	"***1111111111###***2222222222###***3333333333###***4444444444###***"
	"5555555555###***6666666666###***7777777777###***8888888888###***9999"
	"999999###***0000000000###";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter:"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, def_expect));
	STKTEST_AGENT_ASSERT(min == 160);
	STKTEST_AGENT_ASSERT(max == 160);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &def_input,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_61(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;NO-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 1);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 10);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_62(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;BASIC-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 1);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 10);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_63(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;NO-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 2);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 10);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_64(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "+";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "&lt;COLOUR-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 2);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 0);
	STKTEST_AGENT_ASSERT(max == 10);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

#define GET_INPUT_8X_TEMPLATE(seq, expect)				\
static DBusMessage *test_get_input_8##seq(DBusMessage *msg,		\
					const char *alpha,		\
					unsigned char icon_id,		\
					const char *def_input,		\
					unsigned char min,		\
					unsigned char max,		\
					gboolean hide_typing)		\
{									\
	DBusMessage *reply;						\
	const char *ret = "12345";					\
									\
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));		\
	STKTEST_AGENT_ASSERT(icon_id == 0);				\
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));		\
	STKTEST_AGENT_ASSERT(min == 5);					\
	STKTEST_AGENT_ASSERT(max == 5);					\
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);			\
									\
	reply = dbus_message_new_method_return(msg);			\
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,		\
					DBUS_TYPE_INVALID);		\
									\
	return reply;							\
}

GET_INPUT_8X_TEMPLATE(1,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter 12345</span></div>")

GET_INPUT_8X_TEMPLATE(2,
		"<div style=\"text-align: center;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter 12345</span>"
		"</div>")

GET_INPUT_8X_TEMPLATE(3,
		"<div style=\"text-align: right;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter 12345</span>"
		"</div>")

GET_INPUT_8X_TEMPLATE(4,
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"big;color: #347235;background-color: #FFFF00;\">Enter 12345"
		"</span></div>")

GET_INPUT_8X_TEMPLATE(5,
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"small;color: #347235;background-color: #FFFF00;\">Enter "
		"12345</span></div>")

GET_INPUT_8X_TEMPLATE(6,
		"<div style=\"text-align: left;\"><span style=\"font-weight: "
		"bold;color: #347235;background-color: #FFFF00;\">Enter "
		"12345</span></div>")

GET_INPUT_8X_TEMPLATE(7,
		"<div style=\"text-align: left;\"><span style=\"font-style: "
		"italic;color: #347235;background-color: #FFFF00;\">Enter "
		"12345</span></div>")

GET_INPUT_8X_TEMPLATE(8,
		"<div style=\"text-align: left;\"><span "
		"style=\"text-decoration: underline;color: #347235;"
		"background-color: #FFFF00;\">Enter 12345</span></div>")

GET_INPUT_8X_TEMPLATE(9,
		"<div style=\"text-align: left;\"><span "
		"style=\"text-decoration: line-through;color: #347235;"
		"background-color: #FFFF00;\">Enter 12345</span></div>")

GET_INPUT_8X_TEMPLATE(10,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Enter 12345</span></div>")

#define GET_INPUT_9X_11X_TEMPLATE(seq, expect)				\
static DBusMessage *test_get_input_##seq(DBusMessage *msg,		\
					const char *alpha,		\
					unsigned char icon_id,		\
					const char *def_input,		\
					unsigned char min,		\
					unsigned char max,		\
					gboolean hide_typing)		\
{									\
	DBusMessage *reply;						\
	const char *ret = "HELLO";					\
									\
	STKTEST_AGENT_ASSERT(g_str_equal(alpha, expect));		\
	STKTEST_AGENT_ASSERT(icon_id == 0);				\
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));		\
	STKTEST_AGENT_ASSERT(min == 5);					\
	STKTEST_AGENT_ASSERT(max == 5);					\
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);			\
									\
	reply = dbus_message_new_method_return(msg);			\
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,		\
					DBUS_TYPE_INVALID);		\
									\
	return reply;							\
}

GET_INPUT_9X_11X_TEMPLATE(91, "你好")
GET_INPUT_9X_11X_TEMPLATE(92, "你好你好你好你好你好你好你好你好你好你好"
				"你好你好你好你好你好你好你好你好你好你好"
				"你好你好你好你好你好你好你好你好你好你好"
				"你好你好你好你好你好")

static DBusMessage *test_get_input_101(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "你好";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter Hello"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 2);
	STKTEST_AGENT_ASSERT(max == 2);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_102(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "你好你好你好你好你好你好"
				"你好你好你好你好你好你好"
				"你好你好你好你好你好你好"
				"你好你好你好你好你好你好"
				"你好你好你好你好你好你好"
				"你好你好你好你好你好";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter Hello"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 255);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

GET_INPUT_9X_11X_TEMPLATE(111,"ル")
GET_INPUT_9X_11X_TEMPLATE(112, "ルルルルルルルルルルルルルルルルルルルル"
				"ルルルルルルルルルルルルルルルルルルルル"
				"ルルルルルルルルルルルルルルルルルルルル"
				"ルルルルルルルルルル")

static DBusMessage *test_get_input_121(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "ルル";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter Hello"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 2);
	STKTEST_AGENT_ASSERT(max == 2);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_get_input_122(DBusMessage *msg,
					const char *alpha,
					unsigned char icon_id,
					const char *def_input,
					unsigned char min, unsigned char max,
					gboolean hide_typing)
{
	DBusMessage *reply;
	const char *ret = "ルルルルルルルルルルルルルルルルルルルル"
				"ルルルルルルルルルルルルルルルルルルルル"
				"ルルルルルルルルルルルルルルルルルルルル"
				"ルルルルルルルルルル";

	STKTEST_AGENT_ASSERT(g_str_equal(alpha, "Enter Hello"));
	STKTEST_AGENT_ASSERT(icon_id == 0);
	STKTEST_AGENT_ASSERT(g_str_equal(def_input, ""));
	STKTEST_AGENT_ASSERT(min == 5);
	STKTEST_AGENT_ASSERT(max == 255);
	STKTEST_AGENT_ASSERT(hide_typing == FALSE);

	reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ret,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *test_play_tone_11a(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "dial-tone"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Dial Tone"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_11b(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "busy"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Sub. Busy"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_11c(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "congestion"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Congestion"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_11d(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "radio-path-acknowledge"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "RP Ack"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_11e(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "radio-path-not-available"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "No RP"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_11f(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "error"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Spec Info"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_11g(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "call-waiting"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Call Wait"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_11h(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "ringing-tone"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Ring Tone"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_11i(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	const char *expect_text =
			"This command instructs the ME to play an audio tone. "
			"Upon receiving this command, the ME shall check "
			"if it is currently in, or in the process of setting "
			"up (SET-UP message sent to the network, see "
			"GSM\"04.08\"(8)), a speech call. - If the ME I";
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "general-beep"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, expect_text));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_11j(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "general-beep"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Beep"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_11k(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "positive-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Positive"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_11l(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "negative-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Negative"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_11m(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "general-beep"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "Quick"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_11n(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "error"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;ABORT&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return stktest_error_end_session(msg);
}

static DBusMessage *test_play_tone_11o(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "general-beep"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, ""));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_21(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "positive-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, CYRILLIC));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_31(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "positive-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;BASIC-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 1);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_32(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "positive-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;BASIC-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 1);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_33(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "positive-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;COLOUR-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 2);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_34(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "positive-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "&lt;COLOUR-ICON&gt;"));
	STKTEST_AGENT_ASSERT(icon_id == 2);

	return dbus_message_new_method_return(msg);
}

#define PLAY_TONE_4X_TEMPLATE(seq, expect)				\
static DBusMessage *test_play_tone_4##seq(DBusMessage *msg,		\
					const char *tone,		\
					const char *text,		\
					unsigned char icon_id)		\
{									\
	g_print("%s\n", text);						\
	STKTEST_AGENT_ASSERT(g_str_equal(tone,				\
					"positive-acknowledgement"));	\
	STKTEST_AGENT_ASSERT(g_str_equal(text, expect));		\
	STKTEST_AGENT_ASSERT(icon_id == 0);				\
									\
	return dbus_message_new_method_return(msg);			\
}									\

PLAY_TONE_4X_TEMPLATE(1a,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(1b, "Text Attribute 2")

PLAY_TONE_4X_TEMPLATE(2a,
		"<div style=\"text-align: center;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(2b, "Text Attribute 2")

PLAY_TONE_4X_TEMPLATE(3a,
		"<div style=\"text-align: right;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(3b, "Text Attribute 2")

PLAY_TONE_4X_TEMPLATE(4a,
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"big;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute 1</span></div>")
PLAY_TONE_4X_TEMPLATE(4b,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 2</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(4c, "Text Attribute 3")

PLAY_TONE_4X_TEMPLATE(5a,
		"<div style=\"text-align: left;\"><span style=\"font-size: "
		"small;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute 1</span></div>")
PLAY_TONE_4X_TEMPLATE(5b,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 2</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(5c, "Text Attribute 3")

PLAY_TONE_4X_TEMPLATE(6a,
		"<div style=\"text-align: left;\"><span style=\"font-weight: "
		"bold;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute</span></div> 1")
PLAY_TONE_4X_TEMPLATE(6b,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 2</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(6c, "Text Attribute 3")

PLAY_TONE_4X_TEMPLATE(7a,
		"<div style=\"text-align: left;\"><span style=\"font-style: "
		"italic;color: #347235;background-color: #FFFF00;\">"
		"Text Attribute</span></div> 1")
PLAY_TONE_4X_TEMPLATE(7b,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 2</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(7c, "Text Attribute 3")

PLAY_TONE_4X_TEMPLATE(8a,
		"<div style=\"text-align: left;\"><span "
		"style=\"text-decoration: underline;color: #347235;"
		"background-color: #FFFF00;\">Text Attribute 1</span></div>")
PLAY_TONE_4X_TEMPLATE(8b,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 2</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(8c, "Text Attribute 3")

PLAY_TONE_4X_TEMPLATE(9a,
		"<div style=\"text-align: left;\"><span "
		"style=\"text-decoration: line-through;color: #347235;"
		"background-color: #FFFF00;\">Text Attribute 1</span></div>")
PLAY_TONE_4X_TEMPLATE(9b,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 2</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(9c, "Text Attribute 3")

PLAY_TONE_4X_TEMPLATE(10a,
		"<div style=\"text-align: left;\"><span style=\"color: "
		"#347235;background-color: #FFFF00;\">Text Attribute 1</span>"
		"</div>")
PLAY_TONE_4X_TEMPLATE(10b, "Text Attribute 2")

static DBusMessage *test_play_tone_51(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "positive-acknowledgement"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "中一"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *test_play_tone_61a(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "dial-tone"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "80ル0"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static DBusMessage *test_play_tone_61b(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "dial-tone"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "81ル1"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}
static DBusMessage *test_play_tone_61c(DBusMessage *msg,
					const char *tone,
					const char *text,
					unsigned char icon_id)
{
	STKTEST_AGENT_ASSERT(g_str_equal(tone, "dial-tone"));
	STKTEST_AGENT_ASSERT(g_str_equal(text, "82ル2"));
	STKTEST_AGENT_ASSERT(icon_id == 0);

	return NULL;
}

static void power_down_reply(DBusPendingCall *call, void *user_data)
{
	__stktest_test_next();
}

static void __stktest_test_finish(gboolean successful)
{
	struct test *test = cur_test->data;
	dbus_bool_t powered = FALSE;

	test->result = successful ? TEST_RESULT_PASSED : TEST_RESULT_FAILED;

	state = TEST_STATE_POWERING_DOWN;
	set_property(STKTEST_PATH, OFONO_MODEM_INTERFACE, "Powered",
			DBUS_TYPE_BOOLEAN, &powered,
			power_down_reply, NULL, NULL);
}

static void __stktest_test_next()
{
	if (cur_test == NULL)
		cur_test = tests;
	else
		cur_test = cur_test->next;

	if (cur_test == NULL) {
		g_main_loop_quit(main_loop);
		return;
	}

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

static void stktest_add_timed_test(const char *name, const char *method,
					const unsigned char *req,
					unsigned int req_len,
					const unsigned char *rsp,
					unsigned int rsp_len,
					void *agent_func,
					terminal_response_func tr_func,
					gdouble expected_min_time,
					gdouble expected_max_time)
{
	GList *last;
	struct test *test;

	stktest_add_test(name, method, req, req_len, rsp, rsp_len, agent_func,
				tr_func);

	last = g_list_last(tests);
	test = last->data;

	test->min_time = expected_min_time;
	test->max_time = expected_max_time;
}

static void __stktest_test_init(void)
{
	stktest_add_test("Display Text 1.1", "DisplayText",
				display_text_111, sizeof(display_text_111),
				display_text_response_111,
				sizeof(display_text_response_111),
				test_display_text_11,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.2", "DisplayText",
				display_text_111, sizeof(display_text_111),
				display_text_response_121,
				sizeof(display_text_response_121),
				test_display_text_12,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.3", "DisplayText",
				display_text_131, sizeof(display_text_131),
				display_text_response_131,
				sizeof(display_text_response_131),
				test_display_text_13,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.4", "DisplayText",
				display_text_141, sizeof(display_text_141),
				display_text_response_141,
				sizeof(display_text_response_141),
				test_display_text_14,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.5", "DisplayText",
				display_text_151, sizeof(display_text_151),
				display_text_response_151,
				sizeof(display_text_response_151),
				test_display_text_15,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.6", "DisplayText",
				display_text_161, sizeof(display_text_161),
				display_text_response_161,
				sizeof(display_text_response_161),
				test_display_text_16,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.7", "DisplayText",
				display_text_171, sizeof(display_text_171),
				display_text_response_171,
				sizeof(display_text_response_171),
				test_display_text_17,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.8", "DisplayText",
				display_text_181, sizeof(display_text_181),
				display_text_response_181,
				sizeof(display_text_response_181),
				test_display_text_18,
				expect_response_and_finish);
	stktest_add_test("Display Text 1.9", "DisplayText",
				display_text_191, sizeof(display_text_191),
				display_text_response_191,
				sizeof(display_text_response_191),
				NULL, expect_response_and_finish);
	stktest_add_test("Display Text 2.1", "DisplayText",
				display_text_211, sizeof(display_text_211),
				display_text_response_211,
				sizeof(display_text_response_211),
				test_display_text_21,
				expect_response_and_finish);
	stktest_add_test("Display Text 3.1", "DisplayText",
				display_text_311, sizeof(display_text_311),
				display_text_response_311,
				sizeof(display_text_response_311),
				test_display_text_31,
				expect_response_and_finish);
	stktest_add_test("Display Text 4.1", "DisplayText",
				display_text_411, sizeof(display_text_411),
				display_text_response_411,
				sizeof(display_text_response_411),
				test_display_text_41,
				expect_response_and_not_canceled_after_3);
	stktest_add_test("Display Text 4.2", "DisplayText",
				display_text_421, sizeof(display_text_421),
				display_text_response_421,
				sizeof(display_text_response_421),
				test_display_text_42,
				expect_response_and_canceled_after_21);
	stktest_add_test("Display Text 4.3", "DisplayText",
				display_text_431, sizeof(display_text_431),
				display_text_response_431,
				sizeof(display_text_response_431),
				test_display_text_43, expect_response);
	stktest_add_test("Display Text 5.1A", "DisplayText",
				display_text_511, sizeof(display_text_511),
				display_text_response_511a,
				sizeof(display_text_response_511a),
				test_display_text_51,
				expect_response_and_finish);
	stktest_add_test("Display Text 5.2A", "DisplayText",
				display_text_521, sizeof(display_text_521),
				display_text_response_521a,
				sizeof(display_text_response_521a),
				test_display_text_52,
				expect_response_and_finish);
	stktest_add_test("Display Text 5.3A", "DisplayText",
				display_text_531, sizeof(display_text_531),
				display_text_response_531a,
				sizeof(display_text_response_531a),
				test_display_text_53,
				expect_response_and_finish);
	stktest_add_test("Display Text 6.1", "DisplayText",
				display_text_611, sizeof(display_text_611),
				display_text_response_611,
				sizeof(display_text_response_611),
				test_display_text_61,
				expect_response_and_finish);
	stktest_add_test("Display Text 7.1", "DisplayText",
				display_text_711, sizeof(display_text_711),
				display_text_response_711,
				sizeof(display_text_response_711),
				test_display_text_71,
				expect_response_and_finish);
	/*
	 * We skip parts where the UI is asked to display simple text to ensure
	 * that the alignment, font is set up correctly and not 'remembered'
	 * from a previous state.  oFono does not keep any state of the
	 * previous commands
	 */
	stktest_add_test("Display Text 8.1", "DisplayText",
				display_text_811, sizeof(display_text_811),
				display_text_response_811,
				sizeof(display_text_response_811),
				test_display_text_81,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.2", "DisplayText",
				display_text_821, sizeof(display_text_821),
				display_text_response_821,
				sizeof(display_text_response_821),
				test_display_text_82,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.3", "DisplayText",
				display_text_831, sizeof(display_text_831),
				display_text_response_831,
				sizeof(display_text_response_831),
				test_display_text_83,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.4", "DisplayText",
				display_text_841, sizeof(display_text_841),
				display_text_response_841,
				sizeof(display_text_response_841),
				test_display_text_84,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.5", "DisplayText",
				display_text_851, sizeof(display_text_851),
				display_text_response_851,
				sizeof(display_text_response_851),
				test_display_text_85,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.6", "DisplayText",
				display_text_861, sizeof(display_text_861),
				display_text_response_861,
				sizeof(display_text_response_861),
				test_display_text_86,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.7", "DisplayText",
				display_text_871, sizeof(display_text_871),
				display_text_response_871,
				sizeof(display_text_response_871),
				test_display_text_87,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.8", "DisplayText",
				display_text_881, sizeof(display_text_881),
				display_text_response_881,
				sizeof(display_text_response_881),
				test_display_text_88,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.9", "DisplayText",
				display_text_891, sizeof(display_text_891),
				display_text_response_891,
				sizeof(display_text_response_891),
				test_display_text_89,
				expect_response_and_finish);
	stktest_add_test("Display Text 8.10", "DisplayText",
				display_text_8101, sizeof(display_text_8101),
				display_text_response_8101,
				sizeof(display_text_response_8101),
				test_display_text_810,
				expect_response_and_finish);
	stktest_add_test("Display Text 9.1", "DisplayText",
				display_text_911, sizeof(display_text_911),
				display_text_response_911,
				sizeof(display_text_response_911),
				test_display_text_91,
				expect_response_and_finish);
	stktest_add_test("Display Text 10.1", "DisplayText",
				display_text_1011, sizeof(display_text_1011),
				display_text_response_1011,
				sizeof(display_text_response_1011),
				test_display_text_101,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 1.1", "RequestDigit",
				get_inkey_111, sizeof(get_inkey_111),
				get_inkey_response_111,
				sizeof(get_inkey_response_111),
				test_get_inkey_11,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 1.2", "RequestDigit",
				get_inkey_121, sizeof(get_inkey_121),
				get_inkey_response_121,
				sizeof(get_inkey_response_121),
				test_get_inkey_12,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 1.3", "RequestDigit",
				get_inkey_131, sizeof(get_inkey_131),
				get_inkey_response_131,
				sizeof(get_inkey_response_131),
				test_get_inkey_13,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 1.4", "RequestDigit",
				get_inkey_141, sizeof(get_inkey_141),
				get_inkey_response_141,
				sizeof(get_inkey_response_141),
				test_get_inkey_14,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 1.5", "RequestKey",
				get_inkey_151, sizeof(get_inkey_151),
				get_inkey_response_151,
				sizeof(get_inkey_response_151),
				test_get_inkey_15,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 1.6", "RequestKey",
				get_inkey_161, sizeof(get_inkey_161),
				get_inkey_response_161,
				sizeof(get_inkey_response_161),
				test_get_inkey_16,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 2.1", "RequestDigit",
				get_inkey_211, sizeof(get_inkey_211),
				get_inkey_response_211,
				sizeof(get_inkey_response_211),
				test_get_inkey_21,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 3.1", "RequestDigit",
				get_inkey_311, sizeof(get_inkey_311),
				get_inkey_response_311,
				sizeof(get_inkey_response_311),
				test_get_inkey_31,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 3.2", "RequestDigit",
				get_inkey_321, sizeof(get_inkey_321),
				get_inkey_response_321,
				sizeof(get_inkey_response_321),
				test_get_inkey_32,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 4.1", "RequestKey",
				get_inkey_411, sizeof(get_inkey_411),
				get_inkey_response_411,
				sizeof(get_inkey_response_411),
				test_get_inkey_41,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 5.1a", "RequestConfirmation",
				get_inkey_511, sizeof(get_inkey_511),
				get_inkey_response_511,
				sizeof(get_inkey_response_511),
				test_get_inkey_51a,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 5.1b", "RequestConfirmation",
				get_inkey_512, sizeof(get_inkey_512),
				get_inkey_response_512,
				sizeof(get_inkey_response_512),
				test_get_inkey_51b,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 6.1", "RequestDigit",
				get_inkey_611, sizeof(get_inkey_611),
				get_inkey_response_611,
				sizeof(get_inkey_response_611),
				test_get_inkey_61,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 6.2", "RequestDigit",
				get_inkey_621, sizeof(get_inkey_621),
				get_inkey_response_621,
				sizeof(get_inkey_response_621),
				test_get_inkey_62,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 6.3", "RequestDigit",
				get_inkey_631, sizeof(get_inkey_631),
				get_inkey_response_631,
				sizeof(get_inkey_response_631),
				test_get_inkey_63,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 6.4", "RequestDigit",
				get_inkey_641, sizeof(get_inkey_641),
				get_inkey_response_641,
				sizeof(get_inkey_response_641),
				test_get_inkey_64,
				expect_response_and_finish);
	/* Test Sequence for GetInkey 7.1 skipped, we do not support help */
	stktest_add_test("Get Inkey 8.1", "RequestDigit",
				get_inkey_811, sizeof(get_inkey_811),
				get_inkey_response_811,
				sizeof(get_inkey_response_811),
				test_get_inkey_81,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.1", "RequestDigit",
				get_inkey_911, sizeof(get_inkey_911),
				get_inkey_response_911,
				sizeof(get_inkey_response_911),
				test_get_inkey_91,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.2", "RequestDigit",
				get_inkey_921, sizeof(get_inkey_921),
				get_inkey_response_921,
				sizeof(get_inkey_response_921),
				test_get_inkey_92,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.3", "RequestDigit",
				get_inkey_931, sizeof(get_inkey_931),
				get_inkey_response_931,
				sizeof(get_inkey_response_931),
				test_get_inkey_93,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.4", "RequestDigit",
				get_inkey_941, sizeof(get_inkey_941),
				get_inkey_response_941,
				sizeof(get_inkey_response_941),
				test_get_inkey_94,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.5", "RequestDigit",
				get_inkey_951, sizeof(get_inkey_951),
				get_inkey_response_951,
				sizeof(get_inkey_response_951),
				test_get_inkey_95,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.6", "RequestDigit",
				get_inkey_961, sizeof(get_inkey_961),
				get_inkey_response_961,
				sizeof(get_inkey_response_961),
				test_get_inkey_96,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.7", "RequestDigit",
				get_inkey_971, sizeof(get_inkey_971),
				get_inkey_response_971,
				sizeof(get_inkey_response_971),
				test_get_inkey_97,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.8", "RequestDigit",
				get_inkey_981, sizeof(get_inkey_981),
				get_inkey_response_981,
				sizeof(get_inkey_response_981),
				test_get_inkey_98,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.9", "RequestDigit",
				get_inkey_991, sizeof(get_inkey_991),
				get_inkey_response_991,
				sizeof(get_inkey_response_991),
				test_get_inkey_99,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 9.10", "RequestDigit",
				get_inkey_9101, sizeof(get_inkey_9101),
				get_inkey_response_9101,
				sizeof(get_inkey_response_9101),
				test_get_inkey_910,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 10.1", "RequestDigit",
				get_inkey_1011, sizeof(get_inkey_1011),
				get_inkey_response_1011,
				sizeof(get_inkey_response_1011),
				test_get_inkey_101,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 10.2", "RequestDigit",
				get_inkey_1021, sizeof(get_inkey_1021),
				get_inkey_response_1021,
				sizeof(get_inkey_response_1021),
				test_get_inkey_102,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 11.1", "RequestKey",
				get_inkey_1111, sizeof(get_inkey_1111),
				get_inkey_response_1111,
				sizeof(get_inkey_response_1111),
				test_get_inkey_111,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 12.1", "RequestDigit",
				get_inkey_1211, sizeof(get_inkey_1211),
				get_inkey_response_1211,
				sizeof(get_inkey_response_1211),
				test_get_inkey_121,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 12.2", "RequestDigit",
				get_inkey_1221, sizeof(get_inkey_1221),
				get_inkey_response_1221,
				sizeof(get_inkey_response_1221),
				test_get_inkey_122,
				expect_response_and_finish);
	stktest_add_test("Get Inkey 13.1", "RequestKey",
				get_inkey_1311, sizeof(get_inkey_1311),
				get_inkey_response_1311,
				sizeof(get_inkey_response_1311),
				test_get_inkey_131,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.1", "RequestDigits",
				get_input_111, sizeof(get_input_111),
				get_input_response_111,
				sizeof(get_input_response_111),
				test_get_input_11,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.2", "RequestDigits",
				get_input_121, sizeof(get_input_121),
				get_input_response_121,
				sizeof(get_input_response_121),
				test_get_input_12,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.3", "RequestInput",
				get_input_131, sizeof(get_input_131),
				get_input_response_131,
				sizeof(get_input_response_131),
				test_get_input_13,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.4", "RequestDigits",
				get_input_141, sizeof(get_input_141),
				get_input_response_141,
				sizeof(get_input_response_141),
				test_get_input_14,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.5", "RequestDigits",
				get_input_151, sizeof(get_input_151),
				get_input_response_151,
				sizeof(get_input_response_151),
				test_get_input_15,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.6", "RequestDigits",
				get_input_161, sizeof(get_input_161),
				get_input_response_161,
				sizeof(get_input_response_161),
				test_get_input_16,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.7", "RequestDigits",
				get_input_171, sizeof(get_input_171),
				get_input_response_171,
				sizeof(get_input_response_171),
				test_get_input_17,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.8", "RequestDigits",
				get_input_181, sizeof(get_input_181),
				get_input_response_181,
				sizeof(get_input_response_181),
				test_get_input_18,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.9", "RequestDigits",
				get_input_191, sizeof(get_input_191),
				get_input_response_191a,
				sizeof(get_input_response_191a),
				test_get_input_19,
				expect_response_and_finish);
	stktest_add_test("Get Input 1.10", "RequestDigits",
				get_input_1101, sizeof(get_input_1101),
				get_input_response_1101,
				sizeof(get_input_response_1101),
				test_get_input_110,
				expect_response_and_finish);
	stktest_add_test("Get Input 2.1", "RequestDigits",
				get_input_211, sizeof(get_input_211),
				get_input_response_211,
				sizeof(get_input_response_211),
				test_get_input_21,
				expect_response_and_finish);
	stktest_add_test("Get Input 3.1", "RequestInput",
				get_input_311, sizeof(get_input_311),
				get_input_response_311,
				sizeof(get_input_response_311),
				test_get_input_31,
				expect_response_and_finish);
	stktest_add_test("Get Input 3.2", "RequestInput",
				get_input_321, sizeof(get_input_321),
				get_input_response_321,
				sizeof(get_input_response_321),
				test_get_input_32,
				expect_response_and_finish);
	stktest_add_test("Get Input 4.1", "RequestInput",
				get_input_411, sizeof(get_input_411),
				get_input_response_411,
				sizeof(get_input_response_411),
				test_get_input_41,
				expect_response_and_finish);
	stktest_add_test("Get Input 4.2", "RequestInput",
				get_input_421, sizeof(get_input_421),
				get_input_response_421,
				sizeof(get_input_response_421),
				test_get_input_42,
				expect_response_and_finish);
	stktest_add_test("Get Input 5.1", "RequestDigits",
				get_input_511, sizeof(get_input_511),
				get_input_response_511,
				sizeof(get_input_response_511),
				test_get_input_51,
				expect_response_and_finish);
	stktest_add_test("Get Input 5.2", "RequestDigits",
				get_input_521, sizeof(get_input_521),
				get_input_response_521,
				sizeof(get_input_response_521),
				test_get_input_52,
				expect_response_and_finish);
	stktest_add_test("Get Input 6.1", "RequestDigits",
				get_input_611, sizeof(get_input_611),
				get_input_response_611a,
				sizeof(get_input_response_611a),
				test_get_input_61,
				expect_response_and_finish);
	stktest_add_test("Get Input 6.2", "RequestDigits",
				get_input_621, sizeof(get_input_621),
				get_input_response_621a,
				sizeof(get_input_response_621a),
				test_get_input_62,
				expect_response_and_finish);
	stktest_add_test("Get Input 6.3", "RequestDigits",
				get_input_631, sizeof(get_input_631),
				get_input_response_631a,
				sizeof(get_input_response_631a),
				test_get_input_63,
				expect_response_and_finish);
	stktest_add_test("Get Input 6.4", "RequestDigits",
				get_input_641, sizeof(get_input_641),
				get_input_response_641a,
				sizeof(get_input_response_641a),
				test_get_input_64,
				expect_response_and_finish);
	/* GetInput 7.1 skipped, Help not supported */
	stktest_add_test("Get Input 8.1", "RequestDigits",
				get_input_811, sizeof(get_input_811),
				get_input_response_811,
				sizeof(get_input_response_811),
				test_get_input_81,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.2", "RequestDigits",
				get_input_821, sizeof(get_input_821),
				get_input_response_821,
				sizeof(get_input_response_821),
				test_get_input_82,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.3", "RequestDigits",
				get_input_831, sizeof(get_input_831),
				get_input_response_831,
				sizeof(get_input_response_831),
				test_get_input_83,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.4", "RequestDigits",
				get_input_841, sizeof(get_input_841),
				get_input_response_841,
				sizeof(get_input_response_841),
				test_get_input_84,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.5", "RequestDigits",
				get_input_851, sizeof(get_input_851),
				get_input_response_851,
				sizeof(get_input_response_851),
				test_get_input_85,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.6", "RequestDigits",
				get_input_861, sizeof(get_input_861),
				get_input_response_861,
				sizeof(get_input_response_861),
				test_get_input_86,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.7", "RequestDigits",
				get_input_871, sizeof(get_input_871),
				get_input_response_871,
				sizeof(get_input_response_871),
				test_get_input_87,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.8", "RequestDigits",
				get_input_881, sizeof(get_input_881),
				get_input_response_881,
				sizeof(get_input_response_881),
				test_get_input_88,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.9", "RequestDigits",
				get_input_891, sizeof(get_input_891),
				get_input_response_891,
				sizeof(get_input_response_891),
				test_get_input_89,
				expect_response_and_finish);
	stktest_add_test("Get Input 8.10", "RequestDigits",
				get_input_8101, sizeof(get_input_8101),
				get_input_response_8101,
				sizeof(get_input_response_8101),
				test_get_input_810,
				expect_response_and_finish);
	stktest_add_test("Get Input 9.1", "RequestInput",
				get_input_911, sizeof(get_input_911),
				get_input_response_911,
				sizeof(get_input_response_911),
				test_get_input_91,
				expect_response_and_finish);
	stktest_add_test("Get Input 9.2", "RequestInput",
				get_input_921, sizeof(get_input_921),
				get_input_response_921,
				sizeof(get_input_response_921),
				test_get_input_92,
				expect_response_and_finish);
	stktest_add_test("Get Input 10.1", "RequestInput",
				get_input_1011, sizeof(get_input_1011),
				get_input_response_1011,
				sizeof(get_input_response_1011),
				test_get_input_101,
				expect_response_and_finish);
	stktest_add_test("Get Input 10.2", "RequestInput",
				get_input_1021, sizeof(get_input_1021),
				get_input_response_1021,
				sizeof(get_input_response_1021),
				test_get_input_102,
				expect_response_and_finish);
	stktest_add_test("Get Input 11.1", "RequestInput",
				get_input_1111, sizeof(get_input_1111),
				get_input_response_1111,
				sizeof(get_input_response_1111),
				test_get_input_111,
				expect_response_and_finish);
	stktest_add_test("Get Input 11.2", "RequestInput",
				get_input_1121, sizeof(get_input_1121),
				get_input_response_1121,
				sizeof(get_input_response_1121),
				test_get_input_112,
				expect_response_and_finish);
	stktest_add_test("Get Input 12.1", "RequestInput",
				get_input_1211, sizeof(get_input_1211),
				get_input_response_1211,
				sizeof(get_input_response_1211),
				test_get_input_121,
				expect_response_and_finish);
	stktest_add_test("Get Input 12.2", "RequestInput",
				get_input_1221, sizeof(get_input_1221),
				get_input_response_1221,
				sizeof(get_input_response_1221),
				test_get_input_122,
				expect_response_and_finish);
	stktest_add_test("More Time 1.1", NULL,
				more_time_111, sizeof(more_time_111),
				more_time_response_111,
				sizeof(more_time_response_111),
				NULL, expect_response_and_finish);
	stktest_add_timed_test("Play Tone 1.1a", "LoopTone",
				play_tone_111, sizeof(play_tone_111),
				play_tone_response_111,
				sizeof(play_tone_response_111),
				test_play_tone_11a,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_timed_test("Play Tone 1.1b", "LoopTone",
				play_tone_112, sizeof(play_tone_112),
				play_tone_response_112,
				sizeof(play_tone_response_112),
				test_play_tone_11b,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_timed_test("Play Tone 1.1c", "LoopTone",
				play_tone_113, sizeof(play_tone_113),
				play_tone_response_113,
				sizeof(play_tone_response_113),
				test_play_tone_11c,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_test("Play Tone 1.1d", "PlayTone",
				play_tone_114, sizeof(play_tone_114),
				play_tone_response_114,
				sizeof(play_tone_response_114),
				test_play_tone_11d,
				expect_response_and_finish);
	stktest_add_timed_test("Play Tone 1.1e", "LoopTone",
				play_tone_115, sizeof(play_tone_115),
				play_tone_response_115,
				sizeof(play_tone_response_115),
				test_play_tone_11e,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_timed_test("Play Tone 1.1f", "LoopTone",
				play_tone_116, sizeof(play_tone_116),
				play_tone_response_116,
				sizeof(play_tone_response_116),
				test_play_tone_11f,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_timed_test("Play Tone 1.1g", "LoopTone",
				play_tone_117, sizeof(play_tone_117),
				play_tone_response_117,
				sizeof(play_tone_response_117),
				test_play_tone_11g,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_timed_test("Play Tone 1.1h", "LoopTone",
				play_tone_118, sizeof(play_tone_118),
				play_tone_response_118,
				sizeof(play_tone_response_118),
				test_play_tone_11h,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_test("Play Tone 1.1i", "PlayTone",
				play_tone_119, sizeof(play_tone_119),
				play_tone_response_119,
				sizeof(play_tone_response_119),
				test_play_tone_11i,
				expect_response_and_finish);
	stktest_add_test("Play Tone 1.1j", "PlayTone",
				play_tone_1110, sizeof(play_tone_1110),
				play_tone_response_1110,
				sizeof(play_tone_response_1110),
				test_play_tone_11j,
				expect_response_and_finish);
	stktest_add_test("Play Tone 1.1k", "PlayTone",
				play_tone_1111, sizeof(play_tone_1111),
				play_tone_response_1111,
				sizeof(play_tone_response_1111),
				test_play_tone_11k,
				expect_response_and_finish);
	stktest_add_test("Play Tone 1.1l", "PlayTone",
				play_tone_1112, sizeof(play_tone_1112),
				play_tone_response_1112,
				sizeof(play_tone_response_1112),
				test_play_tone_11l,
				expect_response_and_finish);
	stktest_add_test("Play Tone 1.1m", "PlayTone",
				play_tone_1113, sizeof(play_tone_1113),
				play_tone_response_1113,
				sizeof(play_tone_response_1113),
				test_play_tone_11m,
				expect_response_and_finish);
	stktest_add_test("Play Tone 1.1n", "LoopTone",
				play_tone_1114, sizeof(play_tone_1114),
				play_tone_response_1114,
				sizeof(play_tone_response_1114),
				test_play_tone_11n,
				expect_response_and_finish);
	stktest_add_test("Play Tone 1.1o", "PlayTone",
				play_tone_1115, sizeof(play_tone_1115),
				play_tone_response_1115,
				sizeof(play_tone_response_1115),
				test_play_tone_11o,
				expect_response_and_finish);
	stktest_add_test("Play Tone 2.1a", "PlayTone",
				play_tone_211, sizeof(play_tone_211),
				play_tone_response_211,
				sizeof(play_tone_response_211),
				test_play_tone_21,
				expect_response_and_finish);
	stktest_add_test("Play Tone 2.1b", "PlayTone",
				play_tone_212, sizeof(play_tone_212),
				play_tone_response_212,
				sizeof(play_tone_response_212),
				test_play_tone_21,
				expect_response_and_finish);
	stktest_add_test("Play Tone 2.1c", "PlayTone",
				play_tone_213, sizeof(play_tone_213),
				play_tone_response_213,
				sizeof(play_tone_response_213),
				test_play_tone_21,
				expect_response_and_finish);
	stktest_add_test("Play Tone 3.1", "PlayTone",
				play_tone_311, sizeof(play_tone_311),
				play_tone_response_311,
				sizeof(play_tone_response_311),
				test_play_tone_31,
				expect_response_and_finish);
	stktest_add_test("Play Tone 3.2", "PlayTone",
				play_tone_321, sizeof(play_tone_321),
				play_tone_response_321,
				sizeof(play_tone_response_321),
				test_play_tone_32,
				expect_response_and_finish);
	stktest_add_test("Play Tone 3.3", "PlayTone",
				play_tone_331, sizeof(play_tone_331),
				play_tone_response_331,
				sizeof(play_tone_response_331),
				test_play_tone_33,
				expect_response_and_finish);
	stktest_add_test("Play Tone 3.4", "PlayTone",
				play_tone_341, sizeof(play_tone_341),
				play_tone_response_341,
				sizeof(play_tone_response_341),
				test_play_tone_34,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.1a", "PlayTone",
				play_tone_411, sizeof(play_tone_411),
				play_tone_response_411,
				sizeof(play_tone_response_411),
				test_play_tone_41a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.1b", "PlayTone",
				play_tone_412, sizeof(play_tone_412),
				play_tone_response_412,
				sizeof(play_tone_response_412),
				test_play_tone_41b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.2a", "PlayTone",
				play_tone_421, sizeof(play_tone_421),
				play_tone_response_421,
				sizeof(play_tone_response_421),
				test_play_tone_42a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.2b", "PlayTone",
				play_tone_422, sizeof(play_tone_422),
				play_tone_response_422,
				sizeof(play_tone_response_422),
				test_play_tone_42b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.3a", "PlayTone",
				play_tone_431, sizeof(play_tone_431),
				play_tone_response_431,
				sizeof(play_tone_response_431),
				test_play_tone_43a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.3b", "PlayTone",
				play_tone_432, sizeof(play_tone_432),
				play_tone_response_432,
				sizeof(play_tone_response_432),
				test_play_tone_43b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.4a", "PlayTone",
				play_tone_441, sizeof(play_tone_441),
				play_tone_response_441,
				sizeof(play_tone_response_441),
				test_play_tone_44a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.4b", "PlayTone",
				play_tone_442, sizeof(play_tone_442),
				play_tone_response_442,
				sizeof(play_tone_response_442),
				test_play_tone_44b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.4c", "PlayTone",
				play_tone_443, sizeof(play_tone_443),
				play_tone_response_443,
				sizeof(play_tone_response_443),
				test_play_tone_44c,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.5a", "PlayTone",
				play_tone_451, sizeof(play_tone_451),
				play_tone_response_451,
				sizeof(play_tone_response_451),
				test_play_tone_45a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.5b", "PlayTone",
				play_tone_452, sizeof(play_tone_452),
				play_tone_response_452,
				sizeof(play_tone_response_452),
				test_play_tone_45b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.5c", "PlayTone",
				play_tone_453, sizeof(play_tone_453),
				play_tone_response_453,
				sizeof(play_tone_response_453),
				test_play_tone_45c,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.6a", "PlayTone",
				play_tone_461, sizeof(play_tone_461),
				play_tone_response_461,
				sizeof(play_tone_response_461),
				test_play_tone_46a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.6b", "PlayTone",
				play_tone_462, sizeof(play_tone_462),
				play_tone_response_462,
				sizeof(play_tone_response_462),
				test_play_tone_46b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.6c", "PlayTone",
				play_tone_463, sizeof(play_tone_463),
				play_tone_response_463,
				sizeof(play_tone_response_463),
				test_play_tone_46c,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.7a", "PlayTone",
				play_tone_471, sizeof(play_tone_471),
				play_tone_response_471,
				sizeof(play_tone_response_471),
				test_play_tone_47a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.7b", "PlayTone",
				play_tone_472, sizeof(play_tone_472),
				play_tone_response_472,
				sizeof(play_tone_response_472),
				test_play_tone_47b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.7c", "PlayTone",
				play_tone_473, sizeof(play_tone_473),
				play_tone_response_473,
				sizeof(play_tone_response_473),
				test_play_tone_47c,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.8a", "PlayTone",
				play_tone_481, sizeof(play_tone_481),
				play_tone_response_481,
				sizeof(play_tone_response_481),
				test_play_tone_48a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.8b", "PlayTone",
				play_tone_482, sizeof(play_tone_482),
				play_tone_response_482,
				sizeof(play_tone_response_482),
				test_play_tone_48b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.8c", "PlayTone",
				play_tone_483, sizeof(play_tone_483),
				play_tone_response_483,
				sizeof(play_tone_response_483),
				test_play_tone_48c,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.9a", "PlayTone",
				play_tone_491, sizeof(play_tone_491),
				play_tone_response_491,
				sizeof(play_tone_response_491),
				test_play_tone_49a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.9b", "PlayTone",
				play_tone_492, sizeof(play_tone_492),
				play_tone_response_492,
				sizeof(play_tone_response_492),
				test_play_tone_49b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.9c", "PlayTone",
				play_tone_493, sizeof(play_tone_493),
				play_tone_response_493,
				sizeof(play_tone_response_493),
				test_play_tone_49c,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.10a", "PlayTone",
				play_tone_4101, sizeof(play_tone_4101),
				play_tone_response_4101,
				sizeof(play_tone_response_4101),
				test_play_tone_410a,
				expect_response_and_finish);
	stktest_add_test("Play Tone 4.10b", "PlayTone",
				play_tone_4102, sizeof(play_tone_4102),
				play_tone_response_4102,
				sizeof(play_tone_response_4102),
				test_play_tone_410b,
				expect_response_and_finish);
	stktest_add_test("Play Tone 5.1a", "PlayTone",
				play_tone_511, sizeof(play_tone_511),
				play_tone_response_511,
				sizeof(play_tone_response_511),
				test_play_tone_51,
				expect_response_and_finish);
	stktest_add_test("Play Tone 5.1b", "PlayTone",
				play_tone_512, sizeof(play_tone_512),
				play_tone_response_512,
				sizeof(play_tone_response_512),
				test_play_tone_51,
				expect_response_and_finish);
	stktest_add_test("Play Tone 5.1c", "PlayTone",
				play_tone_513, sizeof(play_tone_513),
				play_tone_response_513,
				sizeof(play_tone_response_513),
				test_play_tone_51,
				expect_response_and_finish);
	stktest_add_timed_test("Play Tone 6.1a", "LoopTone",
				play_tone_611, sizeof(play_tone_611),
				play_tone_response_611,
				sizeof(play_tone_response_611),
				test_play_tone_61a,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_timed_test("Play Tone 6.1b", "LoopTone",
				play_tone_612, sizeof(play_tone_612),
				play_tone_response_612,
				sizeof(play_tone_response_612),
				test_play_tone_61b,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_timed_test("Play Tone 6.1c", "LoopTone",
				play_tone_613, sizeof(play_tone_613),
				play_tone_response_613,
				sizeof(play_tone_response_613),
				test_play_tone_61c,
				expect_response_and_finish,
				5.0, 6.0);
	stktest_add_test("Poll Interval 1.1", NULL,
				poll_interval_111, sizeof(poll_interval_111),
				poll_interval_response_111,
				sizeof(poll_interval_response_111),
				NULL, expect_response_and_finish);
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
	unsigned int not_run = 0;
	unsigned int passed = 0;
	unsigned int failed = 0;

	g_print("\n\nTest Summary\n");
	g_print("============\n");

	for (l = tests; l; l = l->next) {
		struct test *test = l->data;

		g_print("%-60s", test->name);

		switch (test->result) {
		case TEST_RESULT_NOT_RUN:
			g_print("Not Run\n");
			not_run += 1;
			break;
		case TEST_RESULT_PASSED:
			g_print("Passed\n");
			passed += 1;
			break;
		case TEST_RESULT_FAILED:
			g_print("Failed\n");
			failed += 1;
		break;
		}
	}

	g_print("\nTotal: %d, Passed: %d(%.1f%%), Failed: %d, NotRun: %d\n",
			not_run + passed + failed, passed,
			(float) passed * 100 / (not_run + passed + failed),
			failed, not_run);
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

	timer = g_timer_new();

	g_main_loop_run(main_loop);

	g_timer_destroy(timer);

	g_dbus_remove_watch(conn, watch);

	if (ofono_running == TRUE)
		ofono_disconnect(conn, NULL);

	dbus_connection_unref(conn);

	g_main_loop_unref(main_loop);

	__stktest_test_summarize();
	__stktest_test_cleanup();

	return 0;
}
