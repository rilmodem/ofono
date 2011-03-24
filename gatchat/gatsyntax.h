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

#ifndef __GATSYNTAX_H
#define __GATSYNTAX_H

#ifdef __cplusplus
extern "C" {
#endif

enum _GAtSyntaxExpectHint {
	G_AT_SYNTAX_EXPECT_PDU,
	G_AT_SYNTAX_EXPECT_MULTILINE,
	G_AT_SYNTAX_EXPECT_PROMPT,
	G_AT_SYNTAX_EXPECT_SHORT_PROMPT
};

typedef enum _GAtSyntaxExpectHint GAtSyntaxExpectHint;

enum _GAtSyntaxResult {
	G_AT_SYNTAX_RESULT_UNRECOGNIZED,
	G_AT_SYNTAX_RESULT_UNSURE,
	G_AT_SYNTAX_RESULT_LINE,
	G_AT_SYNTAX_RESULT_MULTILINE,
	G_AT_SYNTAX_RESULT_PDU,
	G_AT_SYNTAX_RESULT_PROMPT,
};

typedef enum _GAtSyntaxResult GAtSyntaxResult;

typedef struct _GAtSyntax GAtSyntax;

typedef void (*GAtSyntaxSetHintFunc)(GAtSyntax *syntax,
					GAtSyntaxExpectHint hint);
typedef GAtSyntaxResult (*GAtSyntaxFeedFunc)(GAtSyntax *syntax,
						const char *bytes, gsize *len);

struct _GAtSyntax {
	gint ref_count;
	int state;
	GAtSyntaxSetHintFunc set_hint;
	GAtSyntaxFeedFunc feed;
};


GAtSyntax *g_at_syntax_new_full(GAtSyntaxFeedFunc feed,
					GAtSyntaxSetHintFunc hint,
					int initial_state);

/* This syntax implements very strict checking of 27.007 standard, which means
 * it might not work with a majority of modems.  However, it does handle echo
 * properly and can be used to detect a modem's deviations from the relevant
 * standards.
 */
GAtSyntax *g_at_syntax_new_gsmv1(void);

/* This syntax implements an extremely lax parser that can handle a variety
 * of modems.  Unfortunately it does not deal with echo at all, so echo must
 * be explicitly turned off before using the parser
 */
GAtSyntax *g_at_syntax_new_gsm_permissive(void);

GAtSyntax *g_at_syntax_ref(GAtSyntax *syntax);
void g_at_syntax_unref(GAtSyntax *syntax);

#ifdef __cplusplus
}
#endif

#endif /* __GATSYNTAX_H */
