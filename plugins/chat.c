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
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>

#include <glib.h>
#include <gatchat.h>
#include <gatsyntax.h>

#include <ofono/log.h>

#include "chat.h"

struct chat_data {
	GIOChannel *io;
	guint timeout;

	chat_callback_t callback;
	gpointer user_data;
};

static gboolean connect_callback(GIOChannel *io,
				GIOCondition cond, gpointer user_data)
{
	struct chat_data *data = user_data;
	GAtChat *chat;
	GAtSyntax *syntax;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);

	if (!chat)
		return FALSE;

	data->callback(chat, data->user_data);

	g_at_chat_unref(chat);

	return FALSE;
}

static gboolean connect_timeout(gpointer user_data)
{
	struct chat_data *data = user_data;

	data->timeout = 0;

	g_io_channel_close(data->io);

	data->callback(NULL, data->user_data);

	return FALSE;
}

static void connect_destroy(gpointer user_data)
{
	struct chat_data *data = user_data;

	if (data->timeout > 0) {
		g_source_remove(data->timeout);
		data->timeout = 0;
	}

	data->io = NULL;
}

int chat_connect(const char *device, chat_callback_t callback,
						gpointer user_data)
{
	struct chat_data *data;
	struct termios newtio;
	int fd;

	data = g_try_new0(struct chat_data, 1);
	if (!data)
		return -ENOMEM;

	fd = open(device, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		ofono_error("Can't open TTY %s: %s (%d)",
					device, strerror(errno), errno);
		g_free(data);
		return -EIO;
	}

	newtio.c_cflag = B115200 | CRTSCTS | CLOCAL | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;
	newtio.c_lflag = 0;

	newtio.c_cc[VTIME] = 1;
	newtio.c_cc[VMIN] = 5;

	tcflush(fd, TCIFLUSH);

	if (tcsetattr(fd, TCSANOW, &newtio) < 0) {
		ofono_error("Can't change TTY termios: %s (%d)",
						strerror(errno), errno);
		close(fd);
		g_free(data);
		return -EIO;
	}

	data->io = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(data->io, TRUE);

	if (g_io_channel_set_flags(data->io, G_IO_FLAG_NONBLOCK,
						NULL) != G_IO_STATUS_NORMAL) {
		g_io_channel_unref(data->io);
		g_free(data);
		return -EIO;
	}

	data->callback = callback;
	data->user_data = user_data;

	data->timeout = g_timeout_add_seconds(10, connect_timeout, data);

	g_io_add_watch_full(data->io, G_PRIORITY_DEFAULT,
				G_IO_OUT | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
				connect_callback, data, connect_destroy);

	g_io_channel_unref(data->io);

	return 0;
}

void chat_disconnect(GAtChat *chat)
{
}
