/*
 *
 *  AT chat library with GLib integration
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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>

#include <glib.h>

#include "ringbuffer.h"
#include "gatio.h"
#include "gatutil.h"

struct _GAtIO {
	gint ref_count;				/* Ref count */
	guint read_watch;			/* GSource read id, 0 if no */
	GIOChannel *channel;			/* comms channel */
	GAtDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	struct ring_buffer *buf;		/* Current read buffer */
	guint max_read_attempts;		/* max reads / select */
	GAtIOReadFunc read_handler;		/* Read callback */
	gpointer read_data;			/* Read callback userdata */
	GAtDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	gboolean destroyed;			/* Re-entrancy guard */
};

static void read_watcher_destroy_notify(gpointer user_data)
{
	GAtIO *io = user_data;

	ring_buffer_free(io->buf);
	io->buf = NULL;

	io->channel = NULL;
	io->read_watch = 0;

	if (io->user_disconnect)
		io->user_disconnect(io->user_disconnect_data);

	if (io->destroyed)
		g_free(io);
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	unsigned char *buf;
	GAtIO *io = data;
	GIOError err;
	gsize rbytes;
	gsize toread;
	gsize total_read = 0;
	guint read_count = 0;

	if (cond & G_IO_NVAL)
		return FALSE;

	/* Regardless of condition, try to read all the data available */
	do {
		toread = ring_buffer_avail_no_wrap(io->buf);

		if (toread == 0)
			break;

		rbytes = 0;
		buf = ring_buffer_write_ptr(io->buf, 0);

		err = g_io_channel_read(channel, (char *) buf, toread, &rbytes);
		g_at_util_debug_chat(TRUE, (char *)buf, rbytes,
					io->debugf, io->debug_data);

		read_count++;

		total_read += rbytes;

		if (rbytes > 0)
			ring_buffer_write_advance(io->buf, rbytes);

	} while (err == G_IO_ERROR_NONE && rbytes > 0 &&
					read_count < io->max_read_attempts);

	if (total_read > 0 && io->read_handler)
		io->read_handler(io->buf, io->read_data);

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (read_count > 0 && rbytes == 0 && err != G_IO_ERROR_AGAIN)
		return FALSE;

	/* We're overflowing the buffer, shutdown the socket */
	if (ring_buffer_avail(io->buf) == 0)
		return FALSE;

	return TRUE;
}

static GAtIO *create_io(GIOChannel *channel, GIOFlags flags)
{
	GAtIO *io;

	if (!channel)
		return NULL;

	io = g_try_new0(GAtIO, 1);
	if (!io)
		return io;

	io->ref_count = 1;
	io->debugf = NULL;

	if (flags & G_IO_FLAG_NONBLOCK)
		io->max_read_attempts = 3;
	else
		io->max_read_attempts = 1;

	io->buf = ring_buffer_new(4096);

	if (!io->buf)
		goto error;

	if (!g_at_util_setup_io(channel, flags))
		goto error;

	io->channel = channel;
	io->read_watch = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, io,
				read_watcher_destroy_notify);

	return io;

error:
	if (io->buf)
		ring_buffer_free(io->buf);

	g_free(io);

	return NULL;
}

GAtIO *g_at_io_new(GIOChannel *channel)
{
	return create_io(channel, G_IO_FLAG_NONBLOCK);
}

GAtIO *g_at_io_new_blocking(GIOChannel *channel)
{
	return create_io(channel, 0);
}

GIOChannel *g_at_io_get_channel(GAtIO *io)
{
	if (io == NULL)
		return NULL;

	return io->channel;
}

gboolean g_at_io_set_read_handler(GAtIO *io, GAtIOReadFunc read_handler,
					gpointer user_data)
{
	if (io == NULL)
		return FALSE;

	io->read_handler = read_handler;
	io->read_data = user_data;

	if (read_handler && ring_buffer_len(io->buf) > 0)
		read_handler(io->buf, user_data);

	return TRUE;
}

GAtIO *g_at_io_ref(GAtIO *io)
{
	if (io == NULL)
		return NULL;

	g_atomic_int_inc(&io->ref_count);

	return io;
}

void g_at_io_unref(GAtIO *io)
{
	gboolean is_zero;

	if (io == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&io->ref_count);

	if (is_zero == FALSE)
		return;

	g_at_io_shutdown(io);

	/* glib delays the destruction of the watcher until it exits, this
	 * means we can't free the data just yet, even though we've been
	 * destroyed already.  We have to wait until the read_watcher
	 * destroy function gets called
	 */
	if (io->read_watch != 0)
		io->destroyed = TRUE;
	else
		g_free(io);
}

gboolean g_at_io_shutdown(GAtIO *io)
{
	if (io->channel == NULL)
		return FALSE;

	if (io->read_watch)
		g_source_remove(io->read_watch);

	return TRUE;
}

gboolean g_at_io_set_disconnect_function(GAtIO *io,
			GAtDisconnectFunc disconnect, gpointer user_data)
{
	if (io == NULL)
		return FALSE;

	io->user_disconnect = disconnect;
	io->user_disconnect_data = user_data;

	return TRUE;
}

gboolean g_at_io_set_debug(GAtIO *io, GAtDebugFunc func, gpointer user_data)
{
	if (io == NULL)
		return FALSE;

	io->debugf = func;
	io->debug_data = user_data;

	return TRUE;
}
