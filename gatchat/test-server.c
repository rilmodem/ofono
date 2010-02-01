/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#include <unistd.h>
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/signalfd.h>

#include <glib.h>
#include <utmp.h>
#include <pty.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "gatserver.h"
#include "ringbuffer.h"

#define DEFAULT_TCP_PORT 12346
#define DEFAULT_SOCK_PATH "./server_sock"

struct sock_server{
	int server_sock;
};

static GMainLoop *mainloop;
static GAtServer *server;
unsigned int server_watch;

static gboolean server_cleanup()
{
	if (server_watch)
		g_source_remove(server_watch);

	g_at_server_unref(server);
	server = NULL;

	unlink(DEFAULT_SOCK_PATH);

	g_main_loop_quit(mainloop);

	return FALSE;
}

static void server_debug(const char *str, void *data)
{
	g_print("%s: %s\n", (char *) data, str);
}

static void add_handler(GAtServer *server)
{
	g_at_server_set_debug(server, server_debug, "Server");
}

static void server_destroy(gpointer user)
{
	struct sock_server *data = user;

	if (data)
		g_free(data);
}

static void set_raw_mode(int fd)
{
	struct termios options;

	tcgetattr(fd, &options);

	/* Set TTY as raw mode to disable echo back of input characters
	 * when they are received from Modem to avoid feedback loop */
	options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

	tcsetattr(fd, TCSANOW, &options);
}

static GAtServer *create_tty(const char *modem_path)
{
	int master, slave;
	char pty_name[256];
	GIOChannel *server_io;
	GIOChannel *client_io;

	if (!modem_path)
		return NULL;

	if (openpty(&master, &slave, pty_name, NULL, NULL) < 0)
		return NULL;

	set_raw_mode(slave);

	client_io = g_io_channel_unix_new(slave);
	g_io_channel_set_close_on_unref(client_io, TRUE);

	g_print("new pty is created at %s\n", pty_name);

	server_io = g_io_channel_unix_new(master);

	server = g_at_server_new(server_io);
	if (!server) {
		g_io_channel_shutdown(server_io, FALSE, NULL);
		g_io_channel_unref(server_io);

		return FALSE;
	}

	add_handler(server);

	return server;
}

static gboolean on_socket_connected(GIOChannel *chan, GIOCondition cond,
							gpointer user)
{
	struct sockaddr saddr;
	unsigned int len = sizeof(saddr);
	int fd;
	GIOChannel *client_io = NULL;
	struct sock_server *data = user;

	if (cond != G_IO_IN)
		goto error;

	fd = accept(data->server_sock, &saddr, &len);
	if (fd == -1)
		goto error;

	client_io = g_io_channel_unix_new(fd);

	server = g_at_server_new(client_io);
	g_io_channel_unref(client_io);

	if (!server)
		goto error;

	add_handler(server);

	return TRUE;

error:
	if (data)
		g_free(data);

	return FALSE;
}

static struct sock_server *socket_common(int sk, struct sockaddr *addr,
						const char *modem_path)
{
	struct sock_server *sock;

	if (bind(sk, addr, sizeof(struct sockaddr)) < 0) {
		g_print("Can't bind socket: %s (%d)", strerror(errno), errno);

		close(sk);

		return NULL;
	}

	if (listen(sk, 1) < 0) {
		g_print("Can't listen on socket: %s (%d)",
						strerror(errno), errno);

		close(sk);

		return NULL;
	}

	sock = g_try_new0(struct sock_server, 1);
	if (!sock)
		return FALSE;

	sock->server_sock = sk;

	return sock;
}

static gboolean create_tcp(const char *modem_path, int port)
{
	struct sockaddr_in addr;
	int sk;
	struct sock_server *server;
	GIOChannel *server_io;

	if (!modem_path)
		return FALSE;

	sk = socket(PF_INET, SOCK_STREAM, 0);
	if (sk < 0) {
		g_print("Can't create tcp/ip socket: %s (%d)",
						strerror(errno), errno);
		return FALSE;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	server = socket_common(sk, (struct sockaddr *) &addr, modem_path);
	if (!server)
		return FALSE;

	g_print("new tcp is created at tcp port %d\n", port);

	server_io = g_io_channel_unix_new(sk);

	g_io_channel_set_close_on_unref(server_io, TRUE);

	server_watch = g_io_add_watch_full(server_io,
				G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, server, server_destroy);

	g_io_channel_unref(server_io);

	return TRUE;
}

static gboolean create_unix(const char *modem_path, const char *sock_path)
{
	struct sockaddr_un addr;
	int sk;
	struct sock_server *server;
	GIOChannel *server_io;

	if (!modem_path)
		return FALSE;

	sk = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sk < 0) {
		g_print("Can't create unix socket: %s (%d)",
						strerror(errno), errno);

		return FALSE;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	/* Unlink any existing socket for this session */
	unlink(addr.sun_path);

	server = socket_common(sk, (struct sockaddr *) &addr, modem_path);
	if (!server)
		return FALSE;

	g_print("new unix socket is created at %s\n", sock_path);

	server_io = g_io_channel_unix_new(sk);

	g_io_channel_set_close_on_unref(server_io, TRUE);

	server_watch = g_io_add_watch_full(server_io,
				G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, server, server_destroy);

	g_io_channel_unref(server_io);

	return TRUE;
}

static void test_server(int type)
{
	switch (type) {
	case 0:
		server = create_tty("/phonesim1");

		add_handler(server);
		break;
	case 1:
		if (!create_tcp("/phonesim1", DEFAULT_TCP_PORT))
			exit(-1);
		break;
	case 2:
		if (!create_unix("/phonesim1", DEFAULT_SOCK_PATH))
			exit(-1);
		break;
	}
}

static gboolean signal_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	int signal_fd = GPOINTER_TO_INT(data);
	struct signalfd_siginfo si;
	ssize_t res;

	if (cond & (G_IO_NVAL | G_IO_ERR))
		return FALSE;

	res = read(signal_fd, &si, sizeof(si));
	if (res != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGINT:
		server_cleanup();
		break;
	case SIGTERM:
		server_cleanup();
		break;
	default:
		break;
	}

	return TRUE;
}

static int create_signal_io()
{
	sigset_t mask;
	GIOChannel *signal_io;
	int signal_fd, signal_source;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		g_error("Can't set signal mask");
		return 1;
	}

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd < 0) {
		g_error("Can't create signal filedescriptor");
		return 1;
	}

	signal_io = g_io_channel_unix_new(signal_fd);

	g_io_channel_set_close_on_unref(signal_io, TRUE);

	signal_source = g_io_add_watch(signal_io,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			signal_cb, GINT_TO_POINTER(signal_fd));

	g_io_channel_unref(signal_io);

	return signal_source;
}

static void usage(void)
{
	g_print("test-server - AT Server testing\n"
		"Usage:\n");
	g_print("\ttest-server [-t type]\n");
	g_print("Types:\n"
		"\t0: Pseudo TTY port (default)\n"
		"\t1: TCP sock at port 12346)\n"
		"\t2: Unix sock at ./server_sock\n");
}

int main(int argc, char **argv)
{
	int opt, signal_source;
	int type = 0;

	while ((opt = getopt(argc, argv, "ht:")) != EOF) {
		switch (opt) {
		case 't':
			type = atoi(optarg);
			break;
		case 'h':
			usage();
			exit(1);
			break;
		default:
			break;
		}
	}

	test_server(type);

	signal_source = create_signal_io();

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);

	g_main_loop_unref(mainloop);

	g_source_remove(signal_source);

	return 0;
}
