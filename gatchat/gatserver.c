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

enum ParserState {
	PARSER_STATE_IDLE,
	PARSER_STATE_A,
	PARSER_STATE_COMMAND,
	PARSER_STATE_GARBAGE,
};

enum ParserResult {
	PARSER_RESULT_COMMAND,
	PARSER_RESULT_EMPTY_COMMAND,
	PARSER_RESULT_REPEAT_LAST,
	PARSER_RESULT_GARBAGE,
	PARSER_RESULT_UNSURE,
};

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
	GIOChannel *channel;			/* Server IO */
	int server_watch;			/* Watch for server IO */
	guint read_so_far;			/* Number of bytes processed */
	GAtDisconnectFunc user_disconnect;	/* User disconnect func */
	gpointer user_disconnect_data;		/* User disconnect data */
	GAtDebugFunc debugf;			/* Debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	struct ring_buffer *read_buf;		/* Current read buffer */
	guint max_read_attempts;		/* Max reads per select */
	enum ParserState parser_state;
};

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
		snprintf(buf, sizeof(buf), "%c%c%s%c%c", t, r, result_str,
				t, r);
	else
		snprintf(buf, sizeof(buf), "%u%c", (unsigned int) result, t);

	g_at_util_debug_chat(FALSE, buf, strlen(buf),
				server->debugf, server->debug_data);

	g_io_channel_write(server->channel, (char *) buf, strlen(buf),
							&wbuf);
}

static inline gboolean is_at_command_prefix(const char c)
{
	switch (c) {
	case '+':
	case '*':
	case '!':
	case '%':
		return TRUE;
	default:
		return FALSE;
	}
}

static void parse_at_command(GAtServer *server, char *buf)
{
	g_at_server_send_result(server, G_AT_SERVER_RESULT_ERROR);
}

static void parse_v250_settings(GAtServer *server, char *buf)
{
	g_at_server_send_result(server, G_AT_SERVER_RESULT_ERROR);
}

static void server_parse_line(GAtServer *server, char *line)
{
	gsize i = 0;
	char c;

	if (line == NULL) {
		g_at_server_send_result(server, G_AT_SERVER_RESULT_ERROR);
		goto done;
	}

	if (line[0] == '\0') {
		g_at_server_send_result(server, G_AT_SERVER_RESULT_OK);
		goto done;
	}

	c = line[i];
	/* skip semicolon */
	if (c == ';')
		c = line[++i];

	if (is_at_command_prefix(c) || c == 'A' || c == 'D' || c == 'H')
		parse_at_command(server, line + i);
	else if (g_ascii_isalpha(c) || c == '&')
		parse_v250_settings(server, line + i);
	else
		g_at_server_send_result(server, G_AT_SERVER_RESULT_ERROR);

done:
	g_free(line);
}

static enum ParserResult server_feed(GAtServer *server,
					const char *bytes, gsize *len)
{
	gsize i = 0;
	enum ParserResult res = PARSER_RESULT_UNSURE;
	char s3 = server->v250.s3;

	while (i < *len) {
		char byte = bytes[i];

		switch (server->parser_state) {
		case PARSER_STATE_IDLE:
			if (byte == s3) {
				i += 1;
				res = PARSER_RESULT_EMPTY_COMMAND;
				goto out;
			} else if (byte == '\n') {
				i += 1;
				res = PARSER_RESULT_GARBAGE;
				goto out;
			} else if (byte == 'A' || byte == 'a')
				server->parser_state = PARSER_STATE_A;
			else if (byte != ' ' && byte != '\t')
				server->parser_state = PARSER_STATE_GARBAGE;
			break;

		case PARSER_STATE_A:
			if (byte == s3) {
				server->parser_state = PARSER_STATE_IDLE;
				i += 1;
				res = PARSER_RESULT_GARBAGE;
				goto out;
			} else if (byte == '/') {
				server->parser_state = PARSER_STATE_IDLE;
				i += 1;
				res = PARSER_RESULT_REPEAT_LAST;
				goto out;
			} else if (byte == 'T' || byte == 't')
				server->parser_state = PARSER_STATE_COMMAND;
			else
				server->parser_state = PARSER_STATE_GARBAGE;

			break;

		case PARSER_STATE_COMMAND:
			if (byte == s3) {
				server->parser_state = PARSER_STATE_IDLE;
				i += 1;
				res = PARSER_RESULT_COMMAND;
				goto out;
			}
			break;

		case PARSER_STATE_GARBAGE:
			if (byte == s3) {
				server->parser_state = PARSER_STATE_IDLE;
				i += 1;
				res = PARSER_RESULT_GARBAGE;
				goto out;
			}
			break;

		default:
			break;
		};

		i += 1;
	}

out:
	*len = i;
	return res;
}

static char *extract_line(GAtServer *p)
{
	unsigned int wrap = ring_buffer_len_no_wrap(p->read_buf);
	unsigned int pos = 0;
	unsigned char *buf = ring_buffer_read_ptr(p->read_buf, pos);
	int strip_front = 0;
	int line_length = 0;
	gboolean in_string = FALSE;
	char *line;
	int i;

	while (pos < p->read_so_far) {
		if (*buf == '"')
			in_string = !in_string;

		if (in_string == FALSE && (*buf == ' ' || *buf == '\t')) {
			if (line_length == 0)
				strip_front += 1;
		} else
			line_length += 1;

		buf += 1;
		pos += 1;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(p->read_buf, pos);
	}

	/* We will strip AT and \r */
	line_length -= 3;

	line = g_try_new(char, line_length + 1);

	if (!line) {
		ring_buffer_drain(p->read_buf, p->read_so_far);
		return NULL;
	}

	/* Strip leading whitespace + AT */
	ring_buffer_drain(p->read_buf, strip_front + 2);

	pos = 0;
	i = 0;
	wrap = ring_buffer_len_no_wrap(p->read_buf);
	buf = ring_buffer_read_ptr(p->read_buf, pos);

	while (pos < (p->read_so_far - strip_front - 2)) {
		if (*buf == '"')
			in_string = !in_string;

		if ((*buf == ' ' || *buf == '\t') && in_string == FALSE)
			; /* Skip */
		else if (*buf != '\r')
			line[i++] = *buf;

		buf += 1;
		pos += 1;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(p->read_buf, pos);
	}

	/* Strip \r */
	ring_buffer_drain(p->read_buf, p->read_so_far - strip_front - 2);

	line[i] = '\0';

	return line;
}

static void new_bytes(GAtServer *p)
{
	unsigned int len = ring_buffer_len(p->read_buf);
	unsigned int wrap = ring_buffer_len_no_wrap(p->read_buf);
	unsigned char *buf = ring_buffer_read_ptr(p->read_buf, p->read_so_far);
	enum ParserState result;

	while (p->channel && (p->read_so_far < len)) {
		gsize rbytes = MIN(len - p->read_so_far, wrap - p->read_so_far);
		result = server_feed(p, (char *)buf, &rbytes);

		buf += rbytes;
		p->read_so_far += rbytes;

		if (p->read_so_far == wrap) {
			buf = ring_buffer_read_ptr(p->read_buf, p->read_so_far);
			wrap = len;
		}

		if (result == PARSER_RESULT_UNSURE)
			continue;

		switch (result) {
		case PARSER_RESULT_EMPTY_COMMAND:
			/*
			 * According to section 5.2.4 and 5.6 of V250,
			 * Empty commands must be OK by the DCE
			 */
			g_at_server_send_result(p, G_AT_SERVER_RESULT_OK);
			ring_buffer_drain(p->read_buf, p->read_so_far);
			break;

		case PARSER_RESULT_COMMAND:
			server_parse_line(p, extract_line(p));
			break;

		case PARSER_RESULT_REPEAT_LAST:
			/* TODO */
			g_at_server_send_result(p, G_AT_SERVER_RESULT_OK);
			ring_buffer_drain(p->read_buf, p->read_so_far);
			break;

		default:
			ring_buffer_drain(p->read_buf, p->read_so_far);
			break;
		}

		len -= p->read_so_far;
		wrap -= p->read_so_far;
		p->read_so_far = 0;
	}

	/* We're overflowing the buffer, shutdown the socket */
	if (p->read_buf && ring_buffer_avail(p->read_buf) == 0)
		g_source_remove(p->server_watch);
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	unsigned char *buf;
	GAtServer *server = data;
	GIOError err;
	gsize rbytes;
	gsize toread;
	guint total_read = 0;
	guint read_count = 0;

	if (cond & G_IO_NVAL)
		return FALSE;

	do {
		toread = ring_buffer_avail_no_wrap(server->read_buf);

		if (toread == 0)
			break;

		rbytes = 0;
		buf = ring_buffer_write_ptr(server->read_buf);

		err = g_io_channel_read(channel, (char *) buf, toread, &rbytes);
		g_at_util_debug_chat(TRUE, (char *)buf, rbytes,
					server->debugf, server->debug_data);

		read_count++;

		total_read += rbytes;

		if (rbytes > 0)
			ring_buffer_write_advance(server->read_buf, rbytes);
	} while (err == G_IO_ERROR_NONE && rbytes > 0 &&
					read_count < server->max_read_attempts);

	if (total_read > 0)
		new_bytes(server);

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (read_count > 0 && rbytes == 0 && err != G_IO_ERROR_AGAIN)
		return FALSE;

	return TRUE;
}

static void server_watcher_destroy_notify(GAtServer *server)
{
	server->server_watch = 0;

	ring_buffer_free(server->read_buf);
	server->read_buf = NULL;

	server->channel = NULL;

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
	server->channel = io;
	server->read_buf = ring_buffer_new(4096);
	server->max_read_attempts = 3;

	if (!server->read_buf)
		goto error;

	if (!g_at_util_setup_io(server->channel, G_IO_FLAG_NONBLOCK))
		goto error;

	server->server_watch = g_io_add_watch_full(io, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, server,
				(GDestroyNotify)server_watcher_destroy_notify);

	return server;

error:
	if (server->read_buf)
		ring_buffer_free(server->read_buf);

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
