/*
 *
 *  RIL chat library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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
#include "grilio.h"
#include "grilutil.h"

struct _GRilIO {
	gint ref_count;				/* Ref count */
	guint read_watch;			/* GSource read id, 0 if no */
	guint write_watch;			/* GSource write id, 0 if no */
	GIOChannel *channel;			/* comms channel */
	GRilDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	struct ring_buffer *buf;		/* Current read buffer */
	guint max_read_attempts;		/* max reads / select */
	GRilIOReadFunc read_handler;		/* Read callback */
	gpointer read_data;			/* Read callback userdata */
	gboolean use_write_watch;		/* Use write select */
	GRilIOWriteFunc write_handler;		/* Write callback */
	gpointer write_data;			/* Write callback userdata */
	GRilDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	GRilDisconnectFunc write_done_func;	/* tx empty notifier */
	gpointer write_done_data;		/* tx empty data */
	gboolean destroyed;			/* Re-entrancy guard */
};

static void read_watcher_destroy_notify(gpointer user_data)
{
	GRilIO *io = user_data;

	ring_buffer_free(io->buf);
	io->buf = NULL;

	io->debugf = NULL;
	io->debug_data = NULL;

	io->read_watch = 0;
	io->read_handler = NULL;
	io->read_data = NULL;

	io->channel = NULL;

	if (io->destroyed)
		g_free(io);
	else if (io->user_disconnect)
		io->user_disconnect(io->user_disconnect_data);
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	unsigned char *buf;
	GRilIO *io = data;
	GIOStatus status;
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

		status = g_io_channel_read_chars(channel, (char *) buf,
							toread, &rbytes, NULL);

		g_ril_util_debug_hexdump(TRUE, (guchar *) buf, rbytes,
						io->debugf, io->debug_data);

		read_count++;

		total_read += rbytes;

		if (rbytes > 0)
			ring_buffer_write_advance(io->buf, rbytes);

	} while (status == G_IO_STATUS_NORMAL && rbytes > 0 &&
					read_count < io->max_read_attempts);

	if (total_read > 0 && io->read_handler)
		io->read_handler(io->buf, io->read_data);

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (read_count > 0 && rbytes == 0 && status != G_IO_STATUS_AGAIN)
		return FALSE;

	/* We're overflowing the buffer, shutdown the socket */
	if (ring_buffer_avail(io->buf) == 0)
		return FALSE;

	return TRUE;
}

gsize g_ril_io_write(GRilIO *io, const gchar *data, gsize count)
{
	GIOStatus status;
	gsize bytes_written;

	status = g_io_channel_write_chars(io->channel, data,
						count, &bytes_written, NULL);

	if (status != G_IO_STATUS_NORMAL) {
		g_source_remove(io->read_watch);
		return 0;
	}

	g_ril_util_debug_hexdump(FALSE, (guchar *) data, bytes_written,
				io->debugf, io->debug_data);

	return bytes_written;
}

static void write_watcher_destroy_notify(gpointer user_data)
{
	GRilIO *io = user_data;

	io->write_watch = 0;
	io->write_handler = NULL;
	io->write_data = NULL;

	if (io->write_done_func) {
		io->write_done_func(io->write_done_data);
		io->write_done_func = NULL;
		io->write_done_data = NULL;
	}
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GRilIO *io = data;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (io->write_handler == NULL)
		return FALSE;

	return io->write_handler(io->write_data);
}

static GRilIO *create_io(GIOChannel *channel, GIOFlags flags)
{
	GRilIO *io;

	if (channel == NULL)
		return NULL;

	io = g_try_new0(GRilIO, 1);
	if (io == NULL)
		return io;

	io->ref_count = 1;
	io->debugf = NULL;

	if (flags & G_IO_FLAG_NONBLOCK) {
		io->max_read_attempts = 3;
		io->use_write_watch = TRUE;
	} else {
		io->max_read_attempts = 1;
		io->use_write_watch = FALSE;
	}

	io->buf = ring_buffer_new(8192);

	if (!io->buf)
		goto error;

	if (!g_ril_util_setup_io(channel, flags))
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

GRilIO *g_ril_io_new(GIOChannel *channel)
{
	return create_io(channel, G_IO_FLAG_NONBLOCK);
}

GRilIO *g_ril_io_new_blocking(GIOChannel *channel)
{
	return create_io(channel, 0);
}

GIOChannel *g_ril_io_get_channel(GRilIO *io)
{
	if (io == NULL)
		return NULL;

	return io->channel;
}

gboolean g_ril_io_set_read_handler(GRilIO *io, GRilIOReadFunc read_handler,
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

static gboolean call_blocking_read(gpointer user_data)
{
	GRilIO *io = user_data;

	while (can_write_data(io->channel, G_IO_OUT, io) == TRUE);
	write_watcher_destroy_notify(io);

	return FALSE;
}

gboolean g_ril_io_set_write_handler(GRilIO *io, GRilIOWriteFunc write_handler,
					gpointer user_data)
{
	if (io == NULL)
		return FALSE;

	if (write_handler == NULL)
		return FALSE;

	io->write_handler = write_handler;
	io->write_data = user_data;

	if (io->use_write_watch == TRUE)
		io->write_watch = g_io_add_watch_full(io->channel,
				G_PRIORITY_HIGH,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, io,
				write_watcher_destroy_notify);
	else
		io->write_watch = g_idle_add(call_blocking_read, io);

	return TRUE;
}

GRilIO *g_ril_io_ref(GRilIO *io)
{
	if (io == NULL)
		return NULL;

	g_atomic_int_inc(&io->ref_count);

	return io;
}

static gboolean io_shutdown(GRilIO *io)
{
	/* Don't trigger user disconnect on shutdown */
	io->user_disconnect = NULL;
	io->user_disconnect_data = NULL;

	if (io->read_watch > 0)
		g_source_remove(io->read_watch);

	if (io->write_watch > 0)
		g_source_remove(io->write_watch);

	return TRUE;
}

void g_ril_io_unref(GRilIO *io)
{
	gboolean is_zero;

	if (io == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&io->ref_count);

	if (is_zero == FALSE)
		return;

	io_shutdown(io);

	/* glib delays the destruction of the watcher until it exits, this
	 * means we can't free the data just yet, even though we've been
	 * destroyed already.  We have to wait until the read_watcher
	 * destroy function gets called
	 */
	if (io->read_watch > 0)
		io->destroyed = TRUE;
	else
		g_free(io);
}

gboolean g_ril_io_set_disconnect_function(GRilIO *io,
			GRilDisconnectFunc disconnect, gpointer user_data)
{
	if (io == NULL)
		return FALSE;

	io->user_disconnect = disconnect;
	io->user_disconnect_data = user_data;

	return TRUE;
}

gboolean g_ril_io_set_debug(GRilIO *io, GRilDebugFunc func, gpointer user_data)
{
	if (io == NULL)
		return FALSE;

	io->debugf = func;
	io->debug_data = user_data;

	return TRUE;
}

void g_ril_io_set_write_done(GRilIO *io, GRilDisconnectFunc func,
				gpointer user_data)
{
	if (io == NULL)
		return;

	io->write_done_func = func;
	io->write_done_data = user_data;
}

void g_ril_io_drain_ring_buffer(GRilIO *io, guint len)
{
	ring_buffer_drain(io->buf, len);
}
