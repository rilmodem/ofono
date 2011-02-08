/*
 *
 *  AT chat library with GLib integration
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009  Trolltech ASA.
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
#include <alloca.h>

#include <glib.h>

#include "ringbuffer.h"
#include "gatmux.h"
#include "gsm0710.h"

static const char *cmux_prefix[] = { "+CMUX:", NULL };
static const char *none_prefix[] = { NULL };

typedef struct _GAtMuxChannel GAtMuxChannel;
typedef struct _GAtMuxWatch GAtMuxWatch;
typedef void (*GAtMuxWriteFrame)(GAtMux *mux, guint8 dlc, guint8 control,
				const guint8 *data, int len);

/* While 63 channels are theoretically possible, channel 62 and 63 is reserved
 * by 27.010 for use as the beginning of frame and end of frame flags.
 * Refer to Section 5.6 in 27.007
 */
#define MAX_CHANNELS 61
#define BITMAP_SIZE 8
#define MUX_CHANNEL_BUFFER_SIZE 4096
#define MUX_BUFFER_SIZE 4096

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
	const GAtMuxDriver *driver;		/* Driver functions */
	void *driver_data;			/* Driver data */
	char buf[MUX_BUFFER_SIZE];		/* Buffer on the main mux */
	int buf_used;				/* Bytes of buf being used */
	gboolean shutdown;
};

struct mux_setup_data {
	GAtChat *chat;
	GAtMuxSetupFunc func;
	gpointer user;
	GDestroyNotify destroy;
	guint mode;
	guint frame_size;
};

static inline void debug(GAtMux *mux, const char *format, ...)
{
	char str[256];
	va_list ap;

	if (mux->debugf == NULL)
		return;

	va_start(ap, format);

	if (vsnprintf(str, sizeof(str), format, ap) > 0)
		mux->debugf(str, mux->debug_data);

	va_end(ap);
}

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

		debug(channel->mux, "checking source: %p", source);

		if (condition & source->condition) {
			gpointer user_data = NULL;
			GSourceFunc callback = NULL;
			GSourceCallbackFuncs *cb_funcs;
			gpointer cb_data;
			gboolean (*dispatch) (GSource *, GSourceFunc, gpointer);

			debug(channel->mux, "dispatching source: %p", source);

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
			debug(channel->mux, "removing source: %p", source);

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
	GIOStatus status;
	gsize bytes_read;

	if (cond & G_IO_NVAL)
		return FALSE;

	debug(mux, "received data");

	bytes_read = 0;
	status = g_io_channel_read_chars(mux->channel, mux->buf + mux->buf_used,
					sizeof(mux->buf) - mux->buf_used,
					&bytes_read, NULL);

	mux->buf_used += bytes_read;

	if (bytes_read > 0 && mux->driver->feed_data) {
		int nread;

		memset(mux->newdata, 0, BITMAP_SIZE);

		nread = mux->driver->feed_data(mux, mux->buf, mux->buf_used);
		mux->buf_used -= nread;

		if (mux->buf_used > 0)
			memmove(mux->buf, mux->buf + nread, mux->buf_used);

		for (i = 1; i <= MAX_CHANNELS; i++) {
			int offset = i / 8;
			int bit = i % 8;

			if (!(mux->newdata[offset] & (1 << bit)))
				continue;

			debug(mux, "dispatching sources for channel: %p",
				mux->dlcs[i-1]);

			dispatch_sources(mux->dlcs[i-1], G_IO_IN);
		}
	}

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (status != G_IO_STATUS_NORMAL && status != G_IO_STATUS_AGAIN)
		return FALSE;

	if (mux->buf_used == sizeof(mux->buf))
		return FALSE;

	return TRUE;
}

static void write_watcher_destroy_notify(gpointer user_data)
{
	GAtMux *mux = user_data;

	mux->write_watch = 0;
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GAtMux *mux = data;
	int dlc;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	debug(mux, "can write data");

	for (dlc = 0; dlc < MAX_CHANNELS; dlc += 1) {
		GAtMuxChannel *channel = mux->dlcs[dlc];

		if (channel == NULL)
			continue;

		debug(mux, "checking channel for write: %p", channel);

		if (channel->throttled)
			continue;

		debug(mux, "dispatching write sources: %p", channel);

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

	debug(mux, "waking up writer");

	mux->write_watch = g_io_add_watch_full(mux->channel,
				G_PRIORITY_DEFAULT,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, mux,
				write_watcher_destroy_notify);
}

int g_at_mux_raw_write(GAtMux *mux, const void *data, int towrite)
{
	gssize count = towrite;
	gsize bytes_written;

	g_io_channel_write_chars(mux->channel, (gchar *) data,
					count, &bytes_written, NULL);

	return bytes_written;
}

void g_at_mux_feed_dlc_data(GAtMux *mux, guint8 dlc,
				const void *data, int tofeed)
{
	GAtMuxChannel *channel;

	int written;
	int offset;
	int bit;

	debug(mux, "deliver_data: dlc: %hu", dlc);

	if (dlc < 1 || dlc > MAX_CHANNELS)
		return;

	channel = mux->dlcs[dlc-1];

	if (channel == NULL)
		return;

	written = ring_buffer_write(channel->buffer, data, tofeed);

	if (written < 0)
		return;

	offset = dlc / 8;
	bit = dlc % 8;

	mux->newdata[offset] |= 1 << bit;
	channel->condition |= G_IO_IN;
}

void g_at_mux_set_dlc_status(GAtMux *mux, guint8 dlc, int status)
{
	GAtMuxChannel *channel;

	debug(mux, "got status %d, for channel %hu", status, dlc);

	if (dlc < 1 || dlc > MAX_CHANNELS)
		return;

	channel = mux->dlcs[dlc-1];
	if (channel == NULL)
		return;

	if (status & G_AT_MUX_DLC_STATUS_RTR) {
		GSList *l;

		mux->dlcs[dlc-1]->throttled = FALSE;
		debug(mux, "setting throttled to FALSE");

		for (l = mux->dlcs[dlc-1]->sources; l; l = l->next) {
			GAtMuxWatch *source = l->data;

			if (source->condition & G_IO_OUT) {
				wakeup_writer(mux);
				break;
			}
		}
	} else
		mux->dlcs[dlc-1]->throttled = TRUE;
}

void g_at_mux_set_data(GAtMux *mux, void *data)
{
	if (mux == NULL)
		return;

	mux->driver_data = data;
}

void *g_at_mux_get_data(GAtMux *mux)
{
	if (mux == NULL)
		return NULL;

	return mux->driver_data;
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

	if (func == NULL)
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

	if (*bytes_read == 0)
		return G_IO_STATUS_AGAIN;

	return G_IO_STATUS_NORMAL;
}

static GIOStatus channel_write(GIOChannel *channel, const gchar *buf,
				gsize count, gsize *bytes_written, GError **err)
{
	GAtMuxChannel *mux_channel = (GAtMuxChannel *) channel;
	GAtMux *mux = mux_channel->mux;

	if (mux->driver->write)
		mux->driver->write(mux, mux_channel->dlc, buf, count);
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

	debug(mux, "closing channel: %d", mux_channel->dlc);

	dispatch_sources(mux_channel, G_IO_NVAL);

	if (mux->driver->close_dlc)
		mux->driver->close_dlc(mux, mux_channel->dlc);

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

	debug(mux, "creating source: %p, channel: %p, writer: %d, reader: %d",
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

GAtMux *g_at_mux_new(GIOChannel *channel, const GAtMuxDriver *driver)
{
	GAtMux *mux;

	if (channel == NULL)
		return NULL;

	mux = g_try_new0(GAtMux, 1);
	if (mux == NULL)
		return NULL;

	mux->ref_count = 1;
	mux->driver = driver;
	mux->shutdown = TRUE;

	mux->channel = channel;
	g_io_channel_ref(channel);

	g_io_channel_set_close_on_unref(channel, TRUE);

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

		if (mux->driver->remove)
			mux->driver->remove(mux);

		g_free(mux);
	}
}

gboolean g_at_mux_start(GAtMux *mux)
{
	if (mux->channel == NULL)
		return FALSE;

	if (mux->driver->startup == NULL)
		return FALSE;

	if (mux->driver->startup(mux) == FALSE)
		return FALSE;

	mux->read_watch = g_io_add_watch_full(mux->channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
						received_data, mux, NULL);

	mux->shutdown = FALSE;

	return TRUE;
}

gboolean g_at_mux_shutdown(GAtMux *mux)
{
	int i;

	if (mux->shutdown == TRUE)
		return FALSE;

	if (mux->channel == NULL)
		return FALSE;

	if (mux->read_watch > 0)
		g_source_remove(mux->read_watch);

	for (i = 0; i < MAX_CHANNELS; i++) {
		if (mux->dlcs[i] == NULL)
			continue;

		channel_close((GIOChannel *) mux->dlcs[i], NULL);
	}

	if (mux->driver->shutdown)
		mux->driver->shutdown(mux);

	mux->shutdown = TRUE;

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

gboolean g_at_mux_set_debug(GAtMux *mux, GAtDebugFunc func, gpointer user_data)
{
	if (mux == NULL)
		return FALSE;

	mux->debugf = func;
	mux->debug_data = user_data;

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

	if (mux->driver->open_dlc)
		mux->driver->open_dlc(mux, i+1);

	channel = (GIOChannel *) mux_channel;

	g_io_channel_init(channel);
	channel->close_on_unref = TRUE;
	channel->funcs = &channel_funcs;

	channel->is_seekable = FALSE;
	channel->is_readable = TRUE;
	channel->is_writeable = TRUE;

	channel->do_encode = FALSE;

	mux_channel->mux = mux;
	mux_channel->dlc = i+1;
	mux_channel->buffer = ring_buffer_new(MUX_CHANNEL_BUFFER_SIZE);
	mux_channel->throttled = FALSE;

	mux->dlcs[i] = mux_channel;

	debug(mux, "created channel %p, dlc: %d", channel, i+1);

	return channel;
}

static void msd_free(gpointer user_data)
{
	struct mux_setup_data *msd = user_data;

	if (msd->chat)
		g_at_chat_unref(msd->chat);

	g_free(msd);
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

	g_at_chat_unref(msd->chat);
	msd->chat = NULL;

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
	if (g_at_result_iter_open_list(&iter)) {
		if (!g_at_result_iter_next_range(&iter, &min, &max))
			goto error;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		speed = max;
	} else {
		if (!g_at_result_iter_skip_next(&iter))
			goto error;

		/* not available/used */
		speed = -1;
	}

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
	g_at_chat_ref(nmsd->chat);

	if (speed < 0)
		sprintf(buf, "AT+CMUX=%u,0,,%u", msd->mode, msd->frame_size);
	else
		sprintf(buf, "AT+CMUX=%u,0,%u,%u", msd->mode, speed,
							msd->frame_size);

	if (g_at_chat_send(msd->chat, buf, none_prefix,
				mux_setup_cb, nmsd, msd_free) > 0)
		return;

	msd_free(nmsd);

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
				mux_query_cb, msd, msd_free) > 0)
		return TRUE;

	if (msd)
		msd_free(msd);

	return FALSE;
}

#define GSM0710_BUFFER_SIZE 4096

struct gsm0710_data {
	int frame_size;
};

/* Process an incoming GSM 07.10 packet */
static gboolean gsm0710_packet(GAtMux *mux, int dlc, guint8 control,
				const unsigned char *data, int len,
				GAtMuxWriteFrame write_frame)
{
	if (control == 0xEF || control == 0x03) {
		if (dlc >= 1 && dlc <= 63) {
			g_at_mux_feed_dlc_data(mux, dlc, data, len);
			return TRUE;
		}

		if (dlc == 0) {
			/* An embedded command or response on channel 0 */
			if (len >= 2 && data[0] == GSM0710_STATUS_SET) {
				return gsm0710_packet(mux, dlc,
							GSM0710_STATUS_ACK,
							data + 2, len - 2,
							write_frame);
			} else if (len >= 2 && data[0] == 0x43) {
				/* Test command from other side - send the same bytes back */
				unsigned char *resp = alloca(len);
				memcpy(resp, data, len);
				resp[0] = 0x41;	/* Clear the C/R bit in the response */
				write_frame(mux, 0, GSM0710_DATA, resp, len);
			}
		}
	} else if (control == GSM0710_STATUS_ACK && dlc == 0) {
		unsigned char resp[33];

		/* Status change message */
		if (len >= 2) {
			/* Handle status changes on other channels */
			dlc = ((data[0] & 0xFC) >> 2);

			if (dlc >= 1 && dlc <= 63)
				g_at_mux_set_dlc_status(mux, dlc, data[1]);
		}

		/* Send the response to the status change request to ACK it */
		debug(mux, "received status line signal, sending response");
		if (len > 31)
			len = 31;
		resp[0] = GSM0710_STATUS_ACK;
		resp[1] = ((len << 1) | 0x01);
		memcpy(resp + 2, data, len);
		write_frame(mux, 0, GSM0710_DATA, resp, len + 2);
	}

	return TRUE;
}

static void gsm0710_basic_write_frame(GAtMux *mux, guint8 dlc, guint8 control,
					const guint8 *data, int towrite)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);
	guint8 *frame = alloca(gd->frame_size + 7);
	int frame_size;

	frame_size = gsm0710_basic_fill_frame(frame, dlc, control,
						data, towrite);
	g_at_mux_raw_write(mux, frame, frame_size);
}

#define COMPOSE_STATUS_FRAME(data, dlc, status)	\
	guint8 data[4];				\
	data[0] = GSM0710_STATUS_SET;		\
	data[1] = 0x03;				\
	data[2] = ((dlc << 2) | 0x03);		\
	data[3] = status

static void gsm0710_basic_remove(GAtMux *mux)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);

	g_free(gd);
	g_at_mux_set_data(mux, NULL);
}

static gboolean gsm0710_basic_startup(GAtMux *mux)
{
	guint8 frame[6];
	int frame_size;

	frame_size = gsm0710_basic_fill_frame(frame, 0, GSM0710_OPEN_CHANNEL,
						NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static gboolean gsm0710_basic_shutdown(GAtMux *mux)
{
	guint8 frame[6];
	int frame_size;

	frame_size = gsm0710_basic_fill_frame(frame, 0, GSM0710_CLOSE_CHANNEL,
						NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static gboolean gsm0710_basic_open_dlc(GAtMux *mux, guint8 dlc)
{
	guint8 frame[6];
	int frame_size;

	frame_size = gsm0710_basic_fill_frame(frame, dlc, GSM0710_OPEN_CHANNEL,
						NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static gboolean gsm0710_basic_close_dlc(GAtMux *mux, guint8 dlc)
{
	guint8 frame[6];
	int frame_size;

	frame_size = gsm0710_basic_fill_frame(frame, dlc, GSM0710_CLOSE_CHANNEL,
						NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static int gsm0710_basic_feed_data(GAtMux *mux, void *data, int len)
{
	int total = 0;
	int nread;
	guint8 dlc;
	guint8 ctrl;
	guint8 *frame;
	int frame_len;

	do {
		frame = NULL;
		nread = gsm0710_basic_extract_frame(data, len, &dlc, &ctrl,
							&frame, &frame_len);

		total += nread;
		data += nread;
		len -= nread;

		if (frame == NULL)
			break;

		gsm0710_packet(mux, dlc, ctrl, frame, frame_len,
				gsm0710_basic_write_frame);
	} while (nread > 0);

	return total;
}

static void gsm0710_basic_set_status(GAtMux *mux, guint8 dlc, guint8 status)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);
	guint8 *frame = alloca(gd->frame_size + 7);
	int frame_size;

	COMPOSE_STATUS_FRAME(data, dlc, status);
	frame_size = gsm0710_basic_fill_frame(frame, 0, GSM0710_DATA, data, 4);
	g_at_mux_raw_write(mux, frame, frame_size);
}

static void gsm0710_basic_write(GAtMux *mux, guint8 dlc,
				const void *data, int towrite)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);
	guint8 *frame = alloca(gd->frame_size + 7);
	int max;
	int frame_size;

	while (towrite > 0) {
		max = MIN(towrite, gd->frame_size);
		frame_size = gsm0710_basic_fill_frame(frame, dlc,
						GSM0710_DATA, data, max);
		g_at_mux_raw_write(mux, frame, frame_size);
		data = data + max;
		towrite -= max;
	}
}

static GAtMuxDriver gsm0710_basic_driver = {
	.remove = gsm0710_basic_remove,
	.startup = gsm0710_basic_startup,
	.shutdown = gsm0710_basic_shutdown,
	.open_dlc = gsm0710_basic_open_dlc,
	.close_dlc = gsm0710_basic_close_dlc,
	.feed_data = gsm0710_basic_feed_data,
	.set_status = gsm0710_basic_set_status,
	.write = gsm0710_basic_write,
};

GAtMux *g_at_mux_new_gsm0710_basic(GIOChannel *channel, int frame_size)
{
	GAtMux *mux;
	struct gsm0710_data *gd;

	mux = g_at_mux_new(channel, &gsm0710_basic_driver);

	if (mux == NULL)
		return NULL;

	gd = g_new0(struct gsm0710_data, 1);
	gd->frame_size = frame_size;

	g_at_mux_set_data(mux, gd);

	return mux;
}

static void gsm0710_advanced_write_frame(GAtMux *mux, guint8 dlc, guint8 control,
					const guint8 *data, int towrite)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);
	guint8 *frame = alloca(gd->frame_size * 2 + 7);
	int frame_size;

	frame_size = gsm0710_advanced_fill_frame(frame, dlc, control,
							data, towrite);
	g_at_mux_raw_write(mux, frame, frame_size);
}

static void gsm0710_advanced_remove(GAtMux *mux)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);

	g_free(gd);
	g_at_mux_set_data(mux, NULL);
}

static gboolean gsm0710_advanced_startup(GAtMux *mux)
{
	guint8 frame[8]; /* Account for escapes */
	int frame_size;

	frame_size = gsm0710_advanced_fill_frame(frame, 0,
						GSM0710_OPEN_CHANNEL, NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static gboolean gsm0710_advanced_shutdown(GAtMux *mux)
{
	guint8 frame[8]; /* Account for escapes */
	int frame_size;

	frame_size = gsm0710_advanced_fill_frame(frame, 0,
						GSM0710_CLOSE_CHANNEL, NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static gboolean gsm0710_advanced_open_dlc(GAtMux *mux, guint8 dlc)
{
	guint8 frame[8]; /* Account for escapes */
	int frame_size;

	frame_size = gsm0710_advanced_fill_frame(frame, dlc,
						GSM0710_OPEN_CHANNEL, NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static gboolean gsm0710_advanced_close_dlc(GAtMux *mux, guint8 dlc)
{
	guint8 frame[8]; /* Account for escapes */
	int frame_size;

	frame_size = gsm0710_advanced_fill_frame(frame, dlc,
						GSM0710_CLOSE_CHANNEL, NULL, 0);
	g_at_mux_raw_write(mux, frame, frame_size);

	return TRUE;
}

static int gsm0710_advanced_feed_data(GAtMux *mux, void *data, int len)
{
	int total = 0;
	int nread;
	guint8 dlc;
	guint8 ctrl;
	guint8 *frame;
	int frame_len;

	do {
		frame = NULL;
		nread = gsm0710_advanced_extract_frame(data, len, &dlc, &ctrl,
							&frame, &frame_len);

		total += nread;
		data += nread;
		len -= nread;

		if (frame == NULL)
			break;

		gsm0710_packet(mux, dlc, ctrl, frame, frame_len,
				gsm0710_advanced_write_frame);
	} while (nread > 0);

	return total;
}

static void gsm0710_advanced_set_status(GAtMux *mux, guint8 dlc, guint8 status)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);
	guint8 *frame = alloca(gd->frame_size * 2 + 7);
	int frame_size;

	COMPOSE_STATUS_FRAME(data, dlc, status);
	frame_size = gsm0710_advanced_fill_frame(frame, 0,
							GSM0710_DATA, data, 4);
	g_at_mux_raw_write(mux, frame, frame_size);
}

static void gsm0710_advanced_write(GAtMux *mux, guint8 dlc,
					const void *data, int towrite)
{
	struct gsm0710_data *gd = g_at_mux_get_data(mux);
	guint8 *frame = alloca(gd->frame_size * 2 + 7);
	int max;
	int frame_size;

	while (towrite > 0) {
		max = MIN(towrite, gd->frame_size);
		frame_size = gsm0710_advanced_fill_frame(frame, dlc,
						GSM0710_DATA, data, max);
		g_at_mux_raw_write(mux, frame, frame_size);
		data = data + max;
		towrite -= max;
	}
}

static GAtMuxDriver gsm0710_advanced_driver = {
	.remove = gsm0710_advanced_remove,
	.startup = gsm0710_advanced_startup,
	.shutdown = gsm0710_advanced_shutdown,
	.open_dlc = gsm0710_advanced_open_dlc,
	.close_dlc = gsm0710_advanced_close_dlc,
	.feed_data = gsm0710_advanced_feed_data,
	.set_status = gsm0710_advanced_set_status,
	.write = gsm0710_advanced_write,
};

GAtMux *g_at_mux_new_gsm0710_advanced(GIOChannel *channel, int frame_size)
{
	GAtMux *mux;
	struct gsm0710_data *gd;

	mux = g_at_mux_new(channel, &gsm0710_advanced_driver);

	if (mux == NULL)
		return NULL;

	gd = g_new0(struct gsm0710_data, 1);
	gd->frame_size = frame_size;

	g_at_mux_set_data(mux, gd);

	return mux;
}
