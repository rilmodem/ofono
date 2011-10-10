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
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include <gdbus.h>

#define OFONO_SERVICE	"org.ofono"

#define OFONO_MANAGER_INTERFACE		OFONO_SERVICE ".Manager"
#define OFONO_MODEM_INTERFACE		OFONO_SERVICE ".Modem"
#define OFONO_CALLMANAGER_INTERFACE	OFONO_SERVICE ".VoiceCallManager"
#define OFONO_CALL_INTERFACE		OFONO_SERVICE ".VoiceCall"
#define OFONO_AUDIO_INTERFACE		OFONO_SERVICE ".AudioSettings"

struct modem_data {
	char *path;
	GHashTable *call_list;

	DBusConnection *conn;
	guint call_added_watch;
	guint call_removed_watch;
	guint call_changed_watch;
	guint audio_changed_watch;

	gboolean has_callmanager;
	gboolean has_audiosettings;
	gboolean is_huawei;
	gint audio_users;
	guint audio_watch;

	int format;
	int channels;
	int speed;
	int dsp_out;
};

struct call_data {
	char *path;
	struct modem_data *modem;
};

static GHashTable *modem_list;

static gboolean audio_receive(GIOChannel *channel,
				GIOCondition condition, gpointer user_data)
{
	struct modem_data *modem = user_data;
	char buf[512];
	ssize_t rlen, wlen;
	int fd;

	if (condition & (G_IO_NVAL | G_IO_ERR)) {
		modem->audio_watch = 0;
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd(channel);

	rlen = read(fd, buf, sizeof(buf));
	if (rlen < 0)
		return TRUE;

	wlen = write(modem->dsp_out, buf, rlen);
	if (wlen < 0) {
		modem->audio_watch = 0;
		return FALSE;
	}

	return TRUE;
}

static void open_audio(struct modem_data *modem)
{
	GIOChannel *channel;
	struct termios ti;
	int fd;

	if (modem->is_huawei == FALSE)
		return;

	if (modem->audio_users > 0)
		return;

	g_print("enabling audio\n");

	modem->dsp_out = open("/dev/dsp", O_WRONLY, 0);
	if (modem->dsp_out < 0) {
		g_printerr("Failed to open DSP device\n");
		return;
	}

	if (ioctl(modem->dsp_out, SNDCTL_DSP_SETFMT, &modem->format) < 0)
		g_printerr("Failed to set DSP format\n");

	if (ioctl(modem->dsp_out, SNDCTL_DSP_CHANNELS, &modem->channels) < 0)
		g_printerr("Failed to set DSP channels\n");

	if (ioctl(modem->dsp_out, SNDCTL_DSP_SPEED, &modem->speed) < 0)
		g_printerr("Failed to set DSP speed\n");

	fd = open("/dev/ttyUSB1", O_RDWR | O_NOCTTY);
	if (fd < 0) {
		g_printerr("Failed to open audio port\n");
		close(modem->dsp_out);
		modem->dsp_out = -1;
		return;
	}

	/* Switch TTY to raw mode */
	memset(&ti, 0, sizeof(ti));
	cfmakeraw(&ti);

	tcflush(fd, TCIOFLUSH);
	tcsetattr(fd, TCSANOW, &ti);

	channel = g_io_channel_unix_new(fd);
	if (channel == NULL) {
		g_printerr("Failed to create IO channel\n");
		close(modem->dsp_out);
		modem->dsp_out = -1;
		close(fd);
		return;
	}

	g_io_channel_set_close_on_unref(channel, TRUE);

	modem->audio_watch = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				audio_receive, modem);

	g_io_channel_unref(channel);

	modem->audio_users++;
}

static void close_audio(struct modem_data *modem)
{
	if (modem->is_huawei == FALSE)
		return;

	modem->audio_users--;

	if (modem->audio_users > 0)
		return;

	g_print("disabling audio\n");

	if (modem->audio_watch > 0) {
		g_source_remove(modem->audio_watch);
		modem->audio_watch = 0;
	}

	close(modem->dsp_out);
}

static void audio_set(struct modem_data *modem, const char *key,
						DBusMessageIter *iter)
{
	const char *str = NULL;

	if (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_STRING)
		dbus_message_iter_get_basic(iter, &str);

	if (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_BOOLEAN) {
		dbus_bool_t val;

		dbus_message_iter_get_basic(iter, &val);
		str = (val == TRUE) ? "yes" : "no";
	}

	g_print("updating audio (%s) [ %s = %s ]\n", modem->path,
						key, str ? str : "...");
}

static void call_set(struct call_data *call, const char *key,
						DBusMessageIter *iter)
{
	const char *str = NULL;

	if (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_STRING)
		dbus_message_iter_get_basic(iter, &str);

	if (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_BOOLEAN) {
		dbus_bool_t val;

		dbus_message_iter_get_basic(iter, &val);
		str = (val == TRUE) ? "yes" : "no";
	}

	g_print("updating call (%s) [ %s = %s ]\n", call->path,
						key, str ? str : "...");
}

static void destroy_call(gpointer data)
{
	struct call_data *call = data;

	g_print("call removed (%s)\n", call->path);

	close_audio(call->modem);

	g_free(call->path);
	g_free(call);
}

static void create_call(struct modem_data *modem,
				const char *path, DBusMessageIter *iter)
{
	struct call_data *call;
	DBusMessageIter dict;

	call = g_try_new0(struct call_data, 1);
	if (call == NULL)
		return;

	call->path = g_strdup(path);

	g_hash_table_replace(modem->call_list, call->path, call);

	g_print("call added (%s)\n", call->path);

	call->modem = modem;

	open_audio(modem);

	dbus_message_iter_recurse(iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		call_set(call, key, &value);

		dbus_message_iter_next(&dict);
	}
}

static gboolean call_added(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	struct modem_data *modem = user_data;
	DBusMessageIter iter, dict;
	const char *path;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &path);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &dict);

	create_call(modem, path, &iter);

	return TRUE;
}

static gboolean call_removed(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	struct modem_data *modem = user_data;
	DBusMessageIter iter;
	const char *path;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &path);

	g_hash_table_remove(modem->call_list, path);

	return TRUE;
}

static gboolean call_changed(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	struct modem_data *modem = user_data;
	struct call_data *call;
	DBusMessageIter iter, value;
	const char *path, *key;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return TRUE;

	path = dbus_message_get_path(msg);

	call = g_hash_table_lookup(modem->call_list, path);
	if (call == NULL)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &key);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	call_set(call, key, &value);

	return TRUE;
}

static gboolean audio_changed(DBusConnection *conn,
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

	audio_set(modem, key, &value);

	return TRUE;
}

static void get_calls_reply(DBusPendingCall *call, void *user_data)
{
	struct modem_data *modem = user_data;
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

		create_call(modem, path, &dict);

		dbus_message_iter_next(&list);
	}

done:
	dbus_message_unref(reply);
}

static int get_calls(struct modem_data *modem)
{
	DBusMessage *msg;
	DBusPendingCall *call;

	msg = dbus_message_new_method_call(OFONO_SERVICE, modem->path,
				OFONO_CALLMANAGER_INTERFACE, "GetCalls");
	if (msg == NULL)
		return -ENOMEM;

	dbus_message_set_auto_start(msg, FALSE);

	g_print("getting calls (%s)\n", modem->path);

	if (dbus_connection_send_with_reply(modem->conn, msg,
						&call, -1) == FALSE) {
		dbus_message_unref(msg);
		return -EIO;
	}

	dbus_message_unref(msg);

	if (call == NULL)
		return -EINVAL;

	dbus_pending_call_set_notify(call, get_calls_reply, modem, NULL);

	dbus_pending_call_unref(call);

	return 0;
}

static void check_interfaces(struct modem_data *modem, DBusMessageIter *iter)
{
	DBusMessageIter entry;
	gboolean has_callmanager = FALSE;
	gboolean has_audiosettings = FALSE;

	dbus_message_iter_recurse(iter, &entry);

	while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
		const char *interface;

		dbus_message_iter_get_basic(&entry, &interface);

		if (g_str_equal(interface, OFONO_CALLMANAGER_INTERFACE) == TRUE)
			has_callmanager = TRUE;

		if (g_str_equal(interface, OFONO_AUDIO_INTERFACE) == TRUE)
			has_audiosettings = TRUE;

		dbus_message_iter_next(&entry);
	}

	modem->has_audiosettings = has_audiosettings;

	if (modem->has_callmanager == has_callmanager)
		return;

	modem->has_callmanager = has_callmanager;
	if (modem->has_callmanager == TRUE)
		get_calls(modem);
}

static void check_manufacturer(struct modem_data *modem, DBusMessageIter *iter)
{
	const char *manufacturer;

	dbus_message_iter_get_basic(iter, &manufacturer);

	if (g_str_equal(manufacturer, "huawei") == TRUE) {
		g_print("found Huawei modem\n");
		modem->is_huawei = TRUE;
	}
}

static void destroy_modem(gpointer data)
{
	struct modem_data *modem = data;

	g_dbus_remove_watch(modem->conn, modem->call_added_watch);
	g_dbus_remove_watch(modem->conn, modem->call_removed_watch);
	g_dbus_remove_watch(modem->conn, modem->call_changed_watch);
	g_dbus_remove_watch(modem->conn, modem->audio_changed_watch);

	g_hash_table_destroy(modem->call_list);

	g_print("modem removed (%s)\n", modem->path);

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

	modem->format = AFMT_S16_LE;
	modem->channels = 1;
	modem->speed = 8000;
	modem->dsp_out = -1;

	modem->call_list = g_hash_table_new_full(g_str_hash, g_str_equal,
							NULL, destroy_call);

	modem->conn = conn;

	modem->call_added_watch = g_dbus_add_signal_watch(conn, NULL,
				modem->path, OFONO_CALLMANAGER_INTERFACE,
				"CallAdded", call_added, modem, NULL);
	modem->call_removed_watch = g_dbus_add_signal_watch(conn, NULL,
				modem->path, OFONO_CALLMANAGER_INTERFACE,
				"CallRemoved", call_removed, modem, NULL);
	modem->call_changed_watch = g_dbus_add_signal_watch(conn, NULL,
				NULL, OFONO_CALL_INTERFACE,
				"PropertyChanged", call_changed, modem, NULL);
	modem->audio_changed_watch = g_dbus_add_signal_watch(conn, NULL,
				NULL, OFONO_AUDIO_INTERFACE,
				"PropertyChanged", audio_changed, modem, NULL);

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

		if (g_str_equal(key, "Interfaces") == TRUE)
			check_interfaces(modem, &value);
		else if (g_str_equal(key, "Manufacturer") == TRUE)
			check_manufacturer(modem, &value);

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

	if (g_str_equal(key, "Interfaces") == TRUE)
		check_interfaces(modem, &value);
	else if (g_str_equal(key, "Manufacturer") == TRUE)
		check_manufacturer(modem, &value);

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

	modem_added_watch = g_dbus_add_signal_watch(conn, NULL, NULL,
				OFONO_MANAGER_INTERFACE, "ModemAdded",
						modem_added, NULL, NULL);
	modem_removed_watch = g_dbus_add_signal_watch(conn, NULL, NULL,
				OFONO_MANAGER_INTERFACE, "ModemRemoved",
						modem_removed, NULL, NULL);
	modem_changed_watch = g_dbus_add_signal_watch(conn, NULL, NULL,
				OFONO_MODEM_INTERFACE, "PropertyChanged",
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
