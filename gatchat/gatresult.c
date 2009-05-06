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

#include <string.h>

#include <glib.h>

#include "gatresult.h"

void g_at_result_iter_init(GAtResultIter *iter, GAtResult *result)
{
	iter->result = result;
	iter->pre.next = result->lines;
	iter->pre.data = NULL;
	iter->l = &iter->pre;
	iter->line_pos = 0;
}

gboolean g_at_result_iter_next(GAtResultIter *iter, const char *prefix)
{
	char *line;
	int prefix_len = prefix ? strlen(prefix) : 0;

	while ((iter->l = iter->l->next)) {
		line = iter->l->data;

		if (prefix_len == 0) {
			iter->line_pos = 0;
			return TRUE;
		}

		if (g_str_has_prefix(line, prefix) == FALSE)
			continue;

		iter->line_pos = prefix_len;

		while (iter->line_pos < strlen(line) &&
			line[iter->line_pos] == ' ')
			iter->line_pos += 1;

		return TRUE;
	}

	return FALSE;
}

const char *g_at_result_iter_raw_line(GAtResultIter *iter)
{
	const char *line;

	if (!iter)
		return NULL;

	if (!iter->l)
		return NULL;

	line = iter->l->data;

	line += iter->line_pos;

	return line;
}

static inline int skip_to_next_field(const char *line, int pos, int len)
{
	if (pos < len && line[pos] == ',')
		pos += 1;

	while (pos < len && line[pos] == ' ')
		pos += 1;

	return pos;
}

gboolean g_at_result_iter_next_string(GAtResultIter *iter, const char **str)
{
	unsigned int pos;
	unsigned int end;
	unsigned int len;
	char *line;

	if (!iter)
		return FALSE;

	if (!iter->l)
		return FALSE;

	line = iter->l->data;
	len = strlen(line);

	pos = iter->line_pos;

	/* Omitted string */
	if (line[pos] == ',') {
		end = pos;
		memset(iter->buf, 0, sizeof(iter->buf));
		goto out;
	}

	if (line[pos++] != '"')
		return FALSE;

	end = pos;

	while (end < len && line[end] != '"')
		end += 1;

	if (line[end] != '"')
		return FALSE;

	if (end - pos >= sizeof(iter->buf))
		return FALSE;

	strncpy(iter->buf, line+pos, end-pos);
	memset(iter->buf + end - pos, 0, sizeof(iter->buf) - end + pos);

	/* Skip " */
	end += 1;

out:
	iter->line_pos = skip_to_next_field(line, end, len);

	if (str)
		*str = iter->buf;

	return TRUE;
}

gboolean g_at_result_iter_next_number(GAtResultIter *iter, gint *number)
{
	int pos;
	int end;
	int len;
	int value = 0;
	char *line;

	if (!iter)
		return FALSE;

	if (!iter->l)
		return FALSE;

	line = iter->l->data;
	len = strlen(line);

	pos = iter->line_pos;
	end = pos;

	while (line[end] >= '0' && line[end] <= '9') {
		value = value * 10 + (int)(line[end] - '0');
		end += 1;
	}

	if (pos == end)
		return FALSE;

	iter->line_pos = skip_to_next_field(line, end, len);

	if (number)
		*number = value;

	return TRUE;
}

gboolean g_at_result_iter_next_range(GAtResultIter *iter, gint *min, gint *max)
{
	int pos;
	int end;
	int len;
	int low = 0;
	int high = 0;
	char *line;

	if (!iter)
		return FALSE;

	if (!iter->l)
		return FALSE;

	line = iter->l->data;
	len = strlen(line);

	pos = iter->line_pos;

	while (pos < len && line[pos] == ' ')
		pos += 1;

	end = pos;

	while (line[end] >= '0' && line[end] <= '9') {
		low = low * 10 + (int)(line[end] - '0');
		end += 1;
	}

	if (pos == end)
		return FALSE;

	if (line[end] == ',') {
		high = low;
		goto out;
	}

	if (line[end] == '-')
		pos = end = end + 1;
	else
		return FALSE;

	while (line[end] >= '0' && line[end] <= '9') {
		high = high * 10 + (int)(line[end] - '0');
		end += 1;
	}

	if (pos == end)
		return FALSE;

out:
	iter->line_pos = skip_to_next_field(line, end, len);

	if (min)
		*min = low;

	if (max)
		*max = high;

	return TRUE;
}

static gint skip_until(const char *line, int start, const char delim)
{
	int len = strlen(line);
	int i = start;

	while (i < len) {
		if (line[i] == delim)
			return i;

		if (line[i] != '(') {
			i += 1;
			continue;
		}

		i = skip_until(line, i+1, ')');

		if (i < len)
			i += 1;
	}

	return i;
}

gboolean g_at_result_iter_skip_next(GAtResultIter *iter)
{
	unsigned int skipped_to;
	char *line;

	if (!iter)
		return FALSE;

	if (!iter->l)
		return FALSE;

	line = iter->l->data;

	skipped_to = skip_until(line, iter->line_pos, ',');

	if (skipped_to == iter->line_pos && line[skipped_to] != ',')
		return FALSE;

	iter->line_pos = skip_to_next_field(line, skipped_to, strlen(line));

	return TRUE;
}

gboolean g_at_result_iter_open_list(GAtResultIter *iter)
{
	char *line;
	unsigned int len;

	if (!iter)
		return FALSE;

	if (!iter->l)
		return FALSE;

	line = iter->l->data;
	len = strlen(line);

	if (iter->line_pos >= len)
		return FALSE;

	if (line[iter->line_pos] != '(')
		return FALSE;

	iter->line_pos += 1;

	while (iter->line_pos < strlen(line) &&
		line[iter->line_pos] == ' ')
		iter->line_pos += 1;

	return TRUE;
}

gboolean g_at_result_iter_close_list(GAtResultIter *iter)
{
	char *line;
	unsigned int len;

	if (!iter)
		return FALSE;

	if (!iter->l)
		return FALSE;

	line = iter->l->data;
	len = strlen(line);

	if (iter->line_pos >= len)
		return FALSE;

	if (line[iter->line_pos] != ')')
		return FALSE;

	iter->line_pos += 1;

	iter->line_pos = skip_to_next_field(line, iter->line_pos, len);

	return TRUE;
}

const char *g_at_result_final_response(GAtResult *result)
{
	if (!result)
		return NULL;

	return result->final_or_pdu;
}

const char *g_at_result_pdu(GAtResult *result)
{
	if (!result)
		return NULL;

	return result->final_or_pdu;
}

gint g_at_result_num_response_lines(GAtResult *result)
{
	if (!result)
		return 0;

	if (!result->lines)
		return 0;

	return g_slist_length(result->lines);
}
