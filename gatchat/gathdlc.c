/*
 *
 *  AT chat library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>

#include "crc-ccitt.h"
#include "ringbuffer.h"
#include "gatio.h"
#include "gatutil.h"
#include "gathdlc.h"

#define BUFFER_SIZE	(2 * 2048)
#define MAX_BUFFERS	64	/* Maximum number of in-flight write buffers */
#define HDLC_OVERHEAD	256	/* Rough estimate of HDLC protocol overhead */

#define HDLC_FLAG	0x7e	/* Flag sequence */
#define HDLC_ESCAPE	0x7d	/* Asynchronous control escape */
#define HDLC_TRANS	0x20	/* Asynchronous transparency modifier */

#define HDLC_INITFCS	0xffff	/* Initial FCS value */
#define HDLC_GOODFCS	0xf0b8	/* Good final FCS value */

#define HDLC_FCS(fcs, c) crc_ccitt_byte(fcs, c)

#define GUARD_TIMEOUT	1000	/* Pause time before and after '+++' sequence */

struct _GAtHDLC {
	gint ref_count;
	GAtIO *io;
	GQueue *write_queue;	/* Write buffer queue */
	unsigned char *decode_buffer;
	guint decode_offset;
	guint16 decode_fcs;
	gboolean decode_escape;
	guint32 xmit_accm[8];
	guint32 recv_accm;
	GAtReceiveFunc receive_func;
	gpointer receive_data;
	GAtDebugFunc debugf;
	gpointer debug_data;
	int record_fd;
	gboolean in_read_handler;
	gboolean destroyed;
	gboolean wakeup_sent;
	gboolean start_frame_marker;
	gboolean no_carrier_detect;
	GAtSuspendFunc suspend_func;
	gpointer suspend_data;
	guint suspend_source;
	GTimer *timer;
	guint num_plus;
};

static inline void hdlc_record(GAtHDLC *hdlc, gboolean in,
					guint8 *data, guint16 length)
{
	guint16 len = htons(length);
	guint32 ts;
	struct timeval now;
	unsigned char id;
	int err;

	g_at_util_debug_hexdump(in, data, length,
					hdlc->debugf, hdlc->debug_data);

	if (hdlc->record_fd < 0)
		return;

	if (length == 0)
		return;

	gettimeofday(&now, NULL);
	ts = htonl(now.tv_sec & 0xffffffff);

	id = 0x07;

	err = write(hdlc->record_fd, &id, 1);
	if (err < 0)
		return;

	err = write(hdlc->record_fd, &ts, 4);
	if (err < 0)
		return;

	id = in ? 0x02 : 0x01;

	err = write(hdlc->record_fd, &id, 1);
	if (err < 0)
		return;

	err = write(hdlc->record_fd, &len, 2);
	if (err < 0)
		return;

	err = write(hdlc->record_fd, data, length);
	if (err < 0)
		return;
}

void g_at_hdlc_set_recording(GAtHDLC *hdlc, const char *filename)
{
	if (hdlc == NULL)
		return;

	if (hdlc->record_fd > fileno(stderr)) {
		close(hdlc->record_fd);
		hdlc->record_fd = -1;
	}

	if (filename == NULL)
		return;

	hdlc->record_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND,
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
}

void g_at_hdlc_set_recv_accm(GAtHDLC *hdlc, guint32 accm)
{
	if (hdlc == NULL)
		return;

	hdlc->recv_accm = accm;
}

guint32 g_at_hdlc_get_recv_accm(GAtHDLC *hdlc)
{
	if (hdlc == NULL)
		return 0;

	return hdlc->recv_accm;
}

void g_at_hdlc_set_suspend_function(GAtHDLC *hdlc, GAtSuspendFunc func,
							gpointer user_data)
{
	if (hdlc == NULL)
		return;

	if (func == NULL) {
		if (hdlc->timer) {
			g_timer_destroy(hdlc->timer);
			hdlc->timer = NULL;
		}

		if (hdlc->suspend_source > 0) {
			g_source_remove(hdlc->suspend_source);
			hdlc->suspend_source = 0;
		}
	} else
		hdlc->timer = g_timer_new();

	hdlc->suspend_func = func;
	hdlc->suspend_data = user_data;
}

static gboolean hdlc_suspend(gpointer user_data)
{
	GAtHDLC *hdlc = user_data;

	g_at_io_drain_ring_buffer(hdlc->io, 3);

	g_at_io_set_write_handler(hdlc->io, NULL, NULL);
	g_at_io_set_read_handler(hdlc->io, NULL, NULL);

	if (hdlc->suspend_func)
		hdlc->suspend_func(hdlc->suspend_data);

	hdlc->suspend_source = 0;

	return FALSE;
}

static gboolean check_escape(GAtHDLC *hdlc, struct ring_buffer *rbuf)
{
	unsigned int len = ring_buffer_len(rbuf);
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	unsigned char *buf = ring_buffer_read_ptr(rbuf, 0);
	unsigned int pos = 0;
	unsigned int elapsed = g_timer_elapsed(hdlc->timer, NULL) * 1000;
	unsigned int num_plus = 0;
	gboolean guard_timeout = FALSE;

	if (elapsed >= GUARD_TIMEOUT)
		guard_timeout = TRUE;

	while (pos < len && pos < 3) {
		if (*buf != '+')
			break;

		num_plus++;
		buf++;
		pos++;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(rbuf, pos);
	}

	if (num_plus != len)
		return FALSE;

	/* We got some escape chars, but no guard timeout first */
	if (guard_timeout == FALSE && hdlc->num_plus == 0)
		return FALSE;

	if (num_plus != 3) {
		hdlc->num_plus = num_plus;
		return TRUE;
	}

	hdlc->num_plus = 0;
	hdlc->suspend_source = g_timeout_add(GUARD_TIMEOUT, hdlc_suspend, hdlc);

	return TRUE;
}

static void new_bytes(struct ring_buffer *rbuf, gpointer user_data)
{
	GAtHDLC *hdlc = user_data;
	unsigned int len = ring_buffer_len(rbuf);
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	unsigned char *buf = ring_buffer_read_ptr(rbuf, 0);
	unsigned int pos = 0;

	/*
	 * We delete the the paused_timeout_cb or hdlc_suspend as soons as
	 * we read a data.
	 */
	if (hdlc->suspend_source > 0) {
		g_source_remove(hdlc->suspend_source);
		hdlc->suspend_source = 0;
		g_timer_start(hdlc->timer);
	} else if (hdlc->timer) {
		gboolean escaping = check_escape(hdlc, rbuf);

		g_timer_start(hdlc->timer);

		if (escaping)
			return;
	}

	hdlc_record(hdlc, TRUE, buf, wrap);

	hdlc->in_read_handler = TRUE;

	while (pos < len) {
		/*
		 * We try to detect NO CARRIER conditions here.  We
		 * (ab) use the fact that a HDLC_FLAG must be followed
		 * by the Address or Protocol fields, depending on whether
		 * ACFC is enabled.
		 */
		if (hdlc->no_carrier_detect &&
				hdlc->decode_offset == 0 && *buf == '\r')
			break;

		if (hdlc->decode_escape == TRUE) {
			unsigned char val = *buf ^ HDLC_TRANS;

			hdlc->decode_buffer[hdlc->decode_offset++] = val;
			hdlc->decode_fcs = HDLC_FCS(hdlc->decode_fcs, val);

			hdlc->decode_escape = FALSE;
		} else if (*buf == HDLC_ESCAPE) {
			hdlc->decode_escape = TRUE;
		} else if (*buf == HDLC_FLAG) {
			if (hdlc->receive_func && hdlc->decode_offset > 2 &&
					hdlc->decode_fcs == HDLC_GOODFCS) {
				hdlc->receive_func(hdlc->decode_buffer,
							hdlc->decode_offset - 2,
							hdlc->receive_data);

				if (hdlc->destroyed)
					goto out;
			}

			hdlc->decode_fcs = HDLC_INITFCS;
			hdlc->decode_offset = 0;
		} else if (*buf >= 0x20 ||
					(hdlc->recv_accm & (1 << *buf)) == 0) {
			hdlc->decode_buffer[hdlc->decode_offset++] = *buf;
			hdlc->decode_fcs = HDLC_FCS(hdlc->decode_fcs, *buf);
		}

		buf++;
		pos++;

		if (pos == wrap) {
			buf = ring_buffer_read_ptr(rbuf, pos);
			hdlc_record(hdlc, TRUE, buf, len - wrap);
		}
	}

out:
	ring_buffer_drain(rbuf, pos);

	hdlc->in_read_handler = FALSE;

	if (hdlc->destroyed)
		g_free(hdlc);
}

GAtHDLC *g_at_hdlc_new_from_io(GAtIO *io)
{
	GAtHDLC *hdlc;
	struct ring_buffer* write_buffer;

	if (io == NULL)
		return NULL;

	hdlc = g_try_new0(GAtHDLC, 1);
	if (hdlc == NULL)
		return NULL;

	hdlc->ref_count = 1;
	hdlc->decode_fcs = HDLC_INITFCS;
	hdlc->decode_offset = 0;
	hdlc->decode_escape = FALSE;

	hdlc->xmit_accm[0] = ~0U;
	hdlc->xmit_accm[3] = 0x60000000; /* 0x7d, 0x7e */
	hdlc->recv_accm = ~0U;

	write_buffer = ring_buffer_new(BUFFER_SIZE);
	if (!write_buffer)
		goto error;

	hdlc->write_queue = g_queue_new();
	if (!hdlc->write_queue)
		goto error;

	g_queue_push_tail(hdlc->write_queue, write_buffer);

	hdlc->decode_buffer = g_try_malloc(BUFFER_SIZE);
	if (!hdlc->decode_buffer)
		goto error;

	hdlc->record_fd = -1;

	hdlc->io = g_at_io_ref(io);
	g_at_io_set_read_handler(hdlc->io, new_bytes, hdlc);

	return hdlc;

error:
	if (hdlc->write_queue)
		g_queue_free(hdlc->write_queue);

	if (write_buffer)
		ring_buffer_free(write_buffer);

	g_free(hdlc->decode_buffer);

	g_free(hdlc);

	return NULL;
}

GAtHDLC *g_at_hdlc_new(GIOChannel *channel)
{
	GAtIO *io;
	GAtHDLC *hdlc;

	io = g_at_io_new(channel);
	if (io == NULL)
		return NULL;

	hdlc = g_at_hdlc_new_from_io(io);
	g_at_io_unref(io);

	return hdlc;
}

GAtHDLC *g_at_hdlc_ref(GAtHDLC *hdlc)
{
	if (hdlc == NULL)
		return NULL;

	g_atomic_int_inc(&hdlc->ref_count);

	return hdlc;
}

void g_at_hdlc_unref(GAtHDLC *hdlc)
{
	struct ring_buffer *write_buffer;

	if (hdlc == NULL)
		return;

	if (g_atomic_int_dec_and_test(&hdlc->ref_count) == FALSE)
		return;

	if (hdlc->record_fd > fileno(stderr)) {
		close(hdlc->record_fd);
		hdlc->record_fd = -1;
	}

	g_at_io_set_write_handler(hdlc->io, NULL, NULL);
	g_at_io_set_read_handler(hdlc->io, NULL, NULL);

	if (hdlc->suspend_source > 0)
		g_source_remove(hdlc->suspend_source);

	g_at_io_unref(hdlc->io);
	hdlc->io = NULL;

	while ((write_buffer = g_queue_pop_head(hdlc->write_queue)))
		ring_buffer_free(write_buffer);

	g_queue_free(hdlc->write_queue);

	g_free(hdlc->decode_buffer);

	g_timer_destroy(hdlc->timer);

	if (hdlc->in_read_handler)
		hdlc->destroyed = TRUE;
	else
		g_free(hdlc);
}

void g_at_hdlc_set_debug(GAtHDLC *hdlc, GAtDebugFunc func, gpointer user_data)
{
	if (hdlc == NULL)
		return;

	hdlc->debugf = func;
	hdlc->debug_data = user_data;
}

void g_at_hdlc_set_receive(GAtHDLC *hdlc, GAtReceiveFunc func,
							gpointer user_data)
{
	if (hdlc == NULL)
		return;

	hdlc->receive_func = func;
	hdlc->receive_data = user_data;
}

static gboolean can_write_data(gpointer data)
{
	GAtHDLC *hdlc = data;
	unsigned int len;
	unsigned char *buf;
	gsize bytes_written;
	struct ring_buffer* write_buffer;

	/* Write data out from the head of the queue */
	write_buffer = g_queue_peek_head(hdlc->write_queue);

	len = ring_buffer_len_no_wrap(write_buffer);
	buf = ring_buffer_read_ptr(write_buffer, 0);

	bytes_written = g_at_io_write(hdlc->io, (gchar *) buf, len);
	hdlc_record(hdlc, FALSE, buf, bytes_written);
	ring_buffer_drain(write_buffer, bytes_written);

	if (ring_buffer_len(write_buffer) > 0)
		return TRUE;

	/* All data in current buffer is written, free it
	 * unless it's the last buffer in the queue.
	 */
	if ((ring_buffer_len(write_buffer) == 0) &&
			(g_queue_get_length(hdlc->write_queue) > 1)) {
		write_buffer = g_queue_pop_head(hdlc->write_queue);
		ring_buffer_free(write_buffer);
		write_buffer = g_queue_peek_head(hdlc->write_queue);
	}

	if (ring_buffer_len(write_buffer) > 0)
		return TRUE;

	return FALSE;
}

void g_at_hdlc_set_xmit_accm(GAtHDLC *hdlc, guint32 accm)
{
	if (hdlc == NULL)
		return;

	hdlc->xmit_accm[0] = accm;
}

guint32 g_at_hdlc_get_xmit_accm(GAtHDLC *hdlc)
{
	if (hdlc == NULL)
		return 0;

	return hdlc->xmit_accm[0];
}

GAtIO *g_at_hdlc_get_io(GAtHDLC *hdlc)
{
	if (hdlc == NULL)
		return NULL;

	return hdlc->io;
}

#define NEED_ESCAPE(xmit_accm, c) xmit_accm[c >> 5] & (1 << (c & 0x1f))

gboolean g_at_hdlc_send(GAtHDLC *hdlc, const unsigned char *data, gsize size)
{
	struct ring_buffer* write_buffer = g_queue_peek_tail(hdlc->write_queue);

	unsigned int avail = ring_buffer_avail(write_buffer);
	unsigned int wrap = ring_buffer_avail_no_wrap(write_buffer);
	unsigned char *buf;
	unsigned char tail[2];
	unsigned int i = 0;
	guint16 fcs = HDLC_INITFCS;
	gboolean escape = FALSE;
	gsize pos = 0;

	if (avail < size + HDLC_OVERHEAD) {
		if (g_queue_get_length(hdlc->write_queue) > MAX_BUFFERS)
			return FALSE;	/* Too many pending buffers */

		write_buffer = ring_buffer_new(BUFFER_SIZE);
		if (write_buffer == NULL)
			return FALSE;

		g_queue_push_tail(hdlc->write_queue, write_buffer);

		avail = ring_buffer_avail(write_buffer);
		wrap = ring_buffer_avail_no_wrap(write_buffer);
	}

	i = 0;
	buf = ring_buffer_write_ptr(write_buffer, 0);

	if (hdlc->start_frame_marker == TRUE) {
		/* Protocol requires 0x7e as start marker */
		if (pos + 1 > avail)
			return FALSE;

		*buf++ = HDLC_FLAG;
		pos++;

		if (pos == wrap)
			buf = ring_buffer_write_ptr(write_buffer, pos);
	} else if (hdlc->wakeup_sent == FALSE) {
		/* Write an initial 0x7e as wakeup character */
		*buf++ = HDLC_FLAG;
		pos++;

		hdlc->wakeup_sent = TRUE;
	}

	while (pos < avail && i < size) {
		if (escape == TRUE) {
			fcs = HDLC_FCS(fcs, data[i]);
			*buf = data[i++] ^ HDLC_TRANS;
			escape = FALSE;
		} else if (NEED_ESCAPE(hdlc->xmit_accm, data[i])) {
			*buf = HDLC_ESCAPE;
			escape = TRUE;
		} else {
			fcs = HDLC_FCS(fcs, data[i]);
			*buf = data[i++];
		}

		buf++;
		pos++;

		if (pos == wrap)
			buf = ring_buffer_write_ptr(write_buffer, pos);
	}

	if (i < size)
		return FALSE;

	fcs ^= HDLC_INITFCS;
	tail[0] = fcs & 0xff;
	tail[1] = fcs >> 8;

	i = 0;

	while (pos < avail && i < sizeof(tail)) {
		if (escape == TRUE) {
			*buf = tail[i++] ^ HDLC_TRANS;
			escape = FALSE;
		} else if (NEED_ESCAPE(hdlc->xmit_accm, tail[i])) {
			*buf = HDLC_ESCAPE;
			escape = TRUE;
		} else {
			*buf = tail[i++];
		}

		buf++;
		pos++;

		if (pos == wrap)
			buf = ring_buffer_write_ptr(write_buffer, pos);
	}

	if (i < sizeof(tail))
		return FALSE;

	if (pos + 1 > avail)
		return FALSE;

	/* Add 0x7e as end marker */
	*buf = HDLC_FLAG;
	pos++;

	ring_buffer_write_advance(write_buffer, pos);

	g_at_io_set_write_handler(hdlc->io, can_write_data, hdlc);

	return TRUE;
}

void g_at_hdlc_set_start_frame_marker(GAtHDLC *hdlc, gboolean marker)
{
	if (hdlc == NULL)
		return;

	hdlc->start_frame_marker = marker;
}

void g_at_hdlc_set_no_carrier_detect(GAtHDLC *hdlc, gboolean detect)
{
	if (hdlc == NULL)
		return;

	hdlc->no_carrier_detect = detect;
}

void g_at_hdlc_suspend(GAtHDLC *hdlc)
{
	if (hdlc == NULL)
		return;

	g_at_io_set_write_handler(hdlc->io, NULL, NULL);
	g_at_io_set_read_handler(hdlc->io, NULL, NULL);
}

void g_at_hdlc_resume(GAtHDLC *hdlc)
{
	if (hdlc == NULL)
		return;

	g_at_io_set_read_handler(hdlc->io, new_bytes, hdlc);

	if (g_queue_get_length(hdlc->write_queue) > 0)
		g_at_io_set_write_handler(hdlc->io, can_write_data, hdlc);
}
