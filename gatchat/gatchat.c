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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>

#include <glib.h>

#include "ringbuffer.h"
#include "gatchat.h"
#include "gatio.h"

/* #define WRITE_SCHEDULER_DEBUG 1 */

#define COMMAND_FLAG_EXPECT_PDU			0x1
#define COMMAND_FLAG_EXPECT_SHORT_PROMPT	0x2

struct at_chat;
static void chat_wakeup_writer(struct at_chat *chat);

static const char *none_prefix[] = { NULL };

struct at_command {
	char *cmd;
	char **prefixes;
	guint flags;
	guint id;
	guint gid;
	GAtResultFunc callback;
	GAtNotifyFunc listing;
	gpointer user_data;
	GDestroyNotify notify;
};

struct at_notify_node {
	guint id;
	guint gid;
	GAtNotifyFunc callback;
	gpointer user_data;
	GDestroyNotify notify;
	gboolean destroyed;
};

typedef gboolean (*node_remove_func)(struct at_notify_node *node,
					gpointer user_data);

struct at_notify {
	GSList *nodes;
	gboolean pdu;
};

struct at_chat {
	gint ref_count;				/* Ref count */
	guint next_cmd_id;			/* Next command id */
	guint next_notify_id;			/* Next notify id */
	guint next_gid;				/* Next group id */
	GAtIO *io;				/* AT IO */
	GQueue *command_queue;			/* Command queue */
	guint cmd_bytes_written;		/* bytes written from cmd */
	GHashTable *notify_list;		/* List of notification reg */
	GAtDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	guint read_so_far;			/* Number of bytes processed */
	gboolean suspended;			/* Are we suspended? */
	GAtDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	char *pdu_notify;			/* Unsolicited Resp w/ PDU */
	GSList *response_lines;			/* char * lines of the response */
	char *wakeup;				/* command sent to wakeup modem */
	gint timeout_source;
	gdouble inactivity_time;		/* Period of inactivity */
	guint wakeup_timeout;			/* How long to wait for resp */
	GTimer *wakeup_timer;			/* Keep track of elapsed time */
	GAtSyntax *syntax;
	gboolean destroyed;			/* Re-entrancy guard */
	gboolean in_read_handler;		/* Re-entrancy guard */
	gboolean in_notify;
	GSList *terminator_list;		/* Non-standard terminator */
	guint16 terminator_blacklist;		/* Blacklisted terinators */
};

struct _GAtChat {
	gint ref_count;
	struct at_chat *parent;
	guint group;
	GAtChat *slave;
};

struct terminator_info {
	char *terminator;
	int len;
	gboolean success;
};

static gboolean node_is_destroyed(struct at_notify_node *node, gpointer user)
{
	return node->destroyed;
}

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

static void at_notify_node_destroy(gpointer data, gpointer user_data)
{
	struct at_notify_node *node = data;

	if (node->notify)
		node->notify(node->user_data);

	g_free(node);
}

static void at_notify_destroy(gpointer user_data)
{
	struct at_notify *notify = user_data;

	g_slist_foreach(notify->nodes, at_notify_node_destroy, NULL);
	g_slist_free(notify->nodes);
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

static gboolean at_chat_unregister_all(struct at_chat *chat,
					gboolean mark_only,
					node_remove_func func,
					gpointer userdata)
{
	GHashTableIter iter;
	struct at_notify *notify;
	struct at_notify_node *node;
	gpointer key, value;
	GSList *p;
	GSList *c;
	GSList *t;

	if (chat->notify_list == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, chat->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		notify = value;

		p = NULL;
		c = notify->nodes;

		while (c) {
			node = c->data;

			if (func(node, userdata) != TRUE) {
				p = c;
				c = c->next;
				continue;
			}

			if (mark_only) {
				node->destroyed = TRUE;
				p = c;
				c = c->next;
				continue;
			}

			if (p)
				p->next = c->next;
			else
				notify->nodes = c->next;

			at_notify_node_destroy(node, NULL);

			t = c;
			c = c->next;
			g_slist_free_1(t);
		}

		if (notify->nodes == NULL)
			g_hash_table_iter_remove(&iter);
	}

	return TRUE;
}

static struct at_command *at_command_create(guint gid, const char *cmd,
						const char **prefix_list,
						guint flags,
						GAtNotifyFunc listing,
						GAtResultFunc func,
						gpointer user_data,
						GDestroyNotify notify,
						gboolean wakeup)
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
	if (c == NULL)
		return 0;

	len = strlen(cmd);
	c->cmd = g_try_new(char, len + 2);
	if (c->cmd == NULL) {
		g_free(c);
		return 0;
	}

	memcpy(c->cmd, cmd, len);

	/* If we have embedded '\r' then this is a command expecting a prompt
	 * from the modem.  Embed Ctrl-Z at the very end automatically
	 */
	if (wakeup == FALSE) {
		if (strchr(cmd, '\r'))
			c->cmd[len] = 26;
		else
			c->cmd[len] = '\r';

		len += 1;
	}

	c->cmd[len] = '\0';

	c->gid = gid;
	c->flags = flags;
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

static void free_terminator(struct terminator_info *info)
{
	g_free(info->terminator);
	info->terminator = NULL;
	g_free(info);
	info = NULL;
}

static void chat_cleanup(struct at_chat *chat)
{
	struct at_command *c;

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

	if (chat->timeout_source) {
		g_source_remove(chat->timeout_source);
		chat->timeout_source = 0;
	}

	g_at_syntax_unref(chat->syntax);
	chat->syntax = NULL;

	if (chat->terminator_list) {
		g_slist_foreach(chat->terminator_list,
					(GFunc)free_terminator, NULL);
		g_slist_free(chat->terminator_list);
		chat->terminator_list = NULL;
	}
}

static void io_disconnect(gpointer user_data)
{
	struct at_chat *chat = user_data;

	chat_cleanup(chat);
	g_at_io_unref(chat->io);
	chat->io = NULL;

	if (chat->user_disconnect)
		chat->user_disconnect(chat->user_disconnect_data);
}

static void at_notify_call_callback(gpointer data, gpointer user_data)
{
	struct at_notify_node *node = data;
	GAtResult *result = user_data;

	node->callback(result, node->user_data);
}

static gboolean at_chat_match_notify(struct at_chat *chat, char *line)
{
	GHashTableIter iter;
	struct at_notify *notify;
	gpointer key, value;
	gboolean ret = FALSE;
	GAtResult result;

	g_hash_table_iter_init(&iter, chat->notify_list);
	result.lines = 0;
	result.final_or_pdu = 0;

	chat->in_notify = TRUE;

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		notify = value;

		if (!g_str_has_prefix(line, key))
			continue;

		if (notify->pdu) {
			chat->pdu_notify = line;

			if (chat->syntax->set_hint)
				chat->syntax->set_hint(chat->syntax,
							G_AT_SYNTAX_EXPECT_PDU);
			return TRUE;
		}

		if (result.lines == NULL)
			result.lines = g_slist_prepend(NULL, line);

		g_slist_foreach(notify->nodes, at_notify_call_callback,
					&result);
		ret = TRUE;
	}

	chat->in_notify = FALSE;

	if (ret) {
		g_slist_free(result.lines);
		g_free(line);

		at_chat_unregister_all(chat, FALSE, node_is_destroyed, NULL);
	}

	return ret;
}

static void at_chat_finish_command(struct at_chat *p, gboolean ok, char *final)
{
	struct at_command *cmd = g_queue_pop_head(p->command_queue);
	GSList *response_lines;

	/* Cannot happen, but lets be paranoid */
	if (cmd == NULL)
		return;

	p->cmd_bytes_written = 0;

	if (g_queue_peek_head(p->command_queue))
		chat_wakeup_writer(p);

	response_lines = p->response_lines;
	p->response_lines = NULL;

	if (cmd->callback) {
		GAtResult result;

		response_lines = g_slist_reverse(response_lines);

		result.final_or_pdu = final;
		result.lines = response_lines;

		cmd->callback(ok, &result, cmd->user_data);
	}

	g_slist_foreach(response_lines, (GFunc)g_free, NULL);
	g_slist_free(response_lines);

	g_free(final);
	at_command_destroy(cmd);
}

static struct terminator_info terminator_table[] = {
	{ "OK", -1, TRUE },
	{ "ERROR", -1, FALSE },
	{ "NO DIALTONE", -1, FALSE },
	{ "BUSY", -1, FALSE },
	{ "NO CARRIER", -1, FALSE },
	{ "CONNECT", 7, TRUE },
	{ "NO ANSWER", -1, FALSE },
	{ "+CMS ERROR:", 11, FALSE },
	{ "+CME ERROR:", 11, FALSE },
	{ "+EXT ERROR:", 11, FALSE }
};

static void at_chat_add_terminator(struct at_chat *chat, char *terminator,
					int len, gboolean success)
{
	struct terminator_info *info = g_new0(struct terminator_info, 1);
	info->terminator = g_strdup(terminator);
	info->len = len;
	info->success = success;
	chat->terminator_list = g_slist_prepend(chat->terminator_list, info);
}

static void at_chat_blacklist_terminator(struct at_chat *chat,
						GAtChatTerminator terminator)
{
	chat->terminator_blacklist |= 1 << terminator;
}

static gboolean check_terminator(struct terminator_info *info, char *line)
{
	if (info->len == -1 && !strcmp(line, info->terminator))
		return TRUE;

	if (info->len > 0 && !strncmp(line, info->terminator, info->len))
		return TRUE;

	return FALSE;
}

static gboolean at_chat_handle_command_response(struct at_chat *p,
							struct at_command *cmd,
							char *line)
{
	int i;
	int size = sizeof(terminator_table) / sizeof(struct terminator_info);
	int hint;
	GSList *l;

	for (i = 0; i < size; i++) {
		struct terminator_info *info = &terminator_table[i];
		if (check_terminator(info, line) &&
				(p->terminator_blacklist & 1 << i) == 0) {
			at_chat_finish_command(p, info->success, line);
			return TRUE;
		}
	}

	for (l = p->terminator_list; l; l = l->next) {
		struct terminator_info *info = l->data;
		if (check_terminator(info, line)) {
			at_chat_finish_command(p, info->success, line);
			return TRUE;
		}
	}

	if (cmd->prefixes) {
		int n;

		for (n = 0; cmd->prefixes[n]; n++)
			if (g_str_has_prefix(line, cmd->prefixes[n]))
				goto out;

		return FALSE;
	}

out:
	if (cmd->listing && (cmd->flags & COMMAND_FLAG_EXPECT_PDU))
		hint = G_AT_SYNTAX_EXPECT_PDU;
	else
		hint = G_AT_SYNTAX_EXPECT_MULTILINE;

	if (p->syntax->set_hint)
		p->syntax->set_hint(p->syntax, hint);

	if (cmd->listing && (cmd->flags & COMMAND_FLAG_EXPECT_PDU)) {
		p->pdu_notify = line;
		return TRUE;
	}

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

static void have_line(struct at_chat *p, char *str)
{
	/* We're not going to copy terminal <CR><LF> */
	struct at_command *cmd;

	if (str == NULL)
		return;

	/* Check for echo, this should not happen, but lets be paranoid */
	if (!strncmp(str, "AT", 2) == TRUE)
		goto done;

	cmd = g_queue_peek_head(p->command_queue);

	if (cmd && p->cmd_bytes_written > 0) {
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
				at_chat_handle_command_response(p, cmd, str))
			return;
	}

	if (at_chat_match_notify(p, str) == TRUE)
		return;

done:
	/* No matches & no commands active, ignore line */
	g_free(str);
}

static void have_notify_pdu(struct at_chat *p, char *pdu, GAtResult *result)
{
	GHashTableIter iter;
	struct at_notify *notify;
	char *prefix;
	gpointer key, value;
	gboolean called = FALSE;

	p->in_notify = TRUE;

	g_hash_table_iter_init(&iter, p->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		prefix = key;
		notify = value;

		if (!g_str_has_prefix(p->pdu_notify, prefix))
			continue;

		if (!notify->pdu)
			continue;

		g_slist_foreach(notify->nodes, at_notify_call_callback, result);
		called = TRUE;
	}

	p->in_notify = FALSE;

	if (called)
		at_chat_unregister_all(p, FALSE, node_is_destroyed, NULL);
}

static void have_pdu(struct at_chat *p, char *pdu)
{
	struct at_command *cmd;
	GAtResult result;
	gboolean listing_pdu = FALSE;

	if (pdu == NULL)
		goto error;

	result.lines = g_slist_prepend(NULL, p->pdu_notify);
	result.final_or_pdu = pdu;

	cmd = g_queue_peek_head(p->command_queue);

	if (cmd && (cmd->flags & COMMAND_FLAG_EXPECT_PDU) &&
			p->cmd_bytes_written > 0) {
		char c = cmd->cmd[p->cmd_bytes_written - 1];

		if (c == '\r')
			listing_pdu = TRUE;
	}

	if (listing_pdu) {
		cmd->listing(&result, cmd->user_data);

		if (p->syntax->set_hint)
			p->syntax->set_hint(p->syntax,
						G_AT_SYNTAX_EXPECT_MULTILINE);
	} else
		have_notify_pdu(p, pdu, &result);

	g_slist_free(result.lines);

error:
	g_free(p->pdu_notify);
	p->pdu_notify = NULL;

	if (pdu)
		g_free(pdu);
}

static char *extract_line(struct at_chat *p, struct ring_buffer *rbuf)
{
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	unsigned int pos = 0;
	unsigned char *buf = ring_buffer_read_ptr(rbuf, pos);
	gboolean in_string = FALSE;
	int strip_front = 0;
	int line_length = 0;
	char *line;

	while (pos < p->read_so_far) {
		if (in_string == FALSE && (*buf == '\r' || *buf == '\n')) {
			if (!line_length)
				strip_front += 1;
			else
				break;
		} else {
			if (*buf == '"')
				in_string = !in_string;

			line_length += 1;
		}

		buf += 1;
		pos += 1;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(rbuf, pos);
	}

	line = g_try_new(char, line_length + 1);
	if (line == NULL) {
		ring_buffer_drain(rbuf, p->read_so_far);
		return NULL;
	}

	ring_buffer_drain(rbuf, strip_front);
	ring_buffer_read(rbuf, line, line_length);
	ring_buffer_drain(rbuf, p->read_so_far - strip_front - line_length);

	line[line_length] = '\0';

	return line;
}

static void new_bytes(struct ring_buffer *rbuf, gpointer user_data)
{
	struct at_chat *p = user_data;
	unsigned int len = ring_buffer_len(rbuf);
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	unsigned char *buf = ring_buffer_read_ptr(rbuf, p->read_so_far);

	GAtSyntaxResult result;

	p->in_read_handler = TRUE;

	while (p->suspended == FALSE && (p->read_so_far < len)) {
		gsize rbytes = MIN(len - p->read_so_far, wrap - p->read_so_far);
		result = p->syntax->feed(p->syntax, (char *)buf, &rbytes);

		buf += rbytes;
		p->read_so_far += rbytes;

		if (p->read_so_far == wrap) {
			buf = ring_buffer_read_ptr(rbuf, p->read_so_far);
			wrap = len;
		}

		if (result == G_AT_SYNTAX_RESULT_UNSURE)
			continue;

		switch (result) {
		case G_AT_SYNTAX_RESULT_LINE:
		case G_AT_SYNTAX_RESULT_MULTILINE:
			have_line(p, extract_line(p, rbuf));
			break;

		case G_AT_SYNTAX_RESULT_PDU:
			have_pdu(p, extract_line(p, rbuf));
			break;

		case G_AT_SYNTAX_RESULT_PROMPT:
			chat_wakeup_writer(p);
			ring_buffer_drain(rbuf, p->read_so_far);
			break;

		default:
			ring_buffer_drain(rbuf, p->read_so_far);
			break;
		}

		len -= p->read_so_far;
		wrap -= p->read_so_far;
		p->read_so_far = 0;
	}

	p->in_read_handler = FALSE;

	if (p->destroyed)
		g_free(p);
}

static void wakeup_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct at_chat *chat = user_data;

	if (ok == FALSE)
		return;

	if (chat->debugf)
		chat->debugf("Finally woke up the modem\n", chat->debug_data);

	g_source_remove(chat->timeout_source);
	chat->timeout_source = 0;
}

static gboolean wakeup_no_response(gpointer user_data)
{
	struct at_chat *chat = user_data;
	struct at_command *cmd = g_queue_peek_head(chat->command_queue);

	if (chat->debugf)
		chat->debugf("Wakeup got no response\n", chat->debug_data);

	if (cmd == NULL)
		return FALSE;

	at_chat_finish_command(chat, FALSE, NULL);

	cmd = at_command_create(0, chat->wakeup, none_prefix, 0,
				NULL, wakeup_cb, chat, NULL, TRUE);
	if (cmd == NULL) {
		chat->timeout_source = 0;
		return FALSE;
	}

	g_queue_push_head(chat->command_queue, cmd);

	return TRUE;
}

static gboolean can_write_data(gpointer data)
{
	struct at_chat *chat = data;
	struct at_command *cmd;
	gsize bytes_written;
	gsize towrite;
	gsize len;
	char *cr;
	gboolean wakeup_first = FALSE;

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
		if (chat->wakeup_timer == NULL) {
			wakeup_first = TRUE;
			chat->wakeup_timer = g_timer_new();

		} else if (g_timer_elapsed(chat->wakeup_timer, NULL) >
				chat->inactivity_time)
			wakeup_first = TRUE;
	}

	if (chat->cmd_bytes_written == 0 && wakeup_first == TRUE) {
		cmd = at_command_create(0, chat->wakeup, none_prefix, 0,
					NULL, wakeup_cb, chat, NULL, TRUE);
		if (cmd == NULL)
			return FALSE;

		g_queue_push_head(chat->command_queue, cmd);

		len = strlen(chat->wakeup);

		chat->timeout_source = g_timeout_add(chat->wakeup_timeout,
						wakeup_no_response, chat);
	}

	towrite = len - chat->cmd_bytes_written;

	cr = strchr(cmd->cmd + chat->cmd_bytes_written, '\r');

	if (cr)
		towrite = cr - (cmd->cmd + chat->cmd_bytes_written) + 1;

#ifdef WRITE_SCHEDULER_DEBUG
	if (towrite > 5)
		towrite = 5;
#endif

	bytes_written = g_at_io_write(chat->io,
					cmd->cmd + chat->cmd_bytes_written,
					towrite);

	if (bytes_written == 0)
		return FALSE;

	chat->cmd_bytes_written += bytes_written;

	if (bytes_written < towrite)
		return TRUE;

	/*
	 * If we're expecting a short prompt, set the hint for all lines
	 * sent to the modem except the last
	 */
	if ((cmd->flags & COMMAND_FLAG_EXPECT_SHORT_PROMPT) &&
			chat->cmd_bytes_written < len &&
			chat->syntax->set_hint)
		chat->syntax->set_hint(chat->syntax,
					G_AT_SYNTAX_EXPECT_SHORT_PROMPT);

	/* Full command submitted, update timer */
	if (chat->wakeup_timer)
		g_timer_start(chat->wakeup_timer);

	return FALSE;
}

static void chat_wakeup_writer(struct at_chat *chat)
{
	g_at_io_set_write_handler(chat->io, can_write_data, chat);
}

static void at_chat_suspend(struct at_chat *chat)
{
	chat->suspended = TRUE;

	g_at_io_set_write_handler(chat->io, NULL, NULL);
	g_at_io_set_read_handler(chat->io, NULL, NULL);
	g_at_io_set_debug(chat->io, NULL, NULL);
}

static void at_chat_resume(struct at_chat *chat)
{
	chat->suspended = FALSE;

	if (g_at_io_get_channel(chat->io) == NULL) {
		io_disconnect(chat);
		return;
	}

	g_at_io_set_disconnect_function(chat->io, io_disconnect, chat);

	g_at_io_set_debug(chat->io, chat->debugf, chat->debug_data);
	g_at_io_set_read_handler(chat->io, new_bytes, chat);

	if (g_queue_get_length(chat->command_queue) > 0)
		chat_wakeup_writer(chat);
}

static void at_chat_unref(struct at_chat *chat)
{
	gboolean is_zero;

	is_zero = g_atomic_int_dec_and_test(&chat->ref_count);

	if (is_zero == FALSE)
		return;

	if (chat->io) {
		at_chat_suspend(chat);
		g_at_io_unref(chat->io);
		chat->io = NULL;
		chat_cleanup(chat);
	}

	if (chat->in_read_handler)
		chat->destroyed = TRUE;
	else
		g_free(chat);
}

static gboolean at_chat_set_disconnect_function(struct at_chat *chat,
						GAtDisconnectFunc disconnect,
						gpointer user_data)
{
	chat->user_disconnect = disconnect;
	chat->user_disconnect_data = user_data;

	return TRUE;
}

static gboolean at_chat_set_debug(struct at_chat *chat,
					GAtDebugFunc func, gpointer user_data)
{

	chat->debugf = func;
	chat->debug_data = user_data;

	if (chat->io)
		g_at_io_set_debug(chat->io, func, user_data);

	return TRUE;
}

static gboolean at_chat_set_wakeup_command(struct at_chat *chat,
						const char *cmd,
						unsigned int timeout,
						unsigned int msec)
{
	if (chat->wakeup)
		g_free(chat->wakeup);

	chat->wakeup = g_strdup(cmd);
	chat->inactivity_time = (gdouble)msec / 1000;
	chat->wakeup_timeout = timeout;

	return TRUE;
}

static guint at_chat_send_common(struct at_chat *chat, guint gid,
					const char *cmd,
					const char **prefix_list,
					guint flags,
					GAtNotifyFunc listing,
					GAtResultFunc func,
					gpointer user_data,
					GDestroyNotify notify)
{
	struct at_command *c;

	if (chat == NULL || chat->command_queue == NULL)
		return 0;

	c = at_command_create(gid, cmd, prefix_list, flags, listing, func,
				user_data, notify, FALSE);
	if (c == NULL)
		return 0;

	c->id = chat->next_cmd_id++;

	g_queue_push_tail(chat->command_queue, c);

	if (g_queue_get_length(chat->command_queue) == 1)
		chat_wakeup_writer(chat);

	return c->id;
}

static struct at_notify *at_notify_create(struct at_chat *chat,
						const char *prefix,
						gboolean pdu)
{
	struct at_notify *notify;
	char *key;

	key = g_strdup(prefix);
	if (key == NULL)
		return 0;

	notify = g_try_new0(struct at_notify, 1);
	if (notify == NULL) {
		g_free(key);
		return 0;
	}

	notify->pdu = pdu;

	g_hash_table_insert(chat->notify_list, key, notify);

	return notify;
}

static gboolean at_chat_cancel(struct at_chat *chat, guint group, guint id)
{
	GList *l;
	struct at_command *c;

	if (chat->command_queue == NULL)
		return FALSE;

	l = g_queue_find_custom(chat->command_queue, GUINT_TO_POINTER(id),
				at_command_compare_by_id);

	if (l == NULL)
		return FALSE;

	c = l->data;

	if (c->gid != group)
		return FALSE;

	if (c == g_queue_peek_head(chat->command_queue) &&
			chat->cmd_bytes_written > 0) {
		/* We can't actually remove it since it is most likely
		 * already in progress, just null out the callback
		 * so it won't be called
		 */
		c->callback = NULL;
	} else {
		at_command_destroy(c);
		g_queue_remove(chat->command_queue, c);
	}

	return TRUE;
}

static gboolean at_chat_cancel_group(struct at_chat *chat, guint group)
{
	int n = 0;
	struct at_command *c;

	if (chat->command_queue == NULL)
		return FALSE;

	while ((c = g_queue_peek_nth(chat->command_queue, n)) != NULL) {
		if (c->id == 0 || c->gid != group) {
			n += 1;
			continue;
		}

		if (n == 0 && chat->cmd_bytes_written > 0) {
			c->callback = NULL;
			n += 1;
			continue;
		}

		at_command_destroy(c);
		g_queue_remove(chat->command_queue, c);
	}

	return TRUE;
}

static guint at_chat_register(struct at_chat *chat, guint group,
				const char *prefix, GAtNotifyFunc func,
				gboolean expect_pdu, gpointer user_data,
				GDestroyNotify destroy_notify)
{
	struct at_notify *notify;
	struct at_notify_node *node;

	if (chat->notify_list == NULL)
		return 0;

	if (func == NULL)
		return 0;

	if (prefix == NULL || strlen(prefix) == 0)
		return 0;

	notify = g_hash_table_lookup(chat->notify_list, prefix);

	if (notify == NULL)
		notify = at_notify_create(chat, prefix, expect_pdu);

	if (notify == NULL || notify->pdu != expect_pdu)
		return 0;

	node = g_try_new0(struct at_notify_node, 1);
	if (node == NULL)
		return 0;

	node->id = chat->next_notify_id++;
	node->gid = group;
	node->callback = func;
	node->user_data = user_data;
	node->notify = destroy_notify;

	notify->nodes = g_slist_prepend(notify->nodes, node);

	return node->id;
}

static gboolean at_chat_unregister(struct at_chat *chat, gboolean mark_only,
					guint group, guint id)
{
	GHashTableIter iter;
	struct at_notify *notify;
	struct at_notify_node *node;
	gpointer key, value;
	GSList *l;

	if (chat->notify_list == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, chat->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		notify = value;

		l = g_slist_find_custom(notify->nodes, GUINT_TO_POINTER(id),
					at_notify_node_compare_by_id);

		if (l == NULL)
			continue;

		node = l->data;

		if (node->gid != group)
			return FALSE;

		if (mark_only) {
			node->destroyed = TRUE;
			return TRUE;
		}

		at_notify_node_destroy(node, NULL);
		notify->nodes = g_slist_remove(notify->nodes, node);

		if (notify->nodes == NULL)
			g_hash_table_iter_remove(&iter);

		return TRUE;
	}

	return FALSE;
}

static gboolean node_compare_by_group(struct at_notify_node *node,
					gpointer userdata)
{
	guint group = GPOINTER_TO_UINT(userdata);

	if (node->gid == group)
		return TRUE;

	return FALSE;
}

static struct at_chat *create_chat(GIOChannel *channel, GIOFlags flags,
					GAtSyntax *syntax)
{
	struct at_chat *chat;

	if (channel == NULL)
		return NULL;

	if (syntax == NULL)
		return NULL;

	chat = g_try_new0(struct at_chat, 1);
	if (chat == NULL)
		return chat;

	chat->ref_count = 1;
	chat->next_cmd_id = 1;
	chat->next_notify_id = 1;
	chat->debugf = NULL;

	if (flags & G_IO_FLAG_NONBLOCK)
		chat->io = g_at_io_new(channel);
	else
		chat->io = g_at_io_new_blocking(channel);

	if (chat->io == NULL)
		goto error;

	g_at_io_set_disconnect_function(chat->io, io_disconnect, chat);

	chat->command_queue = g_queue_new();
	if (chat->command_queue == NULL)
		goto error;

	chat->notify_list = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, at_notify_destroy);

	g_at_io_set_read_handler(chat->io, new_bytes, chat);

	chat->syntax = g_at_syntax_ref(syntax);

	return chat;

error:
	g_at_io_unref(chat->io);

	if (chat->command_queue)
		g_queue_free(chat->command_queue);

	if (chat->notify_list)
		g_hash_table_destroy(chat->notify_list);

	g_free(chat);
	return NULL;
}

static GAtChat *g_at_chat_new_common(GIOChannel *channel, GIOFlags flags,
					GAtSyntax *syntax)
{
	GAtChat *chat;

	chat = g_try_new0(GAtChat, 1);
	if (chat == NULL)
		return NULL;

	chat->parent = create_chat(channel, flags, syntax);
	if (chat->parent == NULL) {
		g_free(chat);
		return NULL;
	}

	chat->group = chat->parent->next_gid++;
	chat->ref_count = 1;

	return chat;
}

GAtChat *g_at_chat_new(GIOChannel *channel, GAtSyntax *syntax)
{
	return g_at_chat_new_common(channel, G_IO_FLAG_NONBLOCK, syntax);
}

GAtChat *g_at_chat_new_blocking(GIOChannel *channel, GAtSyntax *syntax)
{
	return g_at_chat_new_common(channel, 0, syntax);
}

GAtChat *g_at_chat_clone(GAtChat *clone)
{
	GAtChat *chat;

	if (clone == NULL)
		return NULL;

	chat = g_try_new0(GAtChat, 1);
	if (chat == NULL)
		return NULL;

	chat->parent = clone->parent;
	chat->group = chat->parent->next_gid++;
	chat->ref_count = 1;
	g_atomic_int_inc(&chat->parent->ref_count);

	if (clone->slave != NULL)
		chat->slave = g_at_chat_clone(clone->slave);

	return chat;
}

GAtChat *g_at_chat_set_slave(GAtChat *chat, GAtChat *slave)
{
	if (chat == NULL)
		return NULL;

	if (chat->slave != NULL)
		g_at_chat_unref(chat->slave);

	if (slave != NULL)
		chat->slave = g_at_chat_ref(slave);
	else
		chat->slave = NULL;

	return chat->slave;
}

GAtChat *g_at_chat_get_slave(GAtChat *chat)
{
	if (chat == NULL)
		return NULL;

	return chat->slave;
}

GIOChannel *g_at_chat_get_channel(GAtChat *chat)
{
	if (chat == NULL || chat->parent->io == NULL)
		return NULL;

	return g_at_io_get_channel(chat->parent->io);
}

GAtIO *g_at_chat_get_io(GAtChat *chat)
{
	if (chat == NULL)
		return NULL;

	return chat->parent->io;
}

GAtChat *g_at_chat_ref(GAtChat *chat)
{
	if (chat == NULL)
		return NULL;

	g_atomic_int_inc(&chat->ref_count);

	return chat;
}

void g_at_chat_suspend(GAtChat *chat)
{
	if (chat == NULL)
		return;

	at_chat_suspend(chat->parent);
}

void g_at_chat_resume(GAtChat *chat)
{
	if (chat == NULL)
		return;

	at_chat_resume(chat->parent);
}

void g_at_chat_unref(GAtChat *chat)
{
	gboolean is_zero;

	if (chat == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&chat->ref_count);

	if (is_zero == FALSE)
		return;

	if (chat->slave != NULL)
		g_at_chat_unref(chat->slave);

	at_chat_cancel_group(chat->parent, chat->group);
	g_at_chat_unregister_all(chat);
	at_chat_unref(chat->parent);

	g_free(chat);
}

gboolean g_at_chat_set_disconnect_function(GAtChat *chat,
			GAtDisconnectFunc disconnect, gpointer user_data)
{
	if (chat == NULL || chat->group != 0)
		return FALSE;

	return at_chat_set_disconnect_function(chat->parent, disconnect,
						user_data);
}

gboolean g_at_chat_set_debug(GAtChat *chat,
				GAtDebugFunc func, gpointer user_data)
{

	if (chat == NULL || chat->group != 0)
		return FALSE;

	return at_chat_set_debug(chat->parent, func, user_data);
}

void g_at_chat_add_terminator(GAtChat *chat, char *terminator,
					int len, gboolean success)
{
	if (chat == NULL || chat->group != 0)
		return;

	at_chat_add_terminator(chat->parent, terminator, len, success);
}

void g_at_chat_blacklist_terminator(GAtChat *chat,
						GAtChatTerminator terminator)
{
	if (chat == NULL || chat->group != 0)
		return;

	at_chat_blacklist_terminator(chat->parent, terminator);
}

gboolean g_at_chat_set_wakeup_command(GAtChat *chat, const char *cmd,
					unsigned int timeout, unsigned int msec)
{
	if (chat == NULL || chat->group != 0)
		return FALSE;

	return at_chat_set_wakeup_command(chat->parent, cmd, timeout, msec);
}

guint g_at_chat_send(GAtChat *chat, const char *cmd,
			const char **prefix_list, GAtResultFunc func,
			gpointer user_data, GDestroyNotify notify)
{
	return at_chat_send_common(chat->parent, chat->group,
					cmd, prefix_list, 0, NULL,
					func, user_data, notify);
}

guint g_at_chat_send_listing(GAtChat *chat, const char *cmd,
				const char **prefix_list,
				GAtNotifyFunc listing, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify)
{
	if (listing == NULL)
		return 0;

	return at_chat_send_common(chat->parent, chat->group,
					cmd, prefix_list, 0,
					listing, func, user_data, notify);
}

guint g_at_chat_send_pdu_listing(GAtChat *chat, const char *cmd,
				const char **prefix_list,
				GAtNotifyFunc listing, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify)
{
	if (listing == NULL)
		return 0;

	return at_chat_send_common(chat->parent, chat->group,
					cmd, prefix_list,
					COMMAND_FLAG_EXPECT_PDU,
					listing, func, user_data, notify);
}

guint g_at_chat_send_and_expect_short_prompt(GAtChat *chat, const char *cmd,
						const char **prefix_list,
						GAtResultFunc func,
						gpointer user_data,
						GDestroyNotify notify)
{
	return at_chat_send_common(chat->parent, chat->group,
					cmd, prefix_list,
					COMMAND_FLAG_EXPECT_SHORT_PROMPT,
					NULL, func, user_data, notify);
}

gboolean g_at_chat_cancel(GAtChat *chat, guint id)
{
	/* We use id 0 for wakeup commands */
	if (chat == NULL || id == 0)
		return FALSE;

	return at_chat_cancel(chat->parent, chat->group, id);
}

gboolean g_at_chat_cancel_all(GAtChat *chat)
{
	if (chat == NULL)
		return FALSE;

	return at_chat_cancel_group(chat->parent, chat->group);
}

guint g_at_chat_register(GAtChat *chat, const char *prefix,
				GAtNotifyFunc func, gboolean expect_pdu,
				gpointer user_data,
				GDestroyNotify destroy_notify)
{
	if (chat == NULL)
		return 0;

	return at_chat_register(chat->parent, chat->group, prefix,
				func, expect_pdu, user_data, destroy_notify);
}

gboolean g_at_chat_unregister(GAtChat *chat, guint id)
{
	if (chat == NULL)
		return FALSE;

	return at_chat_unregister(chat->parent, chat->parent->in_notify,
					chat->group, id);
}

gboolean g_at_chat_unregister_all(GAtChat *chat)
{
	if (chat == NULL)
		return FALSE;

	return at_chat_unregister_all(chat->parent,
					chat->parent->in_notify,
					node_compare_by_group,
					GUINT_TO_POINTER(chat->group));
}
