/*
 *
 *  AT server library with GLib integration
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

#include <glib.h>

#include "ringbuffer.h"
#include "gatserver.h"
#include "gatio.h"

#define BUF_SIZE 4096
/* <cr><lf> + the max length of information text + <cr><lf> */
#define MAX_TEXT_SIZE 2052
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
	char s0;			/* set by S0=<val> */
	char s3;			/* set by S3=<val> */
	char s4;			/* set by S4=<val> */
	char s5;			/* set by S5=<val> */
	int s6;				/* set by S6=<val> */
	int s7;				/* set by S7=<val> */
	int s8;				/* set by S8=<val> */
	int s10;			/* set by S10=<val> */
	gboolean echo;			/* set by E<val> */
	gboolean quiet;			/* set by Q<val> */
	gboolean is_v1;			/* set by V<val>, v0 or v1 */
	int res_format;			/* set by X<val> */
	int c109;			/* set by &C<val> */
	int c108;			/* set by &D<val> */
	char l;				/* set by L<val> */
	char m;				/* set by M<val> */
	char dial_mode;			/* set by P or T */
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
	GAtIO *io;				/* Server IO */
	guint read_so_far;			/* Number of bytes processed */
	GAtDisconnectFunc user_disconnect;	/* User disconnect func */
	gpointer user_disconnect_data;		/* User disconnect data */
	GAtDebugFunc debugf;			/* Debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	GHashTable *command_list;		/* List of AT commands */
	GQueue *write_queue;			/* Write buffer queue */
	guint max_read_attempts;		/* Max reads per select */
	enum ParserState parser_state;
	gboolean destroyed;			/* Re-entrancy guard */
	char *last_line;			/* Last read line */
	unsigned int cur_pos;			/* Where we are on the line */
	GAtServerResult last_result;
	gboolean final_sent;
	gboolean final_async;
	gboolean in_read_handler;
	GAtServerFinishFunc finishf;		/* Callback when cmd finishes */
	gpointer finish_data;			/* Finish func data */
};

static void server_wakeup_writer(GAtServer *server);
static void server_parse_line(GAtServer *server);

static struct ring_buffer *allocate_next(GAtServer *server)
{
	struct ring_buffer *buf = ring_buffer_new(BUF_SIZE);

	if (buf == NULL)
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

	server_wakeup_writer(server);
}

static void send_result_common(GAtServer *server, const char *result)

{
	struct v250_settings v250 = server->v250;
	char buf[MAX_TEXT_SIZE + 1];
	char t = v250.s3;
	char r = v250.s4;
	unsigned int len;

	if (v250.quiet)
		return;

	if (result == NULL)
		return;

	if (strlen(result) > 2048)
		return;

	if (v250.is_v1)
		len = sprintf(buf, "%c%c%s%c%c", t, r, result, t, r);
	else
		len = sprintf(buf, "%s%c", result, t);

	send_common(server, buf, len);
}

static inline void send_final_common(GAtServer *server, const char *result)
{
	send_result_common(server, result);
	server->final_async = FALSE;

	if (server->finishf)
		server->finishf(server, server->finish_data);
}

static inline void send_final_numeric(GAtServer *server, GAtServerResult result)
{
	char buf[1024];

	if (server->v250.is_v1)
		sprintf(buf, "%s", server_result_to_string(result));
	else
		sprintf(buf, "%u", (unsigned int)result);

	send_final_common(server, buf);
}

void g_at_server_send_final(GAtServer *server, GAtServerResult result)
{
	if (server == NULL)
		return;

	if (server->final_sent != FALSE)
		return;

	server->final_sent = TRUE;
	server->last_result = result;

	if (result == G_AT_SERVER_RESULT_OK) {
		if (server->final_async)
			server_parse_line(server);

		return;
	}

	send_final_numeric(server, result);
}

void g_at_server_send_ext_final(GAtServer *server, const char *result)
{
	server->final_sent = TRUE;
	server->last_result = G_AT_SERVER_RESULT_EXT_ERROR;
	send_final_common(server, result);
}

void g_at_server_send_intermediate(GAtServer *server, const char *result)
{
	send_result_common(server, result);
}

void g_at_server_send_unsolicited(GAtServer *server, const char *result)
{
	send_result_common(server, result);
}

void g_at_server_send_info(GAtServer *server, const char *line, gboolean last)
{
	char buf[MAX_TEXT_SIZE + 1];
	char t = server->v250.s3;
	char r = server->v250.s4;
	unsigned int len;

	if (strlen(line) > 2048)
		return;

	if (last)
		len = sprintf(buf, "%c%c%s%c%c", t, r, line, t, r);
	else
		len = sprintf(buf, "%c%c%s", t, r, line);

	send_common(server, buf, len);
}

static gboolean get_result_value(GAtServer *server, GAtResult *result,
						int min, int max, int *value)
{
	GAtResultIter iter;
	int val;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, ""))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, &val))
		return FALSE;

	if (val < min || val > max)
		return FALSE;

	if (value)
		*value = val;

	return TRUE;
}

static void v250_settings_create(struct v250_settings *v250)
{
	v250->s0 = 0;
	v250->s3 = '\r';
	v250->s4 = '\n';
	v250->s5 = '\b';
	v250->s6 = 2;
	v250->s7 = 50;
	v250->s8 = 2;
	v250->s10 = 2;
	v250->echo = TRUE;
	v250->quiet = FALSE;
	v250->is_v1 = TRUE;
	v250->res_format = 0;
	v250->c109 = 1;
	v250->c108 = 0;
	v250->l = 0;
	v250->m = 1;
	v250->dial_mode = 'T';
}

static void s_template_cb(GAtServerRequestType type, GAtResult *result,
					GAtServer *server, char *sreg,
					const char *prefix, int min, int max)
{
	char buf[20];
	int tmp;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		if (!get_result_value(server, result, min, max, &tmp)) {
			g_at_server_send_final(server,
						G_AT_SERVER_RESULT_ERROR);
			return;
		}

		*sreg = tmp;
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		tmp = *sreg;
		sprintf(buf, "%03d", tmp);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "%s: (%d-%d)", prefix, min, max);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void at_s0_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	s_template_cb(type, result, server, &server->v250.s0, "S0", 0, 7);
}

static void at_s3_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	s_template_cb(type, result, server, &server->v250.s3, "S3", 0, 127);
}

static void at_s4_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	s_template_cb(type, result, server, &server->v250.s4, "S4", 0, 127);
}

static void at_s5_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	s_template_cb(type, result, server, &server->v250.s5, "S5", 0, 127);
}

static void at_l_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	s_template_cb(type, result, server, &server->v250.l, "L", 0, 3);
}

static void at_m_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	s_template_cb(type, result, server, &server->v250.m, "M", 0, 2);
}

static void at_template_cb(GAtServerRequestType type, GAtResult *result,
					GAtServer *server, int *value,
					const char *prefix,
					int min, int max, int deftval)
{
	char buf[20];
	int tmp;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		if (!get_result_value(server, result, min, max, &tmp)) {
			g_at_server_send_final(server,
						G_AT_SERVER_RESULT_ERROR);
			return;
		}

		*value = tmp;
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		tmp = *value;
		sprintf(buf, "%s: %d", prefix, tmp);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "%s: (%d-%d)", prefix, min, max);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		*value = deftval;
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void at_e_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.echo, "E", 0, 1, 1);
}

static void at_q_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.quiet, "Q", 0, 1, 0);
}

static void at_v_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.is_v1, "V", 0, 1, 1);
}

static void at_x_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.res_format,
			"X", 0, 4, 4);
}

static void at_s6_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.s6, "S6", 0, 1, 1);
}

static void at_s7_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.s7, "S7", 1, 255, 50);
}

static void at_s8_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.s8, "S8", 1, 255, 2);
}

static void at_s10_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.s10, "S10", 1, 254, 2);
}

static void at_c109_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.c109, "&C", 0, 1, 1);
}

static void at_c108_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	at_template_cb(type, result, server, &server->v250.c108, "&D", 0, 2, 2);
}

/* According to ITU V.250 6.3.2 and 6.3.3: "Implementation of this command
 * is mandatory; however, if DTMF or pulse dialling is not implemented,
 * this command will have no effect"
 */
static void at_t_cb(GAtServer *server, GAtServerRequestType type,
					GAtResult *result, gpointer user_data)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		server->v250.dial_mode = 'T';
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void at_p_cb(GAtServer *server, GAtServerRequestType type,
					GAtResult *result, gpointer user_data)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		server->v250.dial_mode = 'P';
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void at_f_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		if (!get_result_value(server, result, 0, 0, NULL)) {
			g_at_server_send_final(server,
						G_AT_SERVER_RESULT_ERROR);
			return;
		}
		/* intentional fallback here */

	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		/* default behavior on AT&F same as ATZ */
		v250_settings_create(&server->v250);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void at_z_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		v250_settings_create(&server->v250);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
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

	node->notify(server, type, &result, node->user_data);

	g_slist_free(result.lines);
}

static unsigned int parse_extended_command(GAtServer *server, char *buf)
{
	const char *valid_extended_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						"0123456789!%-./:_";
	const char *separators = ";?=";
	unsigned int prefix_len, i;
	gboolean in_string = FALSE;
	gboolean seen_equals = FALSE;
	char prefix[18]; /* According to V250, 5.4.1 */
	GAtServerRequestType type;
	char tmp;
	unsigned int cmd_start;

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
	cmd_start = prefix_len;

	/* Continue until we hit eol or ';' */
	while (buf[i] && !(buf[i] == ';' && in_string == FALSE)) {
		if (buf[i] == '"') {
			in_string = !in_string;
			goto next;
		}

		if (in_string == TRUE)
			goto next;

		if (buf[i] == '?') {
			if (seen_equals && buf[i-1] != '=')
				return 0;

			if (buf[i + 1] != '\0' && buf[i + 1] != ';')
				return 0;

			type = G_AT_SERVER_REQUEST_TYPE_QUERY;
			cmd_start += 1;

			if (seen_equals)
				type = G_AT_SERVER_REQUEST_TYPE_SUPPORT;
		} else if (buf[i] == '=') {
			if (seen_equals)
				return 0;

			seen_equals = TRUE;
			type = G_AT_SERVER_REQUEST_TYPE_SET;
			cmd_start += 1;
		}

next:
		i++;
	}

	/* We can scratch in this buffer, so mark ';' as null */
	tmp = buf[i];
	buf[i] = '\0';
	at_command_notify(server, buf + cmd_start, prefix, type);
	buf[i] = tmp;

	/* Also consume the terminating null */
	return i + 1;
}

static int get_basic_prefix_size(const char *buf)
{
	if (g_ascii_isalpha(buf[0])) {
		if (g_ascii_toupper(buf[0]) == 'S') {
			int size;

			/* V.250 5.3.2 'S' command follows with a parameter
			 * number.
			 */
			for (size = 1; g_ascii_isdigit(buf[size]); size++)
				;

			/*
			 * Do some basic sanity checking, don't accept 00, 01,
			 * etc or empty S values
			 */
			if (size == 1)
				return 0;

			if (size > 2 && buf[1] == '0')
				return 0;

			return size;
		}

		/* All other cases it is a simple 1 character prefix */
		return 1;
	}

	if (buf[0] == '&') {
		if (g_ascii_isalpha(buf[1]) == FALSE)
			return 0;

		return 2;
	}

	return 0;
}

static unsigned int parse_basic_command(GAtServer *server, char *buf)
{
	gboolean seen_equals = FALSE;
	char prefix[4], tmp;
	unsigned int i, prefix_size;
	GAtServerRequestType type;
	unsigned int cmd_start;

	prefix_size = get_basic_prefix_size(buf);
	if (prefix_size == 0)
		return 0;

	i = prefix_size;
	prefix[0] = g_ascii_toupper(buf[0]);
	cmd_start = prefix_size;

	if (prefix[0] == 'D') {
		type = G_AT_SERVER_REQUEST_TYPE_SET;

		/* All characters appearing on the same line, up to a
		 * semicolon character (IA5 3/11) or the end of the
		 * command line is the part of the call.
		 */
		while (buf[i] != '\0' && buf[i] != ';')
			i += 1;

		if (buf[i] == ';')
			i += 1;

		goto done;
	}

	type = G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY;

	/* Match '?', '=',  '=?' and '=xxx' */
	if (buf[i] == '=') {
		seen_equals = TRUE;
		i += 1;
		cmd_start += 1;
	}

	if (buf[i] == '?') {
		i += 1;
		cmd_start += 1;

		if (seen_equals)
			type = G_AT_SERVER_REQUEST_TYPE_SUPPORT;
		else
			type = G_AT_SERVER_REQUEST_TYPE_QUERY;
	} else {
		int before = i;

		/* V.250 5.3.1 The subparameter (if any) are all digits */
		while (g_ascii_isdigit(buf[i]))
			i++;

		if (i - before > 0)
			type = G_AT_SERVER_REQUEST_TYPE_SET;
	}

done:
	if (prefix_size <= 3) {
		memcpy(prefix + 1, buf + 1, prefix_size - 1);
		prefix[prefix_size] = '\0';

		tmp = buf[i];
		buf[i] = '\0';
		at_command_notify(server, buf + cmd_start, prefix, type);
		buf[i] = tmp;
	} else /* Handle S-parameter with 100+ */
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);

	/*
	 * Commands like ATA, ATZ cause the remainder linevto be ignored.
	 * In GSM/UMTS the ATD uses the separator ';' character as a voicecall
	 * modifier, so we ignore everything coming after that character
	 * as well.
	 */
	if (prefix[0] == 'A' || prefix[0] == 'Z' || prefix[0] == 'D')
		return strlen(buf);

	/* Consume the seperator ';' */
	if (buf[i] == ';')
		i += 1;

	return i;
}

static void server_parse_line(GAtServer *server)
{
	char *line = server->last_line;
	unsigned int pos = server->cur_pos;
	unsigned int len = strlen(line);

	while (pos < len) {
		unsigned int consumed;

		server->final_sent = FALSE;
		server->final_async = FALSE;

		if (is_extended_command_prefix(line[pos]))
			consumed = parse_extended_command(server, line + pos);
		else
			consumed = parse_basic_command(server, line + pos);

		if (consumed == 0) {
			g_at_server_send_final(server,
						G_AT_SERVER_RESULT_ERROR);
			return;
		}

		pos += consumed;
		server->cur_pos = pos;

		/*
		 * We wait the callback until it finished processing
		 * the command and called the send_final.
		 */
		if (server->final_sent == FALSE) {
			server->final_async = TRUE;
			return;
		}

		if (server->last_result != G_AT_SERVER_RESULT_OK)
			return;
	}

	send_final_numeric(server, G_AT_SERVER_RESULT_OK);
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
			/* Detect CR or HDLC frame marker flag */
			if (byte == s3 || byte == '~') {
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

static char *extract_line(GAtServer *p, struct ring_buffer *rbuf)
{
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	unsigned int pos = 0;
	unsigned char *buf = ring_buffer_read_ptr(rbuf, pos);
	int strip_front = 0;
	int line_length = 0;
	gboolean in_string = FALSE;
	char s3 = p->v250.s3;
	char s5 = p->v250.s5;
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
			buf = ring_buffer_read_ptr(rbuf, pos);
	}

	/* We will strip AT and S3 */
	line_length -= 3;

	line = g_try_new(char, line_length + 1);
	if (line == NULL) {
		ring_buffer_drain(rbuf, p->read_so_far);
		return NULL;
	}

	/* Strip leading whitespace + AT */
	ring_buffer_drain(rbuf, strip_front + 2);

	pos = 0;
	i = 0;
	wrap = ring_buffer_len_no_wrap(rbuf);
	buf = ring_buffer_read_ptr(rbuf, pos);

	while (pos < (p->read_so_far - strip_front - 2)) {
		if (*buf == '"')
			in_string = !in_string;

		if (*buf == s5) {
			if (i != 0)
				i -= 1;
		} else if ((*buf == ' ' || *buf == '\t') && in_string == FALSE)
			; /* Skip */
		else if (*buf != s3)
			line[i++] = *buf;

		buf += 1;
		pos += 1;

		if (pos == wrap)
			buf = ring_buffer_read_ptr(rbuf, pos);
	}

	/* Strip S3 */
	ring_buffer_drain(rbuf, p->read_so_far - strip_front - 2);

	line[i] = '\0';

	return line;
}

static void new_bytes(struct ring_buffer *rbuf, gpointer user_data)
{
	GAtServer *p = user_data;
	unsigned int len = ring_buffer_len(rbuf);
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	unsigned char *buf = ring_buffer_read_ptr(rbuf, p->read_so_far);
	enum ParserResult result;

	/* We do not support command abortion, so ignore input */
	if (p->final_async) {
		ring_buffer_drain(rbuf, len);
		return;
	}

	p->in_read_handler = TRUE;

	while (p->io && (p->read_so_far < len)) {
		gsize rbytes = MIN(len - p->read_so_far, wrap - p->read_so_far);
		result = server_feed(p, (char *)buf, &rbytes);

		if (p->v250.echo)
			send_common(p, (char *)buf, rbytes);

		buf += rbytes;
		p->read_so_far += rbytes;

		if (p->read_so_far == wrap) {
			buf = ring_buffer_read_ptr(rbuf, p->read_so_far);
			wrap = len;
		}

		switch (result) {
		case PARSER_RESULT_UNSURE:
			continue;

		case PARSER_RESULT_EMPTY_COMMAND:
			/*
			 * According to section 5.2.4 and 5.6 of V250,
			 * Empty commands must be OK by the DCE
			 */
			g_at_server_send_final(p, G_AT_SERVER_RESULT_OK);
			ring_buffer_drain(rbuf, p->read_so_far);
			break;

		case PARSER_RESULT_COMMAND:
		{
			g_free(p->last_line);

			p->last_line = extract_line(p, rbuf);
			p->cur_pos = 0;

			if (p->last_line)
				server_parse_line(p);
			else
				g_at_server_send_final(p,
						G_AT_SERVER_RESULT_ERROR);
			break;
		}

		case PARSER_RESULT_REPEAT_LAST:
			p->cur_pos = 0;
			ring_buffer_drain(rbuf, p->read_so_far);

			if (p->last_line)
				server_parse_line(p);
			else
				g_at_server_send_final(p,
						G_AT_SERVER_RESULT_OK);
			break;

		case PARSER_RESULT_GARBAGE:
			ring_buffer_drain(rbuf, p->read_so_far);
			break;
		}

		len -= p->read_so_far;
		wrap -= p->read_so_far;
		p->read_so_far = 0;

		/*
		 * Handle situations where we receive two command lines in
		 * one read, which should not be possible (and implies the
		 * earlier command should be canceled.
		 *
		 * e.g. AT+CMD1\rAT+CMD2
		 */
		if (result != PARSER_RESULT_GARBAGE) {
			ring_buffer_drain(rbuf, len);
			break;
		}
	}

	p->in_read_handler = FALSE;

	if (p->destroyed)
		g_free(p);
}

static gboolean can_write_data(gpointer data)
{
	GAtServer *server = data;
	gsize bytes_written;
	gsize towrite;
	struct ring_buffer *write_buf;
	unsigned char *buf;
#ifdef WRITE_SCHEDULER_DEBUG
	int limiter;
#endif

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

	bytes_written = g_at_io_write(server->io,
			(char *)buf,
#ifdef WRITE_SCHEDULER_DEBUG
			limiter
#else
			towrite
#endif
			);

	if (bytes_written == 0)
		return FALSE;

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
	/* Cleanup pending data to write */
	write_queue_free(server->write_queue);

	g_hash_table_destroy(server->command_list);
	server->command_list = NULL;

	g_free(server->last_line);

	g_at_io_unref(server->io);
	server->io = NULL;
}

static void io_disconnect(gpointer user_data)
{
	GAtServer *server = user_data;

	g_at_server_cleanup(server);

	if (server->user_disconnect)
		server->user_disconnect(server->user_disconnect_data);
}

static void server_wakeup_writer(GAtServer *server)
{
	g_at_io_set_write_handler(server->io, can_write_data, server);
}

static void at_notify_node_destroy(gpointer data)
{
	struct at_command *node = data;

	if (node->destroy_notify)
		node->destroy_notify(node->user_data);

	g_free(node);
}

static void basic_command_register(GAtServer *server)
{
	g_at_server_register(server, "S0", at_s0_cb, NULL, NULL);
	g_at_server_register(server, "S3", at_s3_cb, NULL, NULL);
	g_at_server_register(server, "S4", at_s4_cb, NULL, NULL);
	g_at_server_register(server, "S5", at_s5_cb, NULL, NULL);
	g_at_server_register(server, "E", at_e_cb, NULL, NULL);
	g_at_server_register(server, "Q", at_q_cb, NULL, NULL);
	g_at_server_register(server, "V", at_v_cb, NULL, NULL);
	g_at_server_register(server, "X", at_x_cb, NULL, NULL);
	g_at_server_register(server, "S6", at_s6_cb, NULL, NULL);
	g_at_server_register(server, "S7", at_s7_cb, NULL, NULL);
	g_at_server_register(server, "S8", at_s8_cb, NULL, NULL);
	g_at_server_register(server, "S10", at_s10_cb, NULL, NULL);
	g_at_server_register(server, "&C", at_c109_cb, NULL, NULL);
	g_at_server_register(server, "&D", at_c108_cb, NULL, NULL);
	g_at_server_register(server, "Z", at_z_cb, NULL, NULL);
	g_at_server_register(server, "&F", at_f_cb, NULL, NULL);
	g_at_server_register(server, "L", at_l_cb, NULL, NULL);
	g_at_server_register(server, "M", at_m_cb, NULL, NULL);
	g_at_server_register(server, "T", at_t_cb, NULL, NULL);
	g_at_server_register(server, "P", at_p_cb, NULL, NULL);
}

GAtServer *g_at_server_new(GIOChannel *io)
{
	GAtServer *server;

	if (io == NULL)
		return NULL;

	server = g_try_new0(GAtServer, 1);
	if (server == NULL)
		return NULL;

	server->ref_count = 1;
	v250_settings_create(&server->v250);
	server->io = g_at_io_new(io);
	if (!server->io)
		goto error;

	g_at_io_set_disconnect_function(server->io, io_disconnect, server);

	server->command_list = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free,
							at_notify_node_destroy);

	server->write_queue = g_queue_new();
	if (!server->write_queue)
		goto error;

	if (allocate_next(server) == NULL)
		goto error;

	server->max_read_attempts = 3;

	g_at_io_set_read_handler(server->io, new_bytes, server);

	basic_command_register(server);

	return server;

error:
	g_at_io_unref(server->io);

	if (server->command_list)
		g_hash_table_destroy(server->command_list);

	if (server->write_queue)
		write_queue_free(server->write_queue);

	if (server)
		g_free(server);

	return NULL;
}

GIOChannel *g_at_server_get_channel(GAtServer *server)
{
	if (server == NULL || server->io == NULL)
		return NULL;

	return g_at_io_get_channel(server->io);
}

GAtIO *g_at_server_get_io(GAtServer *server)
{
	if (server == NULL)
		return NULL;

	return server->io;
}

GAtServer *g_at_server_ref(GAtServer *server)
{
	if (server == NULL)
		return NULL;

	g_atomic_int_inc(&server->ref_count);

	return server;
}

void g_at_server_suspend(GAtServer *server)
{
	if (server == NULL)
		return;

	g_at_io_set_write_handler(server->io, NULL, NULL);
	g_at_io_set_read_handler(server->io, NULL, NULL);

	g_at_io_set_debug(server->io, NULL, NULL);
}

void g_at_server_resume(GAtServer *server)
{
	if (server == NULL)
		return;

	if (g_at_io_get_channel(server->io) == NULL) {
		io_disconnect(server);
		return;
	}

	g_at_io_set_disconnect_function(server->io, io_disconnect, server);

	g_at_io_set_debug(server->io, server->debugf, server->debug_data);
	g_at_io_set_read_handler(server->io, new_bytes, server);

	if (g_queue_get_length(server->write_queue) > 0)
		server_wakeup_writer(server);
}

void g_at_server_unref(GAtServer *server)
{
	gboolean is_zero;

	if (server == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&server->ref_count);

	if (is_zero == FALSE)
		return;

	if (server->io) {
		g_at_server_suspend(server);
		g_at_server_cleanup(server);
	}

	g_at_server_shutdown(server);

	/* glib delays the destruction of the watcher until it exits, this
	 * means we can't free the data just yet, even though we've been
	 * destroyed already.  We have to wait until the read_watcher
	 * destroy function gets called
	 */
	if (server->in_read_handler)
		server->destroyed = TRUE;
	else
		g_free(server);
}

gboolean g_at_server_shutdown(GAtServer *server)
{
	if (server == NULL)
		return FALSE;

	/* Don't trigger user disconnect on shutdown */
	server->user_disconnect = NULL;
	server->user_disconnect_data = NULL;

	return TRUE;
}

gboolean g_at_server_set_echo(GAtServer *server, gboolean echo)
{
	if (server == NULL)
		return FALSE;

	server->v250.echo = echo;

	return TRUE;
}

gboolean g_at_server_set_disconnect_function(GAtServer *server,
						GAtDisconnectFunc disconnect,
						gpointer user_data)
{
	if (server == NULL)
		return FALSE;

	server->user_disconnect = disconnect;
	server->user_disconnect_data = user_data;

	return TRUE;
}

gboolean g_at_server_set_debug(GAtServer *server, GAtDebugFunc func,
						gpointer user_data)
{
	if (server == NULL)
		return FALSE;

	server->debugf = func;
	server->debug_data = user_data;

	g_at_io_set_debug(server->io, server->debugf, server->debug_data);

	return TRUE;
}

gboolean g_at_server_register(GAtServer *server, const char *prefix,
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
	if (node == NULL)
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
	if (node == NULL)
		return FALSE;

	g_hash_table_remove(server->command_list, prefix);

	return TRUE;
}

gboolean g_at_server_set_finish_callback(GAtServer *server,
						GAtServerFinishFunc finishf,
						gpointer user_data)
{
	if (server == NULL)
		return FALSE;

	server->finishf = finishf;
	server->finish_data = user_data;

	return TRUE;
}

gboolean g_at_server_command_pending(GAtServer *server)
{
	if (server == NULL)
		return FALSE;

	return server->final_async;
}
