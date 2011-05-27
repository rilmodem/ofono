/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
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

#define OFONO_SERVICE "org.ofono"

#define MANAGER_PATH	"/"
#define MANAGER_INTERFACE OFONO_SERVICE ".Manager"
#define LOCATION_REPORTING_INTERFACE OFONO_SERVICE ".LocationReporting"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <glib.h>

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

static GMainLoop *event_loop;

static char *get_first_modem_path(DBusConnection *conn)
{
	DBusMessage *msg, *reply;
	DBusMessageIter iter, array, entry;
	DBusError error;
	int arg_type;
	const char *path;

	msg = dbus_message_new_method_call(OFONO_SERVICE, MANAGER_PATH,
						MANAGER_INTERFACE, "GetModems");

	dbus_error_init(&error);

	reply = dbus_connection_send_with_reply_and_block(conn, msg, -1,
									&error);

	dbus_message_unref(msg);

	if (!reply) {
		if (dbus_error_is_set(&error)) {
			fprintf(stderr, "%s\n", error.message);
			dbus_error_free(&error);
		} else {
			fprintf(stderr, "GetModems failed");
		}


		return NULL;
	}

	dbus_message_iter_init(reply, &iter);

	dbus_message_iter_recurse(&iter, &array);
	dbus_message_iter_recurse(&array, &entry);

	arg_type = dbus_message_iter_get_arg_type(&entry);
	while (arg_type != DBUS_TYPE_INVALID &&
					arg_type != DBUS_TYPE_OBJECT_PATH) {
		dbus_message_iter_next(&entry);
		arg_type = dbus_message_iter_get_arg_type(&entry);
	}

	if (arg_type != DBUS_TYPE_OBJECT_PATH) {
		fprintf(stderr, "modem not found\n");
		return NULL;
	}

	dbus_message_iter_get_basic(&entry, &path);
	fprintf(stderr, "Using modem: %s\n", path);

	return strdup(path);
}

static gboolean data_read_cb(GIOChannel *channel, GIOCondition cond,
								gpointer data)
{
	int fd = GPOINTER_TO_INT(data);
	char buf[128];
	int ret;

	while ((ret = read(fd, buf, sizeof(buf) - 1)) >= 0) {
		buf[ret] = '\0';
		printf("%s", buf);
	}

	if (errno != EAGAIN && errno != EWOULDBLOCK)
		fprintf(stderr, "Error reading fd");

	return TRUE;
}

static int setup_data_channel(DBusConnection *conn, const char *path)
{
	DBusMessage *msg, *reply;
	DBusError error;
	int fd, fd_source;
	GIOChannel *channel;

	msg = dbus_message_new_method_call(OFONO_SERVICE, path,
				LOCATION_REPORTING_INTERFACE, "Request");

	dbus_error_init(&error);

	reply = dbus_connection_send_with_reply_and_block(conn, msg, -1,
									&error);
	dbus_message_unref(msg);

	printf("Requesting location-reporting...\n");
	if (!reply) {
		if (dbus_error_is_set(&error)) {
			fprintf(stderr, "%s\n", error.message);
			dbus_error_free(&error);
		} else {
			fprintf(stderr, "Request() failed");
		}

		return -1;
	}

	dbus_error_init(&error);

	if (dbus_message_get_args(reply, &error, DBUS_TYPE_UNIX_FD, &fd,
						DBUS_TYPE_INVALID) == FALSE) {
		fprintf(stderr, "%s\n", error.message);
		dbus_error_free(&error);

		return -1;
	}

	printf("Using fd=%d\n", fd);
	fcntl(fd, F_SETFL, O_NONBLOCK);

	channel = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(channel, TRUE);
	fd_source = g_io_add_watch(channel, G_IO_IN, data_read_cb,
							GINT_TO_POINTER(fd));
	g_io_channel_unref(channel);

	return fd_source;
}

static gboolean signal_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	int signal_fd = GPOINTER_TO_INT(data);
	struct signalfd_siginfo si;
	ssize_t len;

	len = read(signal_fd, &si, sizeof(si));
	if (len < 0)
		return TRUE;

	g_main_loop_quit(event_loop);

	return TRUE;
}

static int setup_signals(void)
{
	sigset_t mask;
	int signal_fd, signal_source;
	GIOChannel *signal_io;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		fprintf(stderr, "Can't set signal mask - %m");

		return -1;
	}

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd < 0) {
		fprintf(stderr, "Can't create signal filedescriptor - %m");

		return -1;
	}

	signal_io = g_io_channel_unix_new(signal_fd);
	g_io_channel_set_close_on_unref(signal_io, TRUE);
	signal_source = g_io_add_watch(signal_io,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			signal_cb, GINT_TO_POINTER(signal_fd));
	g_io_channel_unref(signal_io);

	return signal_source;
}

int main(int argc, char *argv[])
{
	DBusConnection *conn;
	char *modem_path;
	int signal_source;
	int data_source;
	int ret;

	if (DBUS_TYPE_UNIX_FD < 0) {
		fprintf(stderr, "File-descriptor passing not supported\n");
		exit(1);
	}

	conn = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);
	if (!conn) {
		fprintf(stderr, "Can't get on system bus\n");
		exit(1);
	}

	if (argc > 1)
		modem_path = strdup(argv[1]);
	else
		modem_path = get_first_modem_path(conn);

	if (modem_path == NULL) {
		ret = 1;
		goto out;
	}

	signal_source = setup_signals();
	if (signal_source < 0)
		goto out;

	data_source = setup_data_channel(conn, modem_path);
	if (data_source < 0) {
		g_source_remove(signal_source);
		goto out;
	}

	event_loop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(event_loop);

	ret = 0;

	g_source_remove(signal_source);
	g_source_remove(data_source);
	g_main_loop_unref(event_loop);

out:
	if (modem_path)
		free(modem_path);

	dbus_connection_unref(conn);

	return ret;
}
