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

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>

#include <glib.h>

#include <ofono/log.h>

#include "session.h"

struct modem_session_callback {
	modem_session_callback_t callback;
	gpointer user_data;
	GDestroyNotify notify;
	guint timeout_watcher;
	GIOChannel *io;
};

static void connect_destroy(gpointer user)
{
	struct modem_session_callback *callback = user;

	if (callback->notify)
		callback->notify(callback->user_data);

	if (callback->timeout_watcher != 0)
		g_source_remove(callback->timeout_watcher);

	g_free(callback);
}

static gboolean connect_cb(GIOChannel *io, GIOCondition cond, gpointer user)
{
	struct modem_session_callback *callback = user;
	int err = 0;
	gboolean success;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & G_IO_OUT) {
		int sock = g_io_channel_unix_get_fd(io);
		socklen_t len = sizeof(err);

		if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
			err = errno == ENOTSOCK ? 0 : errno;
	} else if (cond & (G_IO_HUP | G_IO_ERR))
		err = ECONNRESET;

	success = !err;

	callback->callback(io, success, callback->user_data);

	return FALSE;
}

static gboolean connect_timeout(gpointer user)
{
	struct modem_session_callback *callback = user;

	callback->callback(callback->io, FALSE, callback->user_data);

	callback->timeout_watcher = 0;

	g_io_channel_unref(callback->io);

	return FALSE;
}

static GIOChannel *tty_connect(const char *tty)
{
	GIOChannel *io;
	int sk;
	struct termios newtio;

	sk = open(tty, O_RDWR | O_NOCTTY);

	if (sk < 0) {
		ofono_error("Can't open TTY %s: %s(%d)",
				tty, strerror(errno), errno);
		return NULL;
	}

	newtio.c_cflag = B115200 | CRTSCTS | CLOCAL | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;
	newtio.c_lflag = 0;

	newtio.c_cc[VTIME] = 1;
	newtio.c_cc[VMIN] = 5;

	tcflush(sk, TCIFLUSH);
	if (tcsetattr(sk, TCSANOW, &newtio) < 0) {
		ofono_error("Can't change serial settings: %s(%d)",
				strerror(errno), errno);
		close(sk);
		return NULL;
	}

	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, TRUE);

	if (g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK,
					NULL) != G_IO_STATUS_NORMAL) {
		g_io_channel_unref(io);
		return NULL;
	}

	return io;
}

static GIOChannel *socket_common(int sk, struct sockaddr *addr,
					socklen_t addrlen)
{
	GIOChannel *io = g_io_channel_unix_new(sk);

	if (io == NULL) {
		close(sk);
		return NULL;
	}

	g_io_channel_set_close_on_unref(io, TRUE);

	if (g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK,
					NULL) != G_IO_STATUS_NORMAL) {
		g_io_channel_unref(io);
		return NULL;
	}

	if (connect(sk, addr, addrlen) < 0) {
		if (errno != EAGAIN && errno != EINPROGRESS) {
			g_io_channel_unref(io);
			return NULL;
		}
	}

	return io;
}

static GIOChannel *unix_connect(const char *address)
{
	struct sockaddr_un addr;
	int sk;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = PF_UNIX;

	if (strncmp("x00", address, 3) == 0)
		strcpy(addr.sun_path + 1, address + 3);
	else
		strcpy(addr.sun_path, address);

	sk = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sk < 0)
		return NULL;

	return socket_common(sk, (struct sockaddr *)&addr, sizeof(addr));
}

static GIOChannel *tcp_connect(const char *address)
{
	struct sockaddr_in addr;
	int sk;
	unsigned short port;
	in_addr_t inetaddr;
	char *portstr;
	char addrstr[16];

	memset(&addr, 0, sizeof(addr));

	portstr = strchr(address, ':');

	if (!portstr || (unsigned int)(portstr-address) > (sizeof(addrstr) - 1))
		return NULL;

	strncpy(addrstr, address, portstr-address);
	addrstr[portstr-address] = '\0';

	portstr += 1;

	port = atoi(portstr);

	if (port == 0)
		return NULL;

	inetaddr = inet_addr(addrstr);

	if (inetaddr == INADDR_NONE)
		return NULL;

	sk = socket(PF_INET, SOCK_STREAM, 0);

	if (sk < 0)
		return NULL;

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inetaddr;
	addr.sin_port = htons(port);

	return socket_common(sk, (struct sockaddr *) &addr, sizeof(addr));
}

GIOChannel *modem_session_create(const char *target,
					modem_session_callback_t func,
					gpointer user_data,
					GDestroyNotify notify)
{
	struct modem_session_callback *callback;
	GIOChannel *io = NULL;
	GIOCondition cond;

	if (target == NULL || func == NULL)
		return NULL;

	if (!strncasecmp(target, "tcp:", 4))
		io = tcp_connect(target+4);
	else if (!strncasecmp(target, "unix:", 5))
		io = unix_connect(target+5);
	else if (!strncasecmp(target, "dev:", 4))
		io = tty_connect(target+4);

	if (io == NULL)
		return NULL;

	callback = g_new0(struct modem_session_callback, 1);

	callback->callback = func;
	callback->user_data = user_data;
	callback->notify = notify;
	callback->io = io;
	callback->timeout_watcher = g_timeout_add_seconds(20, connect_timeout,
								callback);

	cond = G_IO_OUT | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	g_io_add_watch_full(io, G_PRIORITY_DEFAULT, cond, connect_cb,
				callback, connect_destroy);

	g_io_channel_unref(io);

	return io;
}
