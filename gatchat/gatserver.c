/*
 *
 *  AT server library with GLib integration
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

#include <glib.h>

#include "ringbuffer.h"
#include "gatserver.h"

/* V.250 Table 1/V.250 Result codes */
static const char *server_result_to_string(GAtServerResult result)
{
	switch (result) {
	case G_AT_SERVER_RESULT_OK:
		return "OK";
	case G_AT_SERVER_RESULT_CONNECT:
		return "CONNECT";
	case G_AT_SERVER_RESULT_RING:
		return "RING";
	case G_AT_SERVER_RESULT_NO_CARRIER:
		return "NO CARRIER";
	case G_AT_SERVER_RESULT_ERROR:
		return "ERROR";
	case G_AT_SERVER_RESULT_NO_DIALTONE:
		return "NO DIALTONE";
	case G_AT_SERVER_RESULT_BUSY:
		return "BUSY";
	case G_AT_SERVER_RESULT_NO_ANSWER:
		return "NO ANSWER";
	default:
		return NULL;
	}
}

/* Basic command setting for V.250 */
struct v250_settings {
	char s3;			/* set by S3=<val> */
	char s4;			/* set by S4=<val> */
	char s5;			/* set by S5=<val> */
	gboolean echo;			/* set by E<val> */
	gboolean quiet;			/* set by Q<val> */
	gboolean is_v1;			/* set by V<val>, v0 or v1 */
	unsigned int res_format;	/* set by X<val> */
	unsigned int c109;		/* set by &C<val> */
	unsigned int c108;		/* set by &D<val> */
};

struct _GAtServer {
	gint ref_count;				/* Ref count */
	struct v250_settings v250;		/* V.250 command setting */
	GIOChannel *server_io;			/* Server IO */
	int server_watch;			/* Watch for server IO */
	guint read_so_far;			/* Number of bytes processed */
	GAtDisconnectFunc user_disconnect;	/* User disconnect func */
	gpointer user_disconnect_data;		/* User disconnect data */
	GAtDebugFunc debugf;			/* Debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	struct ring_buffer *buf;		/* Current read buffer */
};

static int at_server_parse(GAtServer *server, char *buf);

static void g_at_server_send_result(GAtServer *server, GAtServerResult result)
{
	struct v250_settings v250 = server->v250;
	const char *result_str = server_result_to_string(result);
	char buf[1024];
	char t = v250.s3;
	char r = v250.s4;
	gsize wbuf;

	if (v250.quiet)
		return;

	if (result_str == NULL)
		return;

	if (v250.is_v1)
		sprintf(buf, "%c%c%s%c%c", t, r, result_str, t, r);
	else
		sprintf(buf, "%u%c", (unsigned int) result, t);

	g_at_util_debug_chat(FALSE, buf, strlen(buf),
				server->debugf, server->debug_data);

	g_io_channel_write(server->server_io, (char *) buf, strlen(buf),
							&wbuf);
}

static gsize skip_space(const char *buf, gsize pos)
{
	gsize i = pos;
	char c = buf[i];

	while (c == ' ')
		c = buf[++i];

	return i;
}

static inline gboolean is_at_command_prefix(const char c)
{
	if (c == '&')
		return FALSE;

	return g_ascii_ispunct(c);
}

static int parse_at_command(GAtServer *server, char *buf)
{
	int res = G_AT_SERVER_RESULT_ERROR;

	return res;
}

static int parse_v250_settings(GAtServer *server, char *buf)
{
	int res = G_AT_SERVER_RESULT_ERROR;

	return res;
}

static int at_server_parse(GAtServer *server, char *buf)
{
	int res = G_AT_SERVER_RESULT_ERROR;
	gsize i = 0;
	char c;

	/* skip space after "AT" or previous command */
	i = skip_space(buf, i);

	c = buf[i];
	/* skip semicolon */
	if (c == ';')
		c = buf[++i];

	if (is_at_command_prefix(c) || c == 'A' || c == 'D' || c == 'H')
		res = parse_at_command(server, buf + i);
	else if (g_ascii_isalpha(c) || c == '&')
		res = parse_v250_settings(server, buf + i);
	else if (c == '\0')
		res = G_AT_SERVER_RESULT_OK;

	return res;
}

static void parse_buffer(GAtServer *server, char *buf)
{
	int res = G_AT_SERVER_RESULT_ERROR;
	gsize i = 0;

	if (!buf)
		return;

	g_at_util_debug_chat(TRUE, (char *) buf, strlen(buf),
				server->debugf, server->debug_data);

	/* skip header space */
	buf += skip_space(buf, i);

	/* Make sure the command line prefix is "AT" or "at" */
	if (g_str_has_prefix(buf, "AT") ||
				g_str_has_prefix(buf, "at"))
		res = at_server_parse(server, (char *) buf + 2);

	g_at_server_send_result(server, res);

	/* We're overflowing the buffer, shutdown the socket */
	if (server->buf && ring_buffer_avail(server->buf) == 0)
		g_at_server_shutdown(server);

	if (buf)
		g_free(buf);
}

static char *extract_line(GAtServer *p, unsigned int *unread)
{
	unsigned int wrap = ring_buffer_len_no_wrap(p->buf);
	unsigned int pos = 0;
	unsigned char *buf = ring_buffer_read_ptr(p->buf, pos);
	char s3 = p->v250.s3;
	char s4 = p->v250.s4;
	char *line;

	int strip_front = 0;
	int strip_tail = 0;
	int line_length = 0;

	while (pos < p->read_so_far) {
		if (*buf == s3 || *buf == s4)
			if (!line_length)
				strip_front += 1;
			else
				break;
		else
			line_length += 1;

		buf += 1;
		pos += 1;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(p->buf, pos);
	}

	if (!line_length) {
		ring_buffer_drain(p->buf, strip_front);
		return NULL;
	}

	line = g_try_new(char, line_length + 1);

	if (!line) {
		ring_buffer_drain(p->buf, p->read_so_far);
		return NULL;
	}

	ring_buffer_drain(p->buf, strip_front);
	ring_buffer_read(p->buf, line, line_length);

	line[line_length] = '\0';

	while (pos < p->read_so_far) {
		if (*buf == s3 || *buf == s4)
			strip_tail += 1;
		else
			break;

		buf += 1;
		pos += 1;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(p->buf, pos);
	}

	ring_buffer_drain(p->buf, strip_tail);

	*unread = p->read_so_far - strip_front - line_length - strip_tail;

	return line;
}

static void new_bytes(GAtServer *p)
{
	unsigned int len = ring_buffer_len(p->buf);
	unsigned int wrap = ring_buffer_len_no_wrap(p->buf);
	unsigned char *buf = ring_buffer_read_ptr(p->buf, p->read_so_far);
	char s3 = p->v250.s3;

	while (p->read_so_far < len) {
		gsize rbytes = MIN(len - p->read_so_far, wrap - p->read_so_far);
		unsigned char *s3_pos = memchr(buf, s3, rbytes);
		char *line = NULL;
		unsigned int unread = 0;

		p->read_so_far += rbytes;

		if (s3_pos)
			line = extract_line(p, &unread);

		buf += rbytes - unread;
		p->read_so_far -= unread;

		if (p->read_so_far == wrap) {
			buf = ring_buffer_read_ptr(p->buf, p->read_so_far);
			wrap = len;
		}

		if (s3_pos) {
			parse_buffer(p, line);

			len -= p->read_so_far;
			wrap -= p->read_so_far;
			p->read_so_far = 0;
		}
	}
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	unsigned char *buf;
	GAtServer *server = data;
	GIOError err;
	gsize rbytes;
	gsize toread;

	if (cond & G_IO_NVAL)
		return FALSE;

	do {
		toread = ring_buffer_avail_no_wrap(server->buf);

		if (toread == 0)
			break;

		rbytes = 0;
		buf = ring_buffer_write_ptr(server->buf);

		err = g_io_channel_read(channel, (char *) buf, toread, &rbytes);
		g_at_util_debug_chat(TRUE, (char *)buf, rbytes,
					server->debugf, server->debug_data);

		if (rbytes > 0) {
			ring_buffer_write_advance(server->buf, rbytes);

			new_bytes(server);
		}

	} while (err == G_IO_ERROR_NONE && rbytes > 0);

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (rbytes == 0 && err != G_IO_ERROR_AGAIN)
		return FALSE;

	return TRUE;
}

static void server_watcher_destroy_notify(GAtServer *server)
{
	server->server_watch = 0;

	ring_buffer_free(server->buf);
	server->buf = NULL;

	server->server_io = NULL;

	if (server->user_disconnect)
		server->user_disconnect(server->user_disconnect_data);
}

static void v250_settings_create(struct v250_settings *v250)
{
	v250->s3 = '\r';
	v250->s4 = '\n';
	v250->s5 = '\b';
	v250->echo = TRUE;
	v250->quiet = FALSE;
	v250->is_v1 = TRUE;
	v250->res_format = 0;
	v250->c109 = 1;
	v250->c108 = 0;
}

GAtServer *g_at_server_new(GIOChannel *io)
{
	GAtServer *server;

	if (!io)
		return NULL;

	server = g_try_new0(GAtServer, 1);
	if (!server)
		return NULL;

	server->ref_count = 1;
	v250_settings_create(&server->v250);
	server->server_io = io;
	server->read_so_far = 0;
	server->user_disconnect = NULL;
	server->user_disconnect_data = NULL;
	server->debugf = NULL;
	server->debug_data = NULL;
	server->buf = ring_buffer_new(4096);

	if (!server->buf)
		goto error;

	if (!g_at_util_setup_io(server->server_io, G_IO_FLAG_NONBLOCK))
		goto error;

	server->server_watch = g_io_add_watch_full(io,
				G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, server,
				(GDestroyNotify)server_watcher_destroy_notify);

	return server;

error:
	if (server->buf)
		ring_buffer_free(server->buf);

	if (server)
		g_free(server);

	return NULL;
}

GAtServer *g_at_server_ref(GAtServer *server)
{
	if (server == NULL)
		return NULL;

	g_atomic_int_inc(&server->ref_count);

	return server;
}

void g_at_server_unref(GAtServer *server)
{
	gboolean is_zero;

	if (server == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&server->ref_count);

	if (is_zero == FALSE)
		return;

	g_at_server_shutdown(server);
}

gboolean g_at_server_shutdown(GAtServer *server)
{
	if (!server)
		return FALSE;

	/* Don't trigger user disconnect on shutdown */
	server->user_disconnect = NULL;
	server->user_disconnect_data = NULL;

	if (server->server_watch) {
		g_source_remove(server->server_watch);
		server->server_watch = 0;
	}

	g_free(server);
	server = NULL;

	return TRUE;
}

gboolean g_at_server_set_disconnect_function(GAtServer *server,
						GAtDisconnectFunc disconnect,
						gpointer user)
{
	if (server == NULL)
		return FALSE;

	server->user_disconnect = disconnect;
	server->user_disconnect_data = user;

	return TRUE;
}

gboolean g_at_server_set_debug(GAtServer *server, GAtDebugFunc func,
						gpointer user)
{
	if (server == NULL)
		return FALSE;

	server->debugf = func;
	server->debug_data = user;

	return TRUE;
}
