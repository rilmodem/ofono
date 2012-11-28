/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <termios.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <glib.h>

#define IFX_RESET_PATH "/sys/module/hsi_ffl_tty/parameters/reset_modem"

static gchar *option_device = NULL;
static gboolean option_ifx = FALSE;

static GMainLoop *main_loop;
static bool main_terminated;

static int device_fd = -1;
static int client_fd = -1;

static guint device_watch = 0;
static guint client_watch = 0;

static gboolean shutdown_timeout(gpointer user_data)
{
	g_main_loop_quit(main_loop);

	return FALSE;
}

static void do_terminate(void)
{
	if (main_terminated)
		return;

	main_terminated = true;

	g_timeout_add_seconds(1, shutdown_timeout, NULL);
}

static gboolean signal_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct signalfd_siginfo si;
	ssize_t result;
	int fd;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	result = read(fd, &si, sizeof(si));
	if (result != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGINT:
	case SIGTERM:
		do_terminate();
		break;
	}

	return TRUE;
}

static guint create_watch(int fd, GIOFunc func)
{
	GIOChannel *channel;
	guint source;

	channel = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	source = g_io_add_watch(channel,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, func, NULL);

	g_io_channel_unref(channel);

	return source;
}

static guint setup_signalfd(void)
{
	sigset_t mask;
	int fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("Failed to set signal mask");
		return 0;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		perror("Failed to create signal descriptor");
		return 0;
	}

	return create_watch(fd, signal_handler);
}

static int write_file(const char *path, const char *value)
{
	ssize_t written;
	int fd;

	fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		perror("Failed to open file");
		return -1;
	}

	written = write(fd, value, strlen(value));
	if (written < 0) {
		perror("Failed to write value");
		return -1;
	}

	return 0;
}

static int open_device(const char *path)
{
	struct termios ti;
	int fd;

	/* Switch TTY to raw mode */
	memset(&ti, 0, sizeof(ti));
	cfmakeraw(&ti);

	fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		perror("Failed to open device");
		return -1;
	}

	tcflush(fd, TCIOFLUSH);
	tcsetattr(fd, TCSANOW, &ti);

	return fd;
}

static gboolean forward_data(GIOCondition cond, int input_fd, int output_fd)
{
	unsigned char buf[1024];
	ssize_t bytes_read, bytes_written;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	bytes_read = read(input_fd, buf, sizeof(buf));
	if (bytes_read < 0)
		return FALSE;

	bytes_written = write(output_fd, buf, bytes_read);
	if (bytes_written != bytes_read)
		return FALSE;

	return TRUE;
}

static gboolean device_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	if (forward_data(cond, device_fd, client_fd) == FALSE) {
		g_printerr("Closing device descriptor\n");
		if (client_watch > 0) {
			g_source_remove(client_watch);
			client_watch = 0;
		}

		device_watch = 0;
		return FALSE;
	}

	return TRUE;
}

static gboolean client_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	if (forward_data(cond, client_fd, device_fd) == FALSE) {
		g_printerr("Closing client connection\n");
		if (device_watch > 0) {
			g_source_remove(device_watch);
			device_watch = 0;
		}

		client_watch = 0;
		return FALSE;
	}

	return TRUE;
}

static gboolean accept_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct sockaddr_in addr;
	socklen_t addrlen;
	int fd, nfd;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	nfd = accept4(fd, (struct sockaddr *) &addr, &addrlen, SOCK_CLOEXEC);
	if (nfd < 0)
		return TRUE;

	if (device_watch > 0) {
		g_printerr("Closing previous descriptors\n");
		g_source_remove(device_watch);
		device_watch = 0;

		if (client_watch > 0) {
			g_source_remove(client_watch);
			client_watch = 0;
		}
	}

	if (option_ifx == TRUE) {
		write_file(IFX_RESET_PATH, "1");
		sleep(1);
		write_file(IFX_RESET_PATH, "0");
		sleep(1);
	}

	device_fd = open_device(option_device);
	if (device_fd < 0) {
		close(nfd);
		return TRUE;
	}

	device_watch = create_watch(device_fd, device_handler);
	if (device_watch == 0) {
		close(nfd);
		return TRUE;
	}

	client_watch = create_watch(nfd, client_handler);
	if (client_watch == 0) {
		g_source_remove(device_watch);
		device_watch = 0;
		close(nfd);
		return TRUE;
	}

	client_fd = nfd;

	return TRUE;
}

static guint setup_server(void)
{
	struct sockaddr_in addr;
	int fd, opt = 1;

	fd = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("Failed to open server socket");
		return 0;
	}

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(12345);

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Failed to bind server socket");
		close(fd);
		return 0;
	}

	if (listen(fd, 1) < 0) {
		perror("Failed to listen server socket");
		close(fd);
		return 0;
	}

	return create_watch(fd, accept_handler);
}

static GOptionEntry options[] = {
	{ "device", 0, 0, G_OPTION_ARG_STRING, &option_device,
				"Specify device to use", "DEVNODE" },
	{ "ifx", 0, 0, G_OPTION_ARG_NONE, &option_ifx,
				"Use Infineon reset handling" },
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	guint signal_watch;
	guint server_watch;

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &error) == FALSE) {
		if (error != NULL) {
			g_printerr("%s\n", error->message);
			g_error_free(error);
		} else
			g_printerr("An unknown error occurred\n");
		return EXIT_FAILURE;
	}

	g_option_context_free(context);

	if (option_device == NULL) {
		if (option_ifx == TRUE) {
			option_device = g_strdup("/dev/ttyIFX0");
		} else {
			g_printerr("No valid device specified\n");
			return EXIT_FAILURE;
		}
	}

	main_loop = g_main_loop_new(NULL, FALSE);
	signal_watch = setup_signalfd();
	server_watch = setup_server();

	g_main_loop_run(main_loop);

	g_source_remove(server_watch);
	g_source_remove(signal_watch);
	g_main_loop_unref(main_loop);

	g_free(option_device);

	return EXIT_SUCCESS;
}
