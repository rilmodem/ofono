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
#include <assert.h>
#include <termios.h>

#include <glib.h>

#include "ringbuffer.h"
#include "gatresult.h"
#include "gatchat.h"

/* #define WRITE_SCHEDULER_DEBUG 1 */

static void g_at_chat_wakeup_writer(GAtChat *chat);

enum chat_state {
	PARSER_STATE_IDLE = 0,
	PARSER_STATE_INITIAL_CR,
	PARSER_STATE_INITIAL_LF,
	PARSER_STATE_RESPONSE,
	PARSER_STATE_TERMINATOR_CR,
	PARSER_STATE_RESPONSE_COMPLETE,
	PARSER_STATE_GUESS_MULTILINE_RESPONSE,
	PARSER_STATE_MULTILINE_RESPONSE,
	PARSER_STATE_MULTILINE_TERMINATOR_CR,
	PARSER_STATE_MULTILINE_COMPLETE,
	PARSER_STATE_PDU,
	PARSER_STATE_PDU_CR,
	PARSER_STATE_PDU_COMPLETE,
	PARSER_STATE_PROMPT,
	PARSER_STATE_PROMPT_COMPLETE
};

struct at_command {
	char *cmd;
	char **prefixes;
	guint id;
	GAtResultFunc callback;
	GAtNotifyFunc listing;
	gpointer user_data;
	GDestroyNotify notify;
};

struct at_notify_node {
	guint id;
	GAtNotifyFunc callback;
	gpointer user_data;
	GDestroyNotify notify;
};

struct at_notify {
	GSList *nodes;
	gboolean pdu;
};

struct _GAtChat {
	gint ref_count;				/* Ref count */
	guint next_cmd_id;			/* Next command id */
	guint next_notify_id;			/* Next notify id */
	guint read_watch;			/* GSource read id, 0 if none */
	guint write_watch;			/* GSource write id, 0 if none */
	GIOChannel *channel;			/* channel */
	GQueue *command_queue;			/* Command queue */
	guint cmd_bytes_written;		/* bytes written from cmd */
	GHashTable *notify_list;		/* List of notification reg */
	GAtDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	struct ring_buffer *buf;		/* Current read buffer */
	guint read_so_far;			/* Number of bytes processed */
	gboolean disconnecting;			/* Whether we're disconnecting */
	enum chat_state state;		/* Current chat state */
	int flags;
	char *pdu_notify;			/* Unsolicited Resp w/ PDU */
	GSList *response_lines;			/* char * lines of the response */
	char *wakeup;				/* command sent to wakeup modem */
	gdouble inactivity_time;		/* Period of inactivity */
	guint wakeup_timeout;			/* How long to wait for resp */
	GTimer *wakeup_timer;			/* Keep track of elapsed time */
};

static gint at_notify_node_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct at_notify_node *node = a;
	guint id = GPOINTER_TO_UINT(b);

	if (node->id < id)
		return -1;

	if (node->id > id)
		return 1;

	return 0;
}

static void at_notify_node_destroy(struct at_notify_node *node)
{
	if (node->notify)
		node->notify(node->user_data);

	g_free(node);
}

static void at_notify_destroy(struct at_notify *notify)
{
	g_slist_foreach(notify->nodes, (GFunc) at_notify_node_destroy, NULL);
	g_free(notify);
}

static gint at_command_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct at_command *command = a;
	guint id = GPOINTER_TO_UINT(b);

	if (command->id < id)
		return -1;

	if (command->id > id)
		return 1;

	return 0;
}

static struct at_command *at_command_create(const char *cmd,
						const char **prefix_list,
						GAtNotifyFunc listing,
						GAtResultFunc func,
						gpointer user_data,
						GDestroyNotify notify)
{
	struct at_command *c;
	gsize len;
	char **prefixes = NULL;

	if (prefix_list) {
		int num_prefixes = 0;
		int i;

		while (prefix_list[num_prefixes])
			num_prefixes += 1;

		prefixes = g_new(char *, num_prefixes + 1);

		for (i = 0; i < num_prefixes; i++)
			prefixes[i] = strdup(prefix_list[i]);

		prefixes[num_prefixes] = NULL;
	}

	c = g_try_new0(struct at_command, 1);

	if (!c)
		return 0;

	len = strlen(cmd);
	c->cmd = g_try_new(char, len + 2);

	if (!c->cmd) {
		g_free(c);
		return 0;
	}

	memcpy(c->cmd, cmd, len);

	/* If we have embedded '\r' then this is a command expecting a prompt
	 * from the modem.  Embed Ctrl-Z at the very end automatically
	 */
	if (strchr(cmd, '\r'))
		c->cmd[len] = 26;
	else
		c->cmd[len] = '\r';

	c->cmd[len+1] = '\0';

	c->prefixes = prefixes;
	c->callback = func;
	c->listing = listing;
	c->user_data = user_data;
	c->notify = notify;

	return c;
}

static void at_command_destroy(struct at_command *cmd)
{
	if (cmd->notify)
		cmd->notify(cmd->user_data);

	g_strfreev(cmd->prefixes);
	g_free(cmd->cmd);
	g_free(cmd);
}

static void g_at_chat_cleanup(GAtChat *chat)
{
	struct at_command *c;

	ring_buffer_free(chat->buf);
	chat->buf = NULL;

	/* Cleanup pending commands */
	while ((c = g_queue_pop_head(chat->command_queue)))
		at_command_destroy(c);

	g_queue_free(chat->command_queue);
	chat->command_queue = NULL;

	/* Cleanup any response lines we have pending */
	g_slist_foreach(chat->response_lines, (GFunc)g_free, NULL);
	g_slist_free(chat->response_lines);
	chat->response_lines = NULL;

	/* Cleanup registered notifications */
	g_hash_table_destroy(chat->notify_list);
	chat->notify_list = NULL;

	if (chat->pdu_notify) {
		g_free(chat->pdu_notify);
		chat->pdu_notify = NULL;
	}

	if (chat->wakeup) {
		g_free(chat->wakeup);
		chat->wakeup = NULL;
	}

	if (chat->wakeup_timer) {
		g_timer_destroy(chat->wakeup_timer);
		chat->wakeup_timer = 0;
	}
}

static void read_watcher_destroy_notify(GAtChat *chat)
{
	chat->read_watch = 0;

	if (chat->disconnecting)
		return;

	chat->channel = NULL;

	g_at_chat_cleanup(chat);

	if (chat->user_disconnect)
		chat->user_disconnect(chat->user_disconnect_data);
}

static void write_watcher_destroy_notify(GAtChat *chat)
{
	chat->write_watch = 0;
}

static void at_notify_call_callback(gpointer data, gpointer user_data)
{
	struct at_notify_node *node = data;
	GAtResult *result = user_data;

	node->callback(result, node->user_data);
}

static gboolean g_at_chat_match_notify(GAtChat *chat, char *line)
{
	GHashTableIter iter;
	struct at_notify *notify;
	char *prefix;
	gpointer key, value;
	gboolean ret = FALSE;
	GAtResult result;

	g_hash_table_iter_init(&iter, chat->notify_list);
	result.lines = 0;
	result.final_or_pdu = 0;

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		prefix = key;
		notify = value;

		if (!g_str_has_prefix(line, key))
			continue;

		if (notify->pdu) {
			chat->pdu_notify = line;
			chat->state = PARSER_STATE_PDU;
			return TRUE;
		}

		if (!result.lines)
			result.lines = g_slist_prepend(NULL, line);

		g_slist_foreach(notify->nodes, at_notify_call_callback,
					&result);
		ret = TRUE;
	}

	if (ret) {
		g_slist_free(result.lines);
		g_free(line);
		chat->state = PARSER_STATE_IDLE;
	}

	return ret;
}

static void g_at_chat_finish_command(GAtChat *p, gboolean ok,
						char *final)
{
	struct at_command *cmd = g_queue_pop_head(p->command_queue);

	/* Cannot happen, but lets be paranoid */
	if (!cmd)
		return;

	if (cmd->callback) {
		GAtResult result;

		p->response_lines = g_slist_reverse(p->response_lines);

		result.final_or_pdu = final;
		result.lines = p->response_lines;

		cmd->callback(ok, &result, cmd->user_data);
	}

	g_slist_foreach(p->response_lines, (GFunc)g_free, NULL);
	g_slist_free(p->response_lines);
	p->response_lines = NULL;

	g_free(final);

	at_command_destroy(cmd);

	p->cmd_bytes_written = 0;

	if (g_queue_peek_head(p->command_queue))
		g_at_chat_wakeup_writer(p);
}

struct terminator_info {
	const char *terminator;
	int len;
	gboolean success;
};

static struct terminator_info terminator_table[] = {
	{ "OK", -1, TRUE },
	{ "ERROR", -1, FALSE },
	{ "NO DIALTONE", -1, FALSE },
	{ "BUSY", -1, FALSE },
	{ "NO CARRIER", -1, FALSE },
	{ "CONNECT", -1, TRUE },
	{ "NO ANSWER", -1, FALSE },
	{ "+CMS ERROR:", 11, FALSE },
	{ "+CME ERROR:", 11, FALSE },
	{ "+EXT ERROR:", 11, FALSE }
};

static gboolean g_at_chat_handle_command_response(GAtChat *p,
							struct at_command *cmd,
							char *line)
{
	int i;
	int size = sizeof(terminator_table) / sizeof(struct terminator_info);

	p->state = PARSER_STATE_IDLE;

	for (i = 0; i < size; i++) {
		struct terminator_info *info = &terminator_table[i];

		if (info->len == -1 && !strcmp(line, info->terminator)) {
			g_at_chat_finish_command(p, info->success, line);
			return TRUE;
		}

		if (info->len > 0 &&
			!strncmp(line, info->terminator, info->len)) {
			g_at_chat_finish_command(p, info->success, line);
			return TRUE;
		}
	}

	if (cmd->prefixes) {
		int i;

		for (i = 0; cmd->prefixes[i]; i++)
			if (g_str_has_prefix(line, cmd->prefixes[i]))
				goto out;

		return FALSE;
	}

out:
	if (!(p->flags & G_AT_CHAT_FLAG_NO_LEADING_CRLF))
		p->state = PARSER_STATE_GUESS_MULTILINE_RESPONSE;

	if (cmd->listing) {
		GAtResult result;

		result.lines = g_slist_prepend(NULL, line);
		result.final_or_pdu = NULL;

		cmd->listing(&result, cmd->user_data);

		g_slist_free(result.lines);
		g_free(line);
	} else
		p->response_lines = g_slist_prepend(p->response_lines, line);

	return TRUE;
}

static void have_line(GAtChat *p, gboolean strip_preceding)
{
	/* We're not going to copy terminal <CR><LF> */
	unsigned int len = p->read_so_far - 2;
	char *str;
	struct at_command *cmd;

	/* If we have preceding <CR><LF> modify the len */
	if (strip_preceding)
		len -= 2;

	/* Make sure we have terminal null */
	str = g_try_new(char, len + 1);

	if (!str) {
		ring_buffer_drain(p->buf, p->read_so_far);
		return;
	}

	if (strip_preceding)
		ring_buffer_drain(p->buf, 2);
	ring_buffer_read(p->buf, str, len);
	ring_buffer_drain(p->buf, 2);

	str[len] = '\0';

	/* Check for echo, this should not happen, but lets be paranoid */
	if (!strncmp(str, "AT", 2) == TRUE)
		goto done;

	cmd = g_queue_peek_head(p->command_queue);

	if (cmd) {
		char c = cmd->cmd[p->cmd_bytes_written - 1];

		/* We check that we have submitted a terminator, in which case
		 * a command might have failed or completed successfully
		 *
		 * In the generic case, \r is at the end of the command, so we
		 * know the entire command has been submitted.  In the case of
		 * commands like CMGS, every \r or Ctrl-Z might result in a
		 * final response from the modem, so we check this as well.
		 */
		if ((c == '\r' || c == 26) &&
				g_at_chat_handle_command_response(p, cmd, str))
			return;
	}

	if (g_at_chat_match_notify(p, str) == TRUE)
		return;

done:
	/* No matches & no commands active, ignore line */
	g_free(str);
	p->state = PARSER_STATE_IDLE;
}

static void have_pdu(GAtChat *p)
{
	unsigned int len = p->read_so_far - 2;
	char *pdu;
	GHashTableIter iter;
	struct at_notify *notify;
	char *prefix;
	gpointer key, value;
	GAtResult result;

	pdu = g_try_new(char, len + 1);

	if (!pdu) {
		ring_buffer_drain(p->buf, p->read_so_far);
		goto out;
	}

	ring_buffer_read(p->buf, pdu, len);
	ring_buffer_drain(p->buf, 2);

	pdu[len] = '\0';

	result.lines = g_slist_prepend(NULL, p->pdu_notify);
	result.final_or_pdu = pdu;

	g_hash_table_iter_init(&iter, p->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		prefix = key;
		notify = value;

		if (!g_str_has_prefix(p->pdu_notify, prefix))
			continue;

		if (!notify->pdu)
			continue;

		g_slist_foreach(notify->nodes, at_notify_call_callback,
					&result);
	}

	g_slist_free(result.lines);

out:
	g_free(p->pdu_notify);
	p->pdu_notify = NULL;

	if (pdu)
		g_free(pdu);

	p->state = PARSER_STATE_IDLE;
}

static inline void parse_char(GAtChat *chat, char byte)
{
	switch (chat->state) {
	case PARSER_STATE_IDLE:
		if (byte == '\r')
			chat->state = PARSER_STATE_INITIAL_CR;
		else if (chat->flags & G_AT_CHAT_FLAG_NO_LEADING_CRLF) {
			if (byte == '>')
				chat->state = PARSER_STATE_PROMPT;
			else
				chat->state = PARSER_STATE_RESPONSE;
		}
		break;

	case PARSER_STATE_INITIAL_CR:
		if (byte == '\n')
			chat->state = PARSER_STATE_INITIAL_LF;
		else if (byte != '\r' && /* Echo & no <CR><LF>?! */
			(chat->flags & G_AT_CHAT_FLAG_NO_LEADING_CRLF))
			chat->state = PARSER_STATE_RESPONSE;
		else if (byte != '\r')
			chat->state = PARSER_STATE_IDLE;
		break;

	case PARSER_STATE_INITIAL_LF:
		if (byte == '\r')
			chat->state = PARSER_STATE_TERMINATOR_CR;
		else if (byte == '>')
			chat->state = PARSER_STATE_PROMPT;
		else
			chat->state = PARSER_STATE_RESPONSE;
		break;

	case PARSER_STATE_RESPONSE:
		if (byte == '\r')
			chat->state = PARSER_STATE_TERMINATOR_CR;
		break;

	case PARSER_STATE_TERMINATOR_CR:
		if (byte == '\n')
			chat->state = PARSER_STATE_RESPONSE_COMPLETE;
		else
			chat->state = PARSER_STATE_IDLE;
		break;

	case PARSER_STATE_GUESS_MULTILINE_RESPONSE:
		if (byte == '\r')
			chat->state = PARSER_STATE_INITIAL_CR;
		else
			chat->state = PARSER_STATE_MULTILINE_RESPONSE;
		break;

	case PARSER_STATE_MULTILINE_RESPONSE:
		if (byte == '\r')
			chat->state = PARSER_STATE_MULTILINE_TERMINATOR_CR;
		break;

	case PARSER_STATE_MULTILINE_TERMINATOR_CR:
		if (byte == '\n')
			chat->state = PARSER_STATE_MULTILINE_COMPLETE;
		break;

	case PARSER_STATE_PDU:
		if (byte == '\r')
			chat->state = PARSER_STATE_PDU_CR;
		break;

	case PARSER_STATE_PDU_CR:
		if (byte == '\n')
			chat->state = PARSER_STATE_PDU_COMPLETE;
		break;

	case PARSER_STATE_PROMPT:
		if (byte == ' ')
			chat->state = PARSER_STATE_PROMPT_COMPLETE;
		else
			chat->state = PARSER_STATE_RESPONSE;
		break;

	case PARSER_STATE_RESPONSE_COMPLETE:
	case PARSER_STATE_PDU_COMPLETE:
	case PARSER_STATE_MULTILINE_COMPLETE:
	default:
		/* This really shouldn't happen */
		assert(FALSE);
		return;
	}
}

static void new_bytes(GAtChat *p)
{
	unsigned int len = ring_buffer_len(p->buf);
	unsigned int wrap = ring_buffer_len_no_wrap(p->buf);
	unsigned char *buf = ring_buffer_read_ptr(p->buf, p->read_so_far);

	while (p->read_so_far < len) {
		parse_char(p, *buf);

		buf += 1;
		p->read_so_far += 1;

		if (p->read_so_far == wrap) {
			buf = ring_buffer_read_ptr(p->buf, p->read_so_far);
			wrap = len;
		}

		if (p->state == PARSER_STATE_RESPONSE_COMPLETE) {
			gboolean strip_preceding;

			if (p->flags & G_AT_CHAT_FLAG_NO_LEADING_CRLF)
				strip_preceding = FALSE;
			else
				strip_preceding = TRUE;

			len -= p->read_so_far;
			wrap -= p->read_so_far;

			have_line(p, strip_preceding);

			p->read_so_far = 0;
		} else if (p->state == PARSER_STATE_MULTILINE_COMPLETE) {
			len -= p->read_so_far;
			wrap -= p->read_so_far;

			have_line(p, FALSE);

			p->read_so_far = 0;
		} else if (p->state == PARSER_STATE_PDU_COMPLETE) {
			len -= p->read_so_far;
			wrap -= p->read_so_far;

			have_pdu(p);

			p->read_so_far = 0;
		} else if (p->state == PARSER_STATE_INITIAL_CR) {
			len -= p->read_so_far - 1;
			wrap -= p->read_so_far - 1;

			ring_buffer_drain(p->buf, p->read_so_far - 1);

			p->read_so_far = 1;
		} else if (p->state == PARSER_STATE_PROMPT_COMPLETE) {
			len -= p->read_so_far;
			wrap -= p->read_so_far;

			g_at_chat_wakeup_writer(p);

			ring_buffer_drain(p->buf, p->read_so_far);

			p->read_so_far = 0;

			p->state = PARSER_STATE_IDLE;
		}
	}

	if (p->state == PARSER_STATE_IDLE && p->read_so_far > 0) {
		ring_buffer_drain(p->buf, p->read_so_far);
		p->read_so_far = 0;
	}
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	unsigned char *buf;
	GAtChat *chat = data;
	GIOError err;
	gsize rbytes;
	gsize toread;
	gsize total_read = 0;

	if (cond & G_IO_NVAL)
		return FALSE;

	/* Regardless of condition, try to read all the data available */
	do {
		rbytes = 0;

		toread = ring_buffer_avail_no_wrap(chat->buf);

		/* We're going to start overflowing the buffer
		 * this cannot happen under normal circumstances, so probably
		 * the channel is getting garbage, drop off
		 */
		if (toread == 0) {
			if (chat->state == PARSER_STATE_RESPONSE)
				return FALSE;

			err = G_IO_ERROR_AGAIN;
			break;
		}

		buf = ring_buffer_write_ptr(chat->buf);

		err = g_io_channel_read(channel, (char *) buf, toread, &rbytes);

		total_read += rbytes;

		if (rbytes > 0)
			ring_buffer_write_advance(chat->buf, rbytes);

	} while (err == G_IO_ERROR_NONE && rbytes > 0);

	if (total_read > 0)
		new_bytes(chat);

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (err == G_IO_ERROR_NONE && rbytes == 0)
		return FALSE;

	if (err != G_IO_ERROR_NONE && err != G_IO_ERROR_AGAIN)
		return FALSE;

	return TRUE;
}

static gboolean wakeup_no_response(gpointer user)
{
	GAtChat *chat = user;

	g_at_chat_finish_command(chat, FALSE, NULL);

	return FALSE;
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GAtChat *chat = data;
	struct at_command *cmd;
	GIOError err;
	gsize bytes_written;
	gsize towrite;
	gsize len;
	char *cr;
	gboolean wakeup_first = FALSE;
#ifdef WRITE_SCHEDULER_DEBUG
	int limiter;
#endif

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	/* Grab the first command off the queue and write as
	 * much of it as we can
	 */
	cmd = g_queue_peek_head(chat->command_queue);

	/* For some reason command queue is empty, cancel write watcher */
	if (cmd == NULL)
		return FALSE;

	len = strlen(cmd->cmd);

	/* For some reason write watcher fired, but we've already
	 * written the entire command out to the io channel,
	 * cancel write watcher
	 */
	if (chat->cmd_bytes_written >= len)
		return FALSE;

	if (chat->wakeup) {
		if (!chat->wakeup_timer) {
			wakeup_first = TRUE;
			chat->wakeup_timer = g_timer_new();

		} else if (g_timer_elapsed(chat->wakeup_timer, NULL) >
				chat->inactivity_time)
			wakeup_first = TRUE;
	}

	if (chat->cmd_bytes_written == 0 && wakeup_first == TRUE) {
		cmd = at_command_create(chat->wakeup, NULL, NULL, NULL,
					NULL, NULL);

		if (!cmd)
			return FALSE;

		g_queue_push_head(chat->command_queue, cmd);

		len = strlen(chat->wakeup);

		g_timeout_add(chat->wakeup_timeout, wakeup_no_response,
				chat);
	}

	towrite = len - chat->cmd_bytes_written;

	cr = strchr(cmd->cmd + chat->cmd_bytes_written, '\r');

	if (cr)
		towrite = cr - (cmd->cmd + chat->cmd_bytes_written) + 1;

#ifdef WRITE_SCHEDULER_DEBUG
	limiter = towrite;

	if (limiter > 5)
		limiter = 5;
#endif

	err = g_io_channel_write(chat->channel,
			cmd->cmd + chat->cmd_bytes_written,
#ifdef WRITE_SCHEDULER_DEBUG
			limiter,
#else
			towrite,
#endif
			&bytes_written);

	if (err != G_IO_ERROR_NONE) {
		g_at_chat_shutdown(chat);
		return FALSE;
	}

	chat->cmd_bytes_written += bytes_written;

	if (bytes_written < towrite)
		return TRUE;

	/* Full command submitted, update timer */
	if (chat->wakeup_timer)
		g_timer_start(chat->wakeup_timer);

	return FALSE;
}

static void g_at_chat_wakeup_writer(GAtChat *chat)
{
	if (chat->write_watch != 0)
		return;

	chat->write_watch = g_io_add_watch_full(chat->channel,
				G_PRIORITY_DEFAULT,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, chat,
				(GDestroyNotify)write_watcher_destroy_notify);
}

GAtChat *g_at_chat_new(GIOChannel *channel, int flags)
{
	GAtChat *chat;
	GIOFlags io_flags;

	if (!channel)
		return NULL;

	chat = g_try_new0(GAtChat, 1);

	if (!chat)
		return chat;

	chat->next_cmd_id = 1;
	chat->next_notify_id = 1;
	chat->flags = flags;

	chat->buf = ring_buffer_new(4096);

	if (!chat->buf)
		goto error;

	chat->command_queue = g_queue_new();

	if (!chat->command_queue)
		goto error;

	chat->notify_list = g_hash_table_new_full(g_str_hash, g_str_equal,
				g_free, (GDestroyNotify)at_notify_destroy);

	if (g_io_channel_set_encoding(channel, NULL, NULL) !=
			G_IO_STATUS_NORMAL)
		goto error;

	io_flags = g_io_channel_get_flags(channel);

	io_flags |= G_IO_FLAG_NONBLOCK;

	if (g_io_channel_set_flags(channel, io_flags, NULL) !=
			G_IO_STATUS_NORMAL)
		goto error;

	g_io_channel_set_close_on_unref(channel, TRUE);

	chat->channel = channel;
	chat->read_watch = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, chat,
				(GDestroyNotify)read_watcher_destroy_notify);

	return chat;

error:
	if (chat->buf)
		ring_buffer_free(chat->buf);

	if (chat->command_queue)
		g_queue_free(chat->command_queue);

	if (chat->notify_list)
		g_hash_table_destroy(chat->notify_list);

	g_free(chat);
	return NULL;
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

GAtChat *g_at_chat_new_from_tty(const char *device, int flags)
{
	GIOChannel *channel;
	int fd;

	fd = open_device(device);
	if (fd < 0)
		return NULL;

	channel = g_io_channel_unix_new(fd);
	if (!channel) {
		close(fd);
		return NULL;
	}

	return g_at_chat_new(channel, flags);
}

GAtChat *g_at_chat_ref(GAtChat *chat)
{
	if (chat == NULL)
		return NULL;

	g_atomic_int_inc(&chat->ref_count);

	return chat;
}

void g_at_chat_unref(GAtChat *chat)
{
	gboolean is_zero;

	if (chat == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&chat->ref_count);

	if (is_zero) {
		g_at_chat_shutdown(chat);

		g_at_chat_cleanup(chat);
		g_free(chat);
	}
}

gboolean g_at_chat_shutdown(GAtChat *chat)
{
	if (chat->channel == NULL)
		return FALSE;

	chat->disconnecting = TRUE;

	if (chat->read_watch)
		g_source_remove(chat->read_watch);

	if (chat->write_watch)
		g_source_remove(chat->write_watch);

	return TRUE;
}

gboolean g_at_chat_set_disconnect_function(GAtChat *chat,
			GAtDisconnectFunc disconnect, gpointer user_data)
{
	if (chat == NULL)
		return FALSE;

	chat->user_disconnect = disconnect;
	chat->user_disconnect_data = user_data;

	return TRUE;
}

static guint send_common(GAtChat *chat, const char *cmd,
			const char **prefix_list,
			GAtNotifyFunc listing, GAtResultFunc func,
			gpointer user_data, GDestroyNotify notify)
{
	struct at_command *c;

	if (chat == NULL || chat->command_queue == NULL)
		return 0;

	c = at_command_create(cmd, prefix_list, listing, func,
				user_data, notify);

	if (!c)
		return 0;

	c->id = chat->next_cmd_id++;

	g_queue_push_tail(chat->command_queue, c);

	if (g_queue_get_length(chat->command_queue) == 1)
		g_at_chat_wakeup_writer(chat);

	return c->id;
}

guint g_at_chat_send(GAtChat *chat, const char *cmd,
			const char **prefix_list, GAtResultFunc func,
			gpointer user_data, GDestroyNotify notify)
{
	return send_common(chat, cmd, prefix_list, NULL, func,
				user_data, notify);
}

guint g_at_chat_send_listing(GAtChat *chat, const char *cmd,
				const char **prefix_list,
				GAtNotifyFunc listing, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify)
{
	if (listing == NULL)
		return 0;

	return send_common(chat, cmd, prefix_list, listing, func,
				user_data, notify);
}

gboolean g_at_chat_cancel(GAtChat *chat, guint id)
{
	GList *l;

	if (chat == NULL || chat->command_queue == NULL)
		return FALSE;

	l = g_queue_find_custom(chat->command_queue, GUINT_TO_POINTER(id),
				at_command_compare_by_id);

	if (!l)
		return FALSE;

	if (l == g_queue_peek_head(chat->command_queue)) {
		struct at_command *c = l->data;

		/* We can't actually remove it since it is most likely
		 * already in progress, just null out the callback
		 * so it won't be called
		 */
		c->callback = NULL;
	} else {
		at_command_destroy(l->data);
		g_queue_remove(chat->command_queue, l->data);
	}

	return TRUE;
}

static struct at_notify *at_notify_create(GAtChat *chat, const char *prefix,
						gboolean pdu)
{
	struct at_notify *notify;
	char *key;

	key = g_strdup(prefix);

	if (!key)
		return 0;

	notify = g_try_new0(struct at_notify, 1);

	if (!notify) {
		g_free(key);
		return 0;
	}

	notify->pdu = pdu;

	g_hash_table_insert(chat->notify_list, key, notify);

	return notify;
}

guint g_at_chat_register(GAtChat *chat, const char *prefix,
				GAtNotifyFunc func, gboolean expect_pdu,
				gpointer user_data,
				GDestroyNotify destroy_notify)
{
	struct at_notify *notify;
	struct at_notify_node *node;

	if (chat == NULL || chat->notify_list == NULL)
		return 0;

	if (func == NULL)
		return 0;

	if (prefix == NULL || strlen(prefix) == 0)
		return 0;

	notify = g_hash_table_lookup(chat->notify_list, prefix);

	if (!notify)
		notify = at_notify_create(chat, prefix, expect_pdu);

	if (!notify || notify->pdu != expect_pdu)
		return 0;

	node = g_try_new0(struct at_notify_node, 1);

	if (!node)
		return 0;

	node->id = chat->next_notify_id++;
	node->callback = func;
	node->user_data = user_data;
	node->notify = destroy_notify;

	notify->nodes = g_slist_prepend(notify->nodes, node);

	return node->id;
}

gboolean g_at_chat_unregister(GAtChat *chat, guint id)
{
	GHashTableIter iter;
	struct at_notify *notify;
	char *prefix;
	gpointer key, value;
	GSList *l;

	if (chat == NULL || chat->notify_list == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, chat->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		prefix = key;
		notify = value;

		l = g_slist_find_custom(notify->nodes, GUINT_TO_POINTER(id),
					at_notify_node_compare_by_id);

		if (!l)
			continue;

		at_notify_node_destroy(l->data);
		notify->nodes = g_slist_remove(notify->nodes, l->data);

		if (notify->nodes == NULL)
			g_hash_table_iter_remove(&iter);

		return TRUE;
	}

	return TRUE;
}

gboolean g_at_chat_set_wakeup_command(GAtChat *chat, const char *cmd,
					unsigned int timeout, unsigned int msec)
{
	if (chat == NULL)
		return FALSE;

	if (chat->wakeup)
		g_free(chat->wakeup);

	chat->wakeup = g_strdup(cmd);
	chat->inactivity_time = (gdouble)msec / 1000;
	chat->wakeup_timeout = timeout;

	return TRUE;
}
