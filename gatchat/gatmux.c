/*
 *
 *  AT chat library with GLib integration
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
#include <unistd.h>
#include <string.h>
#include <termios.h>

#include <glib.h>

#include "ringbuffer.h"
#include "gsm0710.h"
#include "gatmux.h"

typedef struct _GAtMuxChannel GAtMuxChannel;
typedef struct _GAtMuxWatch GAtMuxWatch;

struct _GAtMuxChannel
{
	GIOChannel channel;
	GAtMux *mux;
	GIOCondition condition;
	struct ring_buffer *buffer;
};

struct _GAtMuxWatch
{
	GSource source;
	GIOChannel *channel;
	GIOCondition condition;
};

struct _GAtMux {
	gint ref_count;				/* Ref count */
	guint read_watch;			/* GSource read id, 0 if none */
	GIOChannel *channel;			/* channel */
	GAtChat *chat;				/* for muxer setup */
	GAtDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	GAtDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */

	GAtMuxChannel *mux_channel;
	struct gsm0710_context ctx;
};

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							gpointer data)
{
	GAtMux *mux = data;

	if (cond & G_IO_NVAL)
		return FALSE;

	gsm0710_ready_read(&mux->ctx);

	return TRUE;
}

static int do_read(struct gsm0710_context *ctx, void *data, int len)
{
	GAtMux *mux = ctx->user_data;
	GError *error = NULL;
	GIOStatus status;
	gsize bytes_read;

	status = g_io_channel_read_chars(mux->channel, data, len,
						&bytes_read, &error);

	return bytes_read;
}

static int do_write(struct gsm0710_context *ctx, const void *data, int len)
{
	GAtMux *mux = ctx->user_data;
	GError *error = NULL;
	GIOStatus status;
	gssize count = len;
	gsize bytes_written;

	status = g_io_channel_write_chars(mux->channel, (gchar *) data,
						count, &bytes_written, &error);

	return bytes_written;
}

static void do_terminate(struct gsm0710_context *ctx)
{
}

static void deliver_data(struct gsm0710_context *ctx, int channel,
						const void *data, int len)
{
	GAtMux *mux = ctx->user_data;
	GMainContext *context;
	int written;

	written = ring_buffer_write(mux->mux_channel->buffer, data, len);
	if (written < 0)
		return;

	context = g_main_context_default();
	g_main_context_wakeup(context);
}

static void deliver_status(struct gsm0710_context *ctx,
						int channel, int status)
{
	GAtMux *mux = ctx->user_data;
	GMainContext *context;

	if (status & GSM0710_RTS)
		mux->mux_channel->condition |= G_IO_OUT;
	else
		mux->mux_channel->condition &= ~G_IO_OUT;

	context = g_main_context_default();
	g_main_context_wakeup(context);
}

static void open_channel(struct gsm0710_context *ctx, int channel)
{
}

static void close_channel(struct gsm0710_context *ctx, int channel)
{
}

static void debug_message(struct gsm0710_context *ctx, const char *msg)
{
}

GAtMux *g_at_mux_new(GIOChannel *channel)
{
	GAtMux *mux;

	if (!channel)
		return NULL;

	mux = g_try_new0(GAtMux, 1);
	if (!mux)
		return NULL;

	mux->ref_count = 1;

	mux->channel = channel;
	g_io_channel_ref(channel);

	g_io_channel_set_close_on_unref(channel, TRUE);

	gsm0710_initialize(&mux->ctx);
	mux->ctx.user_data = mux;

	mux->ctx.mode = GSM0710_MODE_ADVANCED;

	mux->ctx.read = do_read;
	mux->ctx.write = do_write;
	mux->ctx.terminate = do_terminate;
	mux->ctx.deliver_data = deliver_data;
	mux->ctx.deliver_status = deliver_status;
	mux->ctx.open_channel = open_channel;
	mux->ctx.close_channel = close_channel;
	mux->ctx.debug_message = debug_message;

	return mux;
}

static int open_device(const char *device)
{
	struct termios ti;
	int fd;

	fd = open(device, O_RDWR | O_NOCTTY);
	if (fd < 0)
		return -1;

	tcflush(fd, TCIOFLUSH);

	/* Switch TTY to raw mode */
	memset(&ti, 0, sizeof(ti));
	cfmakeraw(&ti);

	tcsetattr(fd, TCSANOW, &ti);

	return fd;
}

GAtMux *g_at_mux_new_from_tty(const char *device)
{
	GAtMux *mux;
	GIOChannel *channel;
	int fd;

	fd = open_device(device);
	if (fd < 0)
		return NULL;

	channel = g_io_channel_unix_new(fd);
	mux = g_at_mux_new(channel);
	g_io_channel_unref(channel);

	if (!mux) {
		close(fd);
		return NULL;
	}

	return mux;
}

GAtMux *g_at_mux_ref(GAtMux *mux)
{
	if (mux == NULL)
		return NULL;

	g_atomic_int_inc(&mux->ref_count);

	return mux;
}

void g_at_mux_unref(GAtMux *mux)
{
	if (mux == NULL)
		return;

	if (g_atomic_int_dec_and_test(&mux->ref_count)) {
		g_at_mux_shutdown(mux);

		g_io_channel_unref(mux->channel);

		g_free(mux);
	}
}

static gboolean startup_callback(gpointer data)
{
	GAtMux *mux = data;
	GIOFlags flags;

	g_at_chat_shutdown(mux->chat);

	g_at_chat_unref(mux->chat);
	mux->chat = NULL;

	g_io_channel_flush(mux->channel, NULL);

	flags = g_io_channel_get_flags(mux->channel) | G_IO_FLAG_NONBLOCK;
	g_io_channel_set_flags(mux->channel, flags, NULL);

	g_io_channel_set_encoding(mux->channel, NULL, NULL);
	g_io_channel_set_buffered(mux->channel, FALSE);

	mux->read_watch = g_io_add_watch_full(mux->channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
						received_data, mux, NULL);

	gsm0710_startup(&mux->ctx);

	gsm0710_open_channel(&mux->ctx, 1);

	return FALSE;
}

static void setup_callback(gboolean ok, GAtResult *result, gpointer user_data)
{
	GAtMux *mux = user_data;

	if (!ok)
		return;

	g_idle_add(startup_callback, mux);
}

static void chat_disconnect(gpointer user_data)
{
}

gboolean g_at_mux_start(GAtMux *mux)
{
	GAtSyntax *syntax;
	char *cmd;
	int speed;

	if (mux->channel == NULL)
		return FALSE;

	syntax = g_at_syntax_new_gsm_permissive();
	mux->chat = g_at_chat_new(mux->channel, syntax);
	g_at_syntax_unref(syntax);

	if (!mux->chat)
		return FALSE;

	g_at_chat_set_debug(mux->chat, mux->debugf, mux->debug_data);

	g_at_chat_set_disconnect_function(mux->chat, chat_disconnect, NULL);

	g_at_chat_set_wakeup_command(mux->chat, "\r", 1000, 5000);

	g_at_chat_send(mux->chat, "ATE0", NULL, NULL, NULL, NULL);

	//g_at_chat_send(mux->chat, "AT+CFUN=0", NULL, NULL, NULL, NULL);

	switch (mux->ctx.port_speed) {
	case 9600:
		speed = 1;
		break;
	case 19200:
		speed = 2;
		break;
	case 38400:
		speed = 3;
		break;
	case 57600:
		speed = 4;
		break;
	case 115200:
		speed = 5;
		break;
	case 230400:
		speed = 6;
		break;
	default:
		speed = 5;
		break;
	}

	cmd = g_strdup_printf("AT+CMUX=%u,0,%u,%u", mux->ctx.mode, speed,
							mux->ctx.frame_size);

	g_at_chat_send(mux->chat, cmd, NULL, setup_callback, mux, NULL);

	return TRUE;
}

gboolean g_at_mux_shutdown(GAtMux *mux)
{
	if (mux->read_watch > 0)
		g_source_remove(mux->read_watch);

	if (mux->channel == NULL)
		return FALSE;

	gsm0710_shutdown(&mux->ctx);

	return TRUE;
}

gboolean g_at_mux_set_disconnect_function(GAtMux *mux,
			GAtDisconnectFunc disconnect, gpointer user_data)
{
	if (mux == NULL)
		return FALSE;

	mux->user_disconnect = disconnect;
	mux->user_disconnect_data = user_data;

	return TRUE;
}

gboolean g_at_mux_set_debug(GAtMux *mux, GAtDebugFunc func, gpointer user)
{
	if (mux == NULL)
		return FALSE;

	mux->debugf = func;
	mux->debug_data = user;

	return TRUE;
}

static gboolean watch_check(GSource *source)
{
	GAtMuxWatch *watch = (GAtMuxWatch *) source;
	GAtMuxChannel *channel = (GAtMuxChannel *) watch->channel;

	if (ring_buffer_len(channel->buffer) > 0)
		channel->condition |= G_IO_IN;
	else
		channel->condition &= ~G_IO_IN;

	if (channel->condition & watch->condition)
		return TRUE;

	return FALSE;
}

static gboolean watch_prepare(GSource *source, gint *timeout)
{
	*timeout = -1;

	return watch_check(source);
}

static gboolean watch_dispatch(GSource *source, GSourceFunc callback,
							gpointer user_data)
{
	GIOFunc func = (GIOFunc) callback;
	GAtMuxWatch *watch = (GAtMuxWatch *) source;
	GAtMuxChannel *channel = (GAtMuxChannel *) watch->channel;

	if (!func)
		return FALSE;

	return func(watch->channel, channel->condition & watch->condition,
								user_data);
}

static void watch_finalize(GSource *source)
{
	GAtMuxWatch *watch = (GAtMuxWatch *) source;

	g_io_channel_unref(watch->channel);
}

static GSourceFuncs watch_funcs = {
	watch_prepare,
	watch_check,
	watch_dispatch,
	watch_finalize
};

static GIOStatus channel_read(GIOChannel *channel, gchar *buf, gsize count,
					gsize *bytes_read, GError **err)
{
	GAtMuxChannel *mux_channel = (GAtMuxChannel *) channel;
	unsigned int avail = ring_buffer_len_no_wrap(mux_channel->buffer);

	if (avail > count)
		avail = count;

	*bytes_read = ring_buffer_read(mux_channel->buffer, buf, avail);

	return G_IO_STATUS_NORMAL;
}

static GIOStatus channel_write(GIOChannel *channel, const gchar *buf,
				gsize count, gsize *bytes_written, GError **err)
{
	GAtMuxChannel *mux_channel = (GAtMuxChannel *) channel;
	GAtMux *mux = mux_channel->mux;

	gsm0710_write_data(&mux->ctx, 1, buf, count);
	*bytes_written = count;

	return G_IO_STATUS_NORMAL;
}

static GIOStatus channel_seek(GIOChannel *channel, gint64 offset,
						GSeekType type, GError **err)
{
	return G_IO_STATUS_NORMAL;
}

static GIOStatus channel_close(GIOChannel *channel, GError **err)
{
	return G_IO_STATUS_NORMAL;
}

static void channel_free(GIOChannel *channel)
{
	GAtMuxChannel *mux_channel = (GAtMuxChannel *) channel;

	ring_buffer_free(mux_channel->buffer);

	g_free(channel);
}

static GSource *channel_create_watch(GIOChannel *channel,
						GIOCondition condition)
{
	GSource *source;
	GAtMuxWatch *watch;

	source = g_source_new(&watch_funcs, sizeof(GAtMuxWatch));
	watch = (GAtMuxWatch *) source;

	watch->channel = channel;
	g_io_channel_ref(channel);

	watch->condition = condition;

	return source;
}

static GIOStatus channel_set_flags(GIOChannel *channel, GIOFlags flags,
								GError **err)
{
	return G_IO_STATUS_NORMAL;
}

static GIOFlags channel_get_flags(GIOChannel *channel)
{
	GIOFlags flags = 0;

	return flags;
}

static GIOFuncs channel_funcs = {
	channel_read,
	channel_write,
	channel_seek,
	channel_close,
	channel_create_watch,
	channel_free,
	channel_set_flags,
	channel_get_flags,
};

GIOChannel *g_at_mux_create_channel(GAtMux *mux)
{
	GAtMuxChannel *mux_channel;
	GIOChannel *channel;

	mux_channel = g_try_new0(GAtMuxChannel, 1);
	if (mux_channel == NULL)
		return NULL;

	channel = (GIOChannel *) mux_channel;

	g_io_channel_init(channel);
	channel->close_on_unref = TRUE;
	channel->funcs = &channel_funcs;

	channel->is_seekable = FALSE;

	mux_channel->mux = mux;
	mux->mux_channel = mux_channel;

	mux_channel->buffer = ring_buffer_new(GSM0710_BUFFER_SIZE);

	return channel;
}

GAtChat *g_at_mux_create_chat(GAtMux *mux, GAtSyntax *syntax)
{
	GIOChannel *channel;

	g_at_mux_start(mux);

	channel = g_at_mux_create_channel(mux);
	if (channel == NULL)
		return NULL;

	return g_at_chat_new(channel, syntax);
}
