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

#define BUF_SIZE 4096
/* #define WRITE_SCHEDULER_DEBUG 1 */

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

/* AT command set that server supported */
struct at_command {
	GAtServerNotifyFunc notify;
	gpointer user_data;
	GDestroyNotify destroy_notify;
};

struct _GAtServer {
	gint ref_count;				/* Ref count */
	struct v250_settings v250;		/* V.250 command setting */
	GIOChannel *channel;			/* Server IO */
	guint read_watch;			/* GSource read id, 0 if none */
	guint write_watch;			/* GSource write id, 0 if none */
	guint read_so_far;			/* Number of bytes processed */
	GAtDisconnectFunc user_disconnect;	/* User disconnect func */
	gpointer user_disconnect_data;		/* User disconnect data */
	GAtDebugFunc debugf;			/* Debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	GHashTable *command_list;		/* List of AT commands */
	struct ring_buffer *read_buf;		/* Current read buffer */
	GQueue *write_queue;			/* Write buffer queue */
	guint max_read_attempts;		/* Max reads per select */
	enum ParserState parser_state;
	gboolean destroyed;			/* Re-entrancy guard */
};

static void g_at_server_wakeup_writer(GAtServer *server);

static struct ring_buffer *allocate_next(GAtServer *server)
{
	struct ring_buffer *buf = ring_buffer_new(BUF_SIZE);

	if (!buf)
		return NULL;

	g_queue_push_tail(server->write_queue, buf);

	return buf;
}

static void send_common(GAtServer *server, const char *buf, unsigned int len)
{
	gsize towrite = len;
	gsize bytes_written = 0;
	struct ring_buffer *write_buf;

	write_buf = g_queue_peek_tail(server->write_queue);

	while (bytes_written < towrite) {
		gsize wbytes = MIN((gsize)ring_buffer_avail(write_buf),
						towrite - bytes_written);

		bytes_written += ring_buffer_write(write_buf,
							buf + bytes_written,
							wbytes);

		/*
		 * Make sure we don't allocate a buffer if we've written
		 * everything out already
		 */
		if (ring_buffer_avail(write_buf) == 0 &&
				bytes_written < towrite)
			write_buf = allocate_next(server);
	}

	g_at_server_wakeup_writer(server);
}

static void g_at_server_send_final(GAtServer *server, GAtServerResult result)
{
	struct v250_settings v250 = server->v250;
	const char *result_str = server_result_to_string(result);
	char buf[1024];
	char t = v250.s3;
	char r = v250.s4;
	unsigned int len;

	if (v250.quiet)
		return;

	if (result_str == NULL)
		return;

	if (v250.is_v1)
		len = snprintf(buf, sizeof(buf), "%c%c%s%c%c", t, r, result_str,
				t, r);
	else
		len = snprintf(buf, sizeof(buf), "%u%c", (unsigned int) result,
				t);

	send_common(server, buf, MIN(len, sizeof(buf)-1));
}

static inline gboolean is_extended_command_prefix(const char c)
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

static gboolean is_basic_command_prefix(const char *buf)
{
	if (g_ascii_isalpha(buf[0]))
		return TRUE;

	if (buf[0] == '&' && g_ascii_isalpha(buf[1]))
		return TRUE;

	return FALSE;
}

static void at_command_notify(GAtServer *server, char *command,
				char *prefix, GAtServerRequestType type)
{
	struct at_command *node;
	GAtResult result;

	node = g_hash_table_lookup(server->command_list, prefix);

	if (node == NULL) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	result.lines = g_slist_prepend(NULL, command);
	result.final_or_pdu = 0;

	node->notify(type, &result, node->user_data);

	g_slist_free(result.lines);
}

static unsigned int parse_extended_command(GAtServer *server, char *buf)
{
	const char *valid_extended_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						"0123456789!%-./:_";
	const char *separators = ";?=";
	unsigned int prefix_len, i;
	gboolean in_string = FALSE;
	gboolean seen_question = FALSE;
	gboolean seen_equals = FALSE;
	char prefix[18]; /* According to V250, 5.4.1 */
	GAtServerRequestType type;

	prefix_len = strcspn(buf, separators);

	if (prefix_len > 17 || prefix_len < 2)
		return 0;

	/* Convert to upper case, we will always use upper case naming */
	for (i = 0; i < prefix_len; i++)
		prefix[i] = g_ascii_toupper(buf[i]);

	prefix[prefix_len] = '\0';

	if (strspn(prefix + 1, valid_extended_chars) != (prefix_len - 1))
		return 0;

	/*
	 * V.250 Section 5.4.1: "The first character following "+" shall be
	 * an alphabetic character in the range "A" through "Z".
	 */
	if (prefix[1] <= 'A' || prefix[1] >= 'Z')
		return 0;

	if (buf[i] != '\0' && buf[i] != ';' && buf[i] != '?' && buf[i] != '=')
		return 0;

	type = G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY;

	/* Continue until we hit eol or ';' */
	while (buf[i] && !(buf[i] == ';' && in_string == FALSE)) {
		if (buf[i] == '"') {
			in_string = !in_string;
			goto next;
		}

		if (in_string == TRUE)
			goto next;

		if (buf[i] == '?') {
			if (seen_question || seen_equals)
				return 0;

			if (buf[i + 1] != '\0' && buf[i + 1] != ';')
				return 0;

			seen_question = TRUE;
			type = G_AT_SERVER_REQUEST_TYPE_QUERY;
		} else if (buf[i] == '=') {
			if (seen_equals || seen_question)
				return 0;

			seen_equals = TRUE;

			if (buf[i + 1] == '?')
				type = G_AT_SERVER_REQUEST_TYPE_SUPPORT;
			else
				type = G_AT_SERVER_REQUEST_TYPE_SET;
		}

next:
		i++;
	}

	/* We can scratch in this buffer, so mark ';' as null */
	buf[i] = '\0';

	at_command_notify(server, buf, prefix, type);

	/* Also consume the terminating null */
	return i + 1;
}

static unsigned int parse_basic_command(GAtServer *server, char *buf)
{
	return 0;
}

static void server_parse_line(GAtServer *server, char *line)
{
	unsigned int pos = 0;
	unsigned int len = strlen(line);

	if (len == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		return;
	}

	while (pos < len) {
		unsigned int consumed;

		if (is_extended_command_prefix(line[pos]))
			consumed = parse_extended_command(server, line + pos);
		else if (is_basic_command_prefix(line + pos))
			consumed = parse_basic_command(server, line + pos);
		else
			consumed = 0;

		if (consumed == 0) {
			g_at_server_send_final(server,
						G_AT_SERVER_RESULT_ERROR);
			break;
		}

		pos += consumed;
	}
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
	char s3 = p->v250.s3;
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

	/* We will strip AT and S3 */
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
		else if (*buf != s3)
			line[i++] = *buf;

		buf += 1;
		pos += 1;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(p->read_buf, pos);
	}

	/* Strip S3 */
	ring_buffer_drain(p->read_buf, p->read_so_far - strip_front - 2);

	line[i] = '\0';

	return line;
}

static void new_bytes(GAtServer *p)
{
	unsigned int len = ring_buffer_len(p->read_buf);
	unsigned int wrap = ring_buffer_len_no_wrap(p->read_buf);
	unsigned char *buf = ring_buffer_read_ptr(p->read_buf, p->read_so_far);
	enum ParserResult result;

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
			g_at_server_send_final(p, G_AT_SERVER_RESULT_OK);
			ring_buffer_drain(p->read_buf, p->read_so_far);
			break;

		case PARSER_RESULT_COMMAND:
		{
			char *line = extract_line(p);

			if (line) {
				server_parse_line(p, line);
				g_free(line);
			} else
				g_at_server_send_final(p,
						G_AT_SERVER_RESULT_ERROR);
			break;
		}

		case PARSER_RESULT_REPEAT_LAST:
			/* TODO */
			g_at_server_send_final(p, G_AT_SERVER_RESULT_OK);
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
		g_source_remove(p->read_watch);
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

		if (rbytes > 0) {
			if (server->v250.echo)
				send_common(server, (char *)buf, rbytes);

			ring_buffer_write_advance(server->read_buf, rbytes);
		}
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

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GAtServer *server = data;
	GIOError err;
	gsize bytes_written;
	gsize towrite;
	struct ring_buffer *write_buf;
	unsigned char *buf;
#ifdef WRITE_SCHEDULER_DEBUG
	int limiter;
#endif

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (!server->write_queue)
		return FALSE;

	/* Write data out from the head of the queue */
	write_buf = g_queue_peek_head(server->write_queue);

	buf = ring_buffer_read_ptr(write_buf, 0);

	towrite = ring_buffer_len_no_wrap(write_buf);

#ifdef WRITE_SCHEDULER_DEBUG
	limiter = towrite;

	if (limiter > 5)
		limiter = 5;
#endif

	err = g_io_channel_write(server->channel,
			(char *)buf,
#ifdef WRITE_SCHEDULER_DEBUG
			limiter,
#else
			towrite,
#endif
			&bytes_written);

	if (err != G_IO_ERROR_NONE) {
		g_source_remove(server->read_watch);
		return FALSE;
	}

	g_at_util_debug_chat(FALSE, (char *)buf, bytes_written, server->debugf,
				server->debug_data);

	ring_buffer_drain(write_buf, bytes_written);

	/* All data in current buffer is written, free it
	 * unless it's the last buffer in the queue.
	 */
	if ((ring_buffer_len(write_buf) == 0) &&
			(g_queue_get_length(server->write_queue) > 1)) {
		write_buf = g_queue_pop_head(server->write_queue);
		ring_buffer_free(write_buf);
		write_buf = g_queue_peek_head(server->write_queue);
	}

	if (ring_buffer_len(write_buf) > 0)
		return TRUE;

	return FALSE;
}

static void write_queue_free(GQueue *write_queue)
{
	struct ring_buffer *write_buf;

	while ((write_buf = g_queue_pop_head(write_queue)))
		ring_buffer_free(write_buf);

	g_queue_free(write_queue);
}

static void g_at_server_cleanup(GAtServer *server)
{
	/* Cleanup all received data */
	ring_buffer_free(server->read_buf);
	server->read_buf = NULL;

	/* Cleanup pending data to write */
	write_queue_free(server->write_queue);

	g_hash_table_destroy(server->command_list);
	server->command_list = NULL;

	server->channel = NULL;
}

static void read_watcher_destroy_notify(GAtServer *server)
{
	g_at_server_cleanup(server);
	server->read_watch = 0;

	if (server->user_disconnect)
		server->user_disconnect(server->user_disconnect_data);

	if (server->destroyed)
		g_free(server);
}

static void write_watcher_destroy_notify(GAtServer *server)
{
	server->write_watch = 0;
}

static void g_at_server_wakeup_writer(GAtServer *server)
{
	if (server->write_watch != 0)
		return;

	server->write_watch = g_io_add_watch_full(server->channel,
			G_PRIORITY_DEFAULT,
			G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			can_write_data, server,
			(GDestroyNotify)write_watcher_destroy_notify);
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

static void at_notify_node_destroy(gpointer data)
{
	struct at_command *node = data;

	if (node->destroy_notify)
		node->destroy_notify(node->user_data);

	g_free(node);
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
	server->command_list = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free,
							at_notify_node_destroy);
	server->read_buf = ring_buffer_new(BUF_SIZE);
	if (!server->read_buf)
		goto error;

	server->write_queue = g_queue_new();
	if (!server->write_queue)
		goto error;

	if (!allocate_next(server))
		goto error;

	server->max_read_attempts = 3;

	if (!g_at_util_setup_io(server->channel, G_IO_FLAG_NONBLOCK))
		goto error;

	server->read_watch = g_io_add_watch_full(io, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, server,
				(GDestroyNotify)read_watcher_destroy_notify);

	return server;

error:
	if (server->command_list)
		g_hash_table_destroy(server->command_list);

	if (server->read_buf)
		ring_buffer_free(server->read_buf);

	if (server->write_queue)
		write_queue_free(server->write_queue);

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

	/* glib delays the destruction of the watcher until it exits, this
	 * means we can't free the data just yet, even though we've been
	 * destroyed already.  We have to wait until the read_watcher
	 * destroy function gets called
	 */
	if (server->read_watch != 0)
		server->destroyed = TRUE;
	else
		g_free(server);
}

gboolean g_at_server_shutdown(GAtServer *server)
{
	if (!server)
		return FALSE;

	/* Don't trigger user disconnect on shutdown */
	server->user_disconnect = NULL;
	server->user_disconnect_data = NULL;

	if (server->write_watch)
		g_source_remove(server->write_watch);

	if (server->read_watch)
		g_source_remove(server->read_watch);

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

gboolean g_at_server_register(GAtServer *server, char *prefix,
					GAtServerNotifyFunc notify,
					gpointer user_data,
					GDestroyNotify destroy_notify)
{
	struct at_command *node;

	if (server == NULL || server->command_list == NULL)
		return FALSE;

	if (notify == NULL)
		return FALSE;

	if (prefix == NULL || strlen(prefix) == 0)
		return FALSE;

	node = g_try_new0(struct at_command, 1);
	if (!node)
		return FALSE;

	node->notify = notify;
	node->user_data = user_data;
	node->destroy_notify = destroy_notify;

	g_hash_table_replace(server->command_list, g_strdup(prefix), node);

	return TRUE;
}

gboolean g_at_server_unregister(GAtServer *server, const char *prefix)
{
	struct at_command *node;

	if (server == NULL || server->command_list == NULL)
		return FALSE;

	if (prefix == NULL || strlen(prefix) == 0)
		return FALSE;

	node = g_hash_table_lookup(server->command_list, prefix);
	if (!node)
		return FALSE;

	g_hash_table_remove(server->command_list, prefix);

	return TRUE;
}
