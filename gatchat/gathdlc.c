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

#include <glib.h>

#include "crc-ccitt.h"
#include "ringbuffer.h"
#include "gatutil.h"
#include "gathdlc.h"

#define BUFFER_SIZE 2048

struct _GAtHDLC {
	gint ref_count;
	GIOChannel *channel;
	guint read_watch;
	struct ring_buffer *read_buffer;
	guint max_read_attempts;
	unsigned char *decode_buffer;
	guint decode_offset;
	guint16 decode_fcs;
	GAtReceiveFunc receive_func;
	gpointer receive_data;
	GAtDebugFunc debugf;
	gpointer debug_data;
};

static void read_watch_destroy(gpointer user_data)
{
	GAtHDLC *hdlc = user_data;

	hdlc->read_watch = 0;
}

static void new_bytes(GAtHDLC *hdlc)
{
	unsigned int len = ring_buffer_len(hdlc->read_buffer);
	unsigned char *buf = ring_buffer_read_ptr(hdlc->read_buffer, 0);
	unsigned char val;
	unsigned int pos = 0;

	while (pos < len) {
		if (buf[pos] == 0x7e) {
			if (hdlc->receive_func && hdlc->decode_offset > 2 &&
						hdlc->decode_fcs == 0xf0b8) {
				hdlc->receive_func(hdlc->decode_buffer,
							hdlc->decode_offset - 2,
							hdlc->receive_data);
			}

			hdlc->decode_fcs = 0xffff;
			hdlc->decode_offset = 0;
			pos++;
			continue;
		}

		if (buf[pos] == 0x7d) {
			if (pos + 2 > len)
				break;
			pos++;
			val = buf[pos] ^ 0x20;
		} else
			val = buf[pos];

		hdlc->decode_buffer[hdlc->decode_offset] = val;
		hdlc->decode_fcs = crc_ccitt_byte(hdlc->decode_fcs, val);

		hdlc->decode_offset++;
		pos++;
	}

	ring_buffer_drain(hdlc->read_buffer, pos);
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	GAtHDLC *hdlc = user_data;
	unsigned char *buf;
	GIOError err;
	gsize rbytes;
	gsize toread;
	gsize total_read = 0;
	guint read_count = 0;

	if (cond & G_IO_NVAL)
		return FALSE;

	/* Regardless of condition, try to read all the data available */
	do {
		toread = ring_buffer_avail_no_wrap(hdlc->read_buffer);

		if (toread == 0)
			break;

		rbytes = 0;
		buf = ring_buffer_write_ptr(hdlc->read_buffer);

		err = g_io_channel_read(channel, (char *) buf, toread, &rbytes);
		g_at_util_debug_dump(TRUE, buf, rbytes,
					hdlc->debugf, hdlc->debug_data);

		read_count++;

		total_read += rbytes;

		if (rbytes > 0)
			ring_buffer_write_advance(hdlc->read_buffer, rbytes);

	} while (err == G_IO_ERROR_NONE && rbytes > 0 &&
					read_count < hdlc->max_read_attempts);

	if (total_read > 0)
		new_bytes(hdlc);

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (read_count > 0 && rbytes == 0 && err != G_IO_ERROR_AGAIN)
		return FALSE;

	return TRUE;
}

GAtHDLC *g_at_hdlc_new(GIOChannel *channel)
{
	GAtHDLC *hdlc;

	if (!channel)
		return NULL;

	hdlc = g_try_new0(GAtHDLC, 1);
	if (!hdlc)
		return NULL;

	hdlc->ref_count = 1;
	hdlc->decode_fcs = 0xffff;
	hdlc->decode_offset = 0;
	hdlc->max_read_attempts = 8;

	hdlc->read_buffer = ring_buffer_new(BUFFER_SIZE);
	if (!hdlc->read_buffer)
		goto error;

	hdlc->decode_buffer = g_try_malloc(BUFFER_SIZE * 2);
	if (!hdlc->decode_buffer)
		goto error;

	if (g_at_util_setup_io(channel, G_IO_FLAG_NONBLOCK) == FALSE)
		goto error;

	hdlc->channel = g_io_channel_ref(channel);

	g_io_channel_set_buffered(hdlc->channel, FALSE);

	hdlc->read_watch = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, hdlc, read_watch_destroy);

	return hdlc;

error:
	if (hdlc->read_buffer)
		ring_buffer_free(hdlc->read_buffer);

	if (hdlc->decode_buffer)
		g_free(hdlc->decode_buffer);

	g_free(hdlc);

	return NULL;
}

GAtHDLC *g_at_hdlc_ref(GAtHDLC *hdlc)
{
	if (!hdlc)
		return NULL;

	g_atomic_int_inc(&hdlc->ref_count);

	return hdlc;
}

void g_at_hdlc_unref(GAtHDLC *hdlc)
{
	if (!hdlc)
		return;

	if (g_atomic_int_dec_and_test(&hdlc->ref_count) == FALSE)
		return;

	if (hdlc->read_watch > 0)
		g_source_remove(hdlc->read_watch);

	g_io_channel_unref(hdlc->channel);

	ring_buffer_free(hdlc->read_buffer);
	g_free(hdlc->decode_buffer);
}

void g_at_hdlc_set_debug(GAtHDLC *hdlc, GAtDebugFunc func, gpointer user_data)
{
	if (!hdlc)
		return;

	hdlc->debugf = func;
	hdlc->debug_data = user_data;
}

void g_at_hdlc_set_receive(GAtHDLC *hdlc, GAtReceiveFunc func,
							gpointer user_data)
{
	if (!hdlc)
		return;

	hdlc->receive_func = func;
	hdlc->receive_data = user_data;
}

static inline void hdlc_put(GAtHDLC *hdlc, guint8 *buf, gsize *pos, guint8 c)
{
	gsize i = *pos;

	if (c == 0x7e || c == 0x7d) {
		buf[i++] = 0x7d;
		buf[i++] = c ^ 0x20;
	} else
		buf[i++] = c;

	*pos = i;
}

gboolean g_at_hdlc_send(GAtHDLC *hdlc, const unsigned char *buf, gsize len)
{
	unsigned char newbuf[BUFFER_SIZE * 2];
	GIOError err;
	gsize bytes_written;
	gsize pos = 0, i = 0;
	guint16 fcs = 0xffff;

	newbuf[pos++] = 0x7e;

	while (len--) {
		fcs = crc_ccitt_byte(fcs, buf[i]);
		hdlc_put(hdlc, newbuf, &pos, buf[i++]);
	}

	fcs ^= 0xffff;
	hdlc_put(hdlc, newbuf, &pos, fcs & 0xff);
	hdlc_put(hdlc, newbuf, &pos, fcs >> 8);

	newbuf[pos++] = 0x7e;

	err = g_io_channel_write(hdlc->channel, (const char *) newbuf,
							pos, &bytes_written);
	g_at_util_debug_dump(FALSE, newbuf, bytes_written,
					hdlc->debugf, hdlc->debug_data);

	return TRUE;
}
