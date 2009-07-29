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

#include <gdbus.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "ofono.h"

static GMainLoop *event_loop;

static void sig_debug(int sig)
{
	__ofono_toggle_debug();
}

static void sig_term(int sig)
{
	g_main_loop_quit(event_loop);
}

static void system_bus_disconnected(DBusConnection *conn, void *user_data)
{
	ofono_error("System bus has disconnected!");

	g_main_loop_quit(event_loop);
}

static gboolean option_detach = TRUE;
static gboolean option_debug = FALSE;

static GOptionEntry options[] = {
	{ "nodetach", 'n', G_OPTION_FLAG_REVERSE,
				G_OPTION_ARG_NONE, &option_detach,
				"Don't run as daemon in background" },
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &option_debug,
				"Enable debug information output" },
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *err = NULL;
	struct sigaction sa;
	DBusConnection *conn;
	DBusError error;

#ifdef NEED_THREADS
	if (g_thread_supported() == FALSE)
		g_thread_init(NULL);
#endif

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &err) == FALSE) {
		if (err != NULL) {
			g_printerr("%s\n", err->message);
			g_error_free(err);
			return 1;
		}

		g_printerr("An unknown error occurred\n");
		return 1;
	}

	g_option_context_free(context);

	if (option_detach == TRUE) {
		if (daemon(0, 0)) {
			perror("Can't start daemon");
			return 1;
		}
	}

	event_loop = g_main_loop_new(NULL, FALSE);

#ifdef NEED_THREADS
	if (dbus_threads_init_default() == FALSE) {
		fprintf(stderr, "Can't init usage of threads\n");
		exit(1);
	}
#endif

	__ofono_log_init(option_detach, option_debug);

	dbus_error_init(&error);

	conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, OFONO_SERVICE, &error);
	if (!conn) {
		if (dbus_error_is_set(&error) == TRUE) {
			ofono_error("Unable to hop onto D-Bus: %s",
					error.message);
			dbus_error_free(&error);
		} else
			ofono_error("Unable to hop onto D-Bus");
		goto cleanup;
	}

	g_dbus_set_disconnect_function(conn, system_bus_disconnected,
					NULL, NULL);

	__ofono_dbus_init(conn);

	__ofono_manager_init();

	__ofono_plugin_init(NULL, NULL);

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;
	sa.sa_handler = sig_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);

	sa.sa_handler = sig_debug;
	sigaction(SIGUSR2, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	g_main_loop_run(event_loop);

	__ofono_plugin_cleanup();

	__ofono_manager_cleanup();

	__ofono_dbus_cleanup();
	dbus_connection_unref(conn);

cleanup:
	g_main_loop_unref(event_loop);

	__ofono_log_cleanup();

	return 0;
}
