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

#ifndef __GATCHAT_RESULT_H
#define __GATCHAT_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

struct _GAtResult {
	GSList *lines;
	char *final_or_pdu;
};

typedef struct _GAtResult GAtResult;

#define G_AT_RESULT_LINE_LENGTH_MAX 2048

struct _GAtResultIter {
	GAtResult *result;
	GSList *l;
	char buf[G_AT_RESULT_LINE_LENGTH_MAX + 1];
	unsigned int line_pos;
	GSList pre;
};

typedef struct _GAtResultIter GAtResultIter;

void g_at_result_iter_init(GAtResultIter *iter, GAtResult *result);

gboolean g_at_result_iter_next(GAtResultIter *iter, const char *prefix);
gboolean g_at_result_iter_open_list(GAtResultIter *iter);
gboolean g_at_result_iter_close_list(GAtResultIter *iter);

gboolean g_at_result_iter_skip_next(GAtResultIter *iter);

gboolean g_at_result_iter_next_range(GAtResultIter *iter, gint *min, gint *max);
gboolean g_at_result_iter_next_string(GAtResultIter *iter, const char **str);
gboolean g_at_result_iter_next_unquoted_string(GAtResultIter *iter,
						const char **str);
gboolean g_at_result_iter_next_number(GAtResultIter *iter, gint *number);
gboolean g_at_result_iter_next_number_default(GAtResultIter *iter, gint dflt,
						gint *number);
gboolean g_at_result_iter_next_hexstring(GAtResultIter *iter,
		const guint8 **str, gint *length);

const char *g_at_result_iter_raw_line(GAtResultIter *iter);

const char *g_at_result_final_response(GAtResult *result);
const char *g_at_result_pdu(GAtResult *result);

gint g_at_result_num_response_lines(GAtResult *result);

#ifdef __cplusplus
}
#endif

#endif /* __GATCHAT_RESULT_H */
