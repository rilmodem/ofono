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

/* #define DBG(fmt, arg...) g_print("%s: " fmt "\n" , __func__ , ## arg) */
#define DBG(fmt, arg...)

static const char *cmux_prefix[] = { "+CMUX:", NULL };
static const char *none_prefix[] = { NULL };

typedef struct _GAtMuxChannel GAtMuxChannel;
typedef struct _GAtMuxWatch GAtMuxWatch;

#define MAX_CHANNELS 63
#define BITMAP_SIZE 8

struct _GAtMuxChannel
{
	GIOChannel channel;
	GAtMux *mux;
	GIOCondition condition;
	struct ring_buffer *buffer;
	GSList *sources;
	gboolean throttled;
	guint dlc;
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
	guint write_watch;			/* GSource write id, 0 if none */
	GIOChannel *channel;			/* main serial channel */
	GAtDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	GAtDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	GAtMuxChannel *dlcs[MAX_CHANNELS];	/* DLCs opened by the MUX */
	guint8 newdata[BITMAP_SIZE];		/* Channels that got new data */
	struct gsm0710_context ctx;
};

struct mux_setup_data {
	GAtChat *chat;
	GAtMuxSetupFunc func;
	gpointer user;
	GDestroyNotify destroy;
	guint mode;
	guint frame_size;
};

static void dispatch_sources(GAtMuxChannel *channel, GIOCondition condition)
{
	GAtMuxWatch *source;
	GSList *c;
	GSList *p;
	GSList *t;

	p = NULL;
	c = channel->sources;

	while (c) {
		gboolean destroy = FALSE;

		source = c->data;

		DBG("Checking source: %p", source);

		if (condition & source->condition) {
			gpointer user_data = NULL;
			GSourceFunc callback = NULL;
			GSourceCallbackFuncs *cb_funcs;
			gpointer cb_data;
			gboolean (*dispatch) (GSource *, GSourceFunc, gpointer);

			DBG("dispatching source: %p", source);

			dispatch = source->source.source_funcs->dispatch;
			cb_funcs = source->source.callback_funcs;
			cb_data = source->source.callback_data;

			if (cb_funcs)
				cb_funcs->ref(cb_data);

			if (cb_funcs)
				cb_funcs->get(cb_data, (GSource *) source,
						&callback, &user_data);

			destroy = !dispatch((GSource *) source, callback,
						user_data);

			if (cb_funcs)
				cb_funcs->unref(cb_data);
		}

		if (destroy) {
			DBG("removing source: %p", source);

			g_source_destroy((GSource *) source);

			if (p)
				p->next = c->next;
			else
				channel->sources = c->next;

			t = c;
			c = c->next;
			g_slist_free_1(t);
		} else {
			p = c;
			c = c->next;
		}
	}
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							gpointer data)
{
	GAtMux *mux = data;
	int i;

	if (cond & G_IO_NVAL)
		return FALSE;

	DBG("received data");

	memset(mux->newdata, 0, BITMAP_SIZE);
	gsm0710_ready_read(&mux->ctx);

	for (i = 1; i <= MAX_CHANNELS; i++) {
		int offset = i / 8;
		int bit = i % 8;

		if (!(mux->newdata[offset] & (1 << bit)))
			continue;

		DBG("dispatching sources for channel: %p", mux->dlcs[i-1]);

		dispatch_sources(mux->dlcs[i-1], G_IO_IN);
	}

	return TRUE;
}

static void write_watcher_destroy_notify(GAtMux *mux)
{
	mux->write_watch = 0;
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GAtMux *mux = data;
	int dlc;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	DBG("Can write data");

	for (dlc = 0; dlc < MAX_CHANNELS; dlc += 1) {
		GAtMuxChannel *channel = mux->dlcs[dlc];

		if (channel == NULL)
			continue;

		DBG("Checking channel for write: %p", channel);

		if (channel->throttled)
			continue;

		DBG("Dispatching write sources: %p", channel);

		dispatch_sources(channel, G_IO_OUT);
	}

	for (dlc = 0; dlc < MAX_CHANNELS; dlc += 1) {
		GAtMuxChannel *channel = mux->dlcs[dlc];
		GSList *l;
		GAtMuxWatch *source;

		if (channel == NULL)
			continue;

		if (channel->throttled)
			continue;

		for (l = channel->sources; l; l = l->next) {
			source = l->data;

			if (source->condition & G_IO_OUT)
				return TRUE;
		}
	}

	return FALSE;
}

static void wakeup_writer(GAtMux *mux)
{
	if (mux->write_watch != 0)
		return;

	DBG("Waking up writer");

	mux->write_watch = g_io_add_watch_full(mux->channel,
				G_PRIORITY_DEFAULT,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, mux,
				(GDestroyNotify)write_watcher_destroy_notify);
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

static void deliver_data(struct gsm0710_context *ctx, int dlc,
						const void *data, int len)
{
	GAtMux *mux = ctx->user_data;
	GAtMuxChannel *channel = mux->dlcs[dlc-1];
	int written;
	int offset;
	int bit;

	DBG("deliver_data: dlc: %d, channel: %p", dlc, channel);

	if (channel == NULL)
		return;

	written = ring_buffer_write(channel->buffer, data, len);

	if (written < 0)
		return;

	offset = dlc / 8;
	bit = dlc % 8;

	mux->newdata[offset] |= 1 << bit;
	channel->condition |= G_IO_IN;
}

static void deliver_status(struct gsm0710_context *ctx,
						int channel, int status)
{
	GAtMux *mux = ctx->user_data;

	DBG("Got status %d, for channel %d", status, channel);

	if (status & GSM0710_RTS) {
		GSList *l;

		mux->dlcs[channel-1]->throttled = FALSE;
		DBG("setting throttled to FALSE");

		for (l = mux->dlcs[channel-1]->sources; l; l = l->next) {
			GAtMuxWatch *source = l->data;

			if (source->condition & G_IO_OUT) {
				wakeup_writer(mux);
				break;
			}
		}
	} else
		mux->dlcs[channel-1]->throttled = TRUE;
}

static void debug_message(struct gsm0710_context *ctx, const char *msg)
{
}

static gboolean watch_check(GSource *source)
{
	return FALSE;
}

static gboolean watch_prepare(GSource *source, gint *timeout)
{
	*timeout = -1;
	return FALSE;
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

	gsm0710_write_data(&mux->ctx, mux_channel->dlc, buf, count);
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
	GAtMuxChannel *mux_channel = (GAtMuxChannel *) channel;
	GAtMux *mux = mux_channel->mux;

	DBG("closing channel: %d", mux_channel->dlc);

	dispatch_sources(mux_channel, G_IO_NVAL);

	gsm0710_close_channel(&mux->ctx, mux_channel->dlc);
	mux->dlcs[mux_channel->dlc - 1] = NULL;

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
	GAtMuxChannel *dlc = (GAtMuxChannel *) channel;
	GAtMux *mux = dlc->mux;

	source = g_source_new(&watch_funcs, sizeof(GAtMuxWatch));
	watch = (GAtMuxWatch *) source;

	watch->channel = channel;
	g_io_channel_ref(channel);

	watch->condition = condition;

	if ((watch->condition & G_IO_OUT) && dlc->throttled == FALSE)
		wakeup_writer(mux);

	DBG("Creating source: %p for channel: %p, writer: %d, reader: %d",
			watch, channel,
			condition & G_IO_OUT,
			condition & G_IO_IN);

	dlc->sources = g_slist_prepend(dlc->sources, watch);

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

static GAtMux *mux_new_gsm0710_common(GIOChannel *channel,
					int mode, int frame_size)
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

	mux->ctx.read = do_read;
	mux->ctx.write = do_write;
	mux->ctx.deliver_data = deliver_data;
	mux->ctx.deliver_status = deliver_status;
	mux->ctx.debug_message = debug_message;

	mux->ctx.mode = mode;
	mux->ctx.frame_size = frame_size;

	return mux;
}

GAtMux *g_at_mux_new_gsm0710_basic(GIOChannel *channel, int frame_size)
{
	return mux_new_gsm0710_common(channel, GSM0710_MODE_BASIC, frame_size);
}

GAtMux *g_at_mux_new_gsm0710_advanced(GIOChannel *channel, int frame_size)
{
	return mux_new_gsm0710_common(channel, GSM0710_MODE_ADVANCED,
					frame_size);
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

gboolean g_at_mux_start(GAtMux *mux)
{
	if (mux->channel == NULL)
		return FALSE;

	mux->read_watch = g_io_add_watch_full(mux->channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
						received_data, mux, NULL);

	gsm0710_startup(&mux->ctx);

	return TRUE;
}

gboolean g_at_mux_shutdown(GAtMux *mux)
{
	int i;

	if (mux->read_watch > 0)
		g_source_remove(mux->read_watch);

	for (i = 0; i < MAX_CHANNELS; i++) {
		if (mux->dlcs[i] == NULL)
			continue;

		channel_close((GIOChannel *) mux->dlcs[i], NULL);
	}

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

GIOChannel *g_at_mux_create_channel(GAtMux *mux)
{
	GAtMuxChannel *mux_channel;
	GIOChannel *channel;
	int i;

	for (i = 0; i < MAX_CHANNELS; i++) {
		if (mux->dlcs[i] == NULL)
			break;
	}

	if (i == MAX_CHANNELS)
		return NULL;

	mux_channel = g_try_new0(GAtMuxChannel, 1);
	if (mux_channel == NULL)
		return NULL;

	gsm0710_open_channel(&mux->ctx, i+1);

	channel = (GIOChannel *) mux_channel;

	g_io_channel_init(channel);
	channel->close_on_unref = TRUE;
	channel->funcs = &channel_funcs;

	channel->is_seekable = FALSE;

	mux_channel->mux = mux;
	mux_channel->dlc = i+1;
	mux_channel->buffer = ring_buffer_new(GSM0710_BUFFER_SIZE);
	mux_channel->throttled = FALSE;

	mux->dlcs[i] = mux_channel;

	DBG("Created channel %p, dlc: %d", channel, i+1);

	return channel;
}

static void mux_setup_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct mux_setup_data *msd = user_data;
	GIOFlags flags;
	GIOChannel *channel;
	GAtMux *mux = NULL;

	if (!ok)
		goto error;

	channel = g_at_chat_get_channel(msd->chat);
	channel = g_io_channel_ref(channel);

	g_at_chat_shutdown(msd->chat);
	g_at_chat_unref(msd->chat);

	flags = g_io_channel_get_flags(channel) | G_IO_FLAG_NONBLOCK;
	g_io_channel_set_flags(channel, flags, NULL);

	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	if (msd->mode == 0)
		mux = g_at_mux_new_gsm0710_basic(channel, msd->frame_size);
	else
		mux = g_at_mux_new_gsm0710_advanced(channel, msd->frame_size);

	g_io_channel_unref(channel);

error:
	msd->func(mux, msd->user);

	if (msd->destroy)
		msd->destroy(msd->user);
}

static void mux_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct mux_setup_data *msd = user_data;
	struct mux_setup_data *nmsd;
	GAtResultIter iter;
	int min, max;
	int speed;
	char buf[64];

	/* CMUX query not supported, abort */
	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CMUX:"))
		goto error;

	/* Mode */
	if (!g_at_result_iter_open_list(&iter))
		goto error;

	if (!g_at_result_iter_next_range(&iter, &min, &max))
		goto error;

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	if (min <= 1 && 1 <= max)
		msd->mode = 1;
	else if (min <= 0 && 0 <= max)
		msd->mode = 0;
	else
		goto error;

	/* Subset */
	if (!g_at_result_iter_open_list(&iter))
		goto error;

	if (!g_at_result_iter_next_range(&iter, &min, &max))
		goto error;

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	if (min > 0)
		goto error;

	/* Speed, pick highest */
	if (!g_at_result_iter_open_list(&iter))
		goto error;

	if (!g_at_result_iter_next_range(&iter, &min, &max))
		goto error;

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	speed = max;

	/* Frame size, pick defaults */
	if (!g_at_result_iter_open_list(&iter))
		goto error;

	if (!g_at_result_iter_next_range(&iter, &min, &max))
		goto error;

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	if (msd->mode == 0) {
		if (min > 31 || max < 31)
			goto error;

		msd->frame_size = 31;
	} else if (msd->mode == 1) {
		if (min > 64 || max < 64)
			goto error;

		msd->frame_size = 64;
	} else
		goto error;

	nmsd = g_memdup(msd, sizeof(struct mux_setup_data));

	sprintf(buf, "AT+CMUX=%u,0,%u,%u", msd->mode, speed, msd->frame_size);

	if (g_at_chat_send(msd->chat, buf, none_prefix,
				mux_setup_cb, nmsd, g_free) > 0)
		return;

	g_free(nmsd);

error:
	msd->func(NULL, msd->user);

	if (msd->destroy)
		msd->destroy(msd->user);
}

gboolean g_at_mux_setup_gsm0710(GAtChat *chat,
				GAtMuxSetupFunc notify, gpointer user_data,
				GDestroyNotify destroy)
{
	struct mux_setup_data *msd;

	if (chat == NULL)
		return FALSE;

	if (notify == NULL)
		return FALSE;

	msd = g_new0(struct mux_setup_data, 1);

	msd->chat = g_at_chat_ref(chat);
	msd->func = notify;
	msd->user = user_data;
	msd->destroy = destroy;

	if (g_at_chat_send(chat, "AT+CMUX=?", cmux_prefix,
				mux_query_cb, msd, g_free) > 0)
		return TRUE;

	if (msd)
		g_free(msd);

	return FALSE;
}
