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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>

#include "gatsyntax.h"

enum GSMV1_STATE {
	GSMV1_STATE_IDLE = 0,
	GSMV1_STATE_INITIAL_CR,
	GSMV1_STATE_INITIAL_LF,
	GSMV1_STATE_RESPONSE,
	GSMV1_STATE_RESPONSE_STRING,
	GSMV1_STATE_TERMINATOR_CR,
	GSMV1_STATE_GUESS_MULTILINE_RESPONSE,
	GSMV1_STATE_MULTILINE_RESPONSE,
	GSMV1_STATE_MULTILINE_TERMINATOR_CR,
	GSMV1_STATE_PDU_CHECK_EXTRA_CR,
	GSMV1_STATE_PDU_CHECK_EXTRA_LF,
	GSMV1_STATE_PDU,
	GSMV1_STATE_PDU_CR,
	GSMV1_STATE_PROMPT,
	GSMV1_STATE_ECHO,
	GSMV1_STATE_PPP_DATA,
	GSMV1_STATE_SHORT_PROMPT,
	GSMV1_STATE_SHORT_PROMPT_CR,
};

enum GSM_PERMISSIVE_STATE {
	GSM_PERMISSIVE_STATE_IDLE = 0,
	GSM_PERMISSIVE_STATE_RESPONSE,
	GSM_PERMISSIVE_STATE_RESPONSE_STRING,
	GSM_PERMISSIVE_STATE_GUESS_PDU,
	GSM_PERMISSIVE_STATE_PDU,
	GSM_PERMISSIVE_STATE_PROMPT,
	GSM_PERMISSIVE_STATE_GUESS_SHORT_PROMPT,
	GSM_PERMISSIVE_STATE_SHORT_PROMPT,
};

static void gsmv1_hint(GAtSyntax *syntax, GAtSyntaxExpectHint hint)
{
	switch (hint) {
	case G_AT_SYNTAX_EXPECT_PDU:
		syntax->state = GSMV1_STATE_PDU_CHECK_EXTRA_CR;
		break;
	case G_AT_SYNTAX_EXPECT_MULTILINE:
		syntax->state = GSMV1_STATE_GUESS_MULTILINE_RESPONSE;
		break;
	case G_AT_SYNTAX_EXPECT_SHORT_PROMPT:
		syntax->state = GSMV1_STATE_SHORT_PROMPT;
		break;
	default:
		break;
	};
}

static GAtSyntaxResult gsmv1_feed(GAtSyntax *syntax,
					const char *bytes, gsize *len)
{
	gsize i = 0;
	GAtSyntaxResult res = G_AT_SYNTAX_RESULT_UNSURE;

	while (i < *len) {
		char byte = bytes[i];

		switch (syntax->state) {
		case GSMV1_STATE_IDLE:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_INITIAL_CR;
			else if (byte == '~')
				syntax->state = GSMV1_STATE_PPP_DATA;
			else
				syntax->state = GSMV1_STATE_ECHO;
			break;

		case GSMV1_STATE_INITIAL_CR:
			if (byte == '\n')
				syntax->state = GSMV1_STATE_INITIAL_LF;
			else if (byte == '\r') {
				syntax->state = GSMV1_STATE_IDLE;
				return G_AT_SYNTAX_RESULT_UNRECOGNIZED;
			} else
				syntax->state = GSMV1_STATE_ECHO;
			break;

		case GSMV1_STATE_INITIAL_LF:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_TERMINATOR_CR;
			else if (byte == '>')
				syntax->state = GSMV1_STATE_PROMPT;
			else if (byte == '"')
				syntax->state = GSMV1_STATE_RESPONSE_STRING;
			else
				syntax->state = GSMV1_STATE_RESPONSE;
			break;

		case GSMV1_STATE_RESPONSE:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_TERMINATOR_CR;
			else if (byte == '"')
				syntax->state = GSMV1_STATE_RESPONSE_STRING;
			break;

		case GSMV1_STATE_RESPONSE_STRING:
			if (byte == '"')
				syntax->state = GSMV1_STATE_RESPONSE;
			break;

		case GSMV1_STATE_TERMINATOR_CR:
			syntax->state = GSMV1_STATE_IDLE;

			if (byte == '\n') {
				i += 1;
				res = G_AT_SYNTAX_RESULT_LINE;
			} else
				res = G_AT_SYNTAX_RESULT_UNRECOGNIZED;

			goto out;

		case GSMV1_STATE_GUESS_MULTILINE_RESPONSE:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_INITIAL_CR;
			else
				syntax->state = GSMV1_STATE_MULTILINE_RESPONSE;
			break;

		case GSMV1_STATE_MULTILINE_RESPONSE:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_MULTILINE_TERMINATOR_CR;
			break;

		case GSMV1_STATE_MULTILINE_TERMINATOR_CR:
			syntax->state = GSMV1_STATE_IDLE;

			if (byte == '\n') {
				i += 1;
				res = G_AT_SYNTAX_RESULT_MULTILINE;
			} else
				res = G_AT_SYNTAX_RESULT_UNRECOGNIZED;

			goto out;

		/* Some 27.007 compliant modems still get this wrong.  They
		 * insert an extra CRLF between the command and he PDU,
		 * in effect making them two separate lines.  We try to
		 * handle this case gracefully
		 */
		case GSMV1_STATE_PDU_CHECK_EXTRA_CR:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_PDU_CHECK_EXTRA_LF;
			else
				syntax->state = GSMV1_STATE_PDU;
			break;

		case GSMV1_STATE_PDU_CHECK_EXTRA_LF:
			res = G_AT_SYNTAX_RESULT_UNRECOGNIZED;
			syntax->state = GSMV1_STATE_PDU;

			if (byte == '\n')
				i += 1;

			goto out;

		case GSMV1_STATE_PDU:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_PDU_CR;
			break;

		case GSMV1_STATE_PDU_CR:
			syntax->state = GSMV1_STATE_IDLE;

			if (byte == '\n') {
				i += 1;
				res = G_AT_SYNTAX_RESULT_PDU;
			} else
				res = G_AT_SYNTAX_RESULT_UNRECOGNIZED;

			goto out;

		case GSMV1_STATE_PROMPT:
			if (byte == ' ') {
				syntax->state = GSMV1_STATE_IDLE;
				i += 1;
				res = G_AT_SYNTAX_RESULT_PROMPT;
				goto out;
			}

			syntax->state = GSMV1_STATE_RESPONSE;
			return G_AT_SYNTAX_RESULT_UNSURE;

		case GSMV1_STATE_ECHO:
			/* This handles the case of echo of the PDU terminated
			 * by CtrlZ character
			 */
			if (byte == 26 || byte == '\r') {
				syntax->state = GSMV1_STATE_IDLE;
				res = G_AT_SYNTAX_RESULT_UNRECOGNIZED;
				i += 1;
				goto out;
			}

			break;

		case GSMV1_STATE_PPP_DATA:
			if (byte == '~') {
				syntax->state = GSMV1_STATE_IDLE;
				res = G_AT_SYNTAX_RESULT_UNRECOGNIZED;
				i += 1;
				goto out;
			}

			break;

		case GSMV1_STATE_SHORT_PROMPT:
			if (byte == '\r')
				syntax->state = GSMV1_STATE_SHORT_PROMPT_CR;
			else
				syntax->state = GSMV1_STATE_ECHO;

			break;

		case GSMV1_STATE_SHORT_PROMPT_CR:
			if (byte == '\n') {
				syntax->state = GSMV1_STATE_IDLE;
				i += 1;
				res = G_AT_SYNTAX_RESULT_PROMPT;
				goto out;
			}

			syntax->state = GSMV1_STATE_RESPONSE;
			return G_AT_SYNTAX_RESULT_UNSURE;

		default:
			break;
		};

		i += 1;
	}

out:
	*len = i;
	return res;
}

static void gsm_permissive_hint(GAtSyntax *syntax, GAtSyntaxExpectHint hint)
{
	if (hint == G_AT_SYNTAX_EXPECT_PDU)
		syntax->state = GSM_PERMISSIVE_STATE_GUESS_PDU;
	else if (hint == G_AT_SYNTAX_EXPECT_SHORT_PROMPT)
		syntax->state = GSM_PERMISSIVE_STATE_GUESS_SHORT_PROMPT;
}

static GAtSyntaxResult gsm_permissive_feed(GAtSyntax *syntax,
						const char *bytes, gsize *len)
{
	gsize i = 0;
	GAtSyntaxResult res = G_AT_SYNTAX_RESULT_UNSURE;

	while (i < *len) {
		char byte = bytes[i];

		switch (syntax->state) {
		case GSM_PERMISSIVE_STATE_IDLE:
			if (byte == '\r' || byte == '\n')
				/* ignore */;
			else if (byte == '>')
				syntax->state = GSM_PERMISSIVE_STATE_PROMPT;
			else
				syntax->state = GSM_PERMISSIVE_STATE_RESPONSE;
			break;

		case GSM_PERMISSIVE_STATE_RESPONSE:
			if (byte == '\r') {
				syntax->state = GSM_PERMISSIVE_STATE_IDLE;

				i += 1;
				res = G_AT_SYNTAX_RESULT_LINE;
				goto out;
			} else if (byte == '"')
				syntax->state =
					GSM_PERMISSIVE_STATE_RESPONSE_STRING;
			break;

		case GSM_PERMISSIVE_STATE_RESPONSE_STRING:
			if (byte == '"')
				syntax->state = GSM_PERMISSIVE_STATE_RESPONSE;
			break;

		case GSM_PERMISSIVE_STATE_GUESS_PDU:
			if (byte != '\r' && byte != '\n')
				syntax->state = GSM_PERMISSIVE_STATE_PDU;
			break;

		case GSM_PERMISSIVE_STATE_PDU:
			if (byte == '\r') {
				syntax->state = GSM_PERMISSIVE_STATE_IDLE;

				i += 1;
				res = G_AT_SYNTAX_RESULT_PDU;
				goto out;
			}
			break;

		case GSM_PERMISSIVE_STATE_PROMPT:
			if (byte == ' ') {
				syntax->state = GSM_PERMISSIVE_STATE_IDLE;
				i += 1;
				res = G_AT_SYNTAX_RESULT_PROMPT;
				goto out;
			}

			syntax->state = GSM_PERMISSIVE_STATE_RESPONSE;
			return G_AT_SYNTAX_RESULT_UNSURE;

		case GSM_PERMISSIVE_STATE_GUESS_SHORT_PROMPT:
			if (byte == '\n')
				/* ignore */;
			else if (byte == '\r')
				syntax->state =
					GSM_PERMISSIVE_STATE_SHORT_PROMPT;
			else
				syntax->state = GSM_PERMISSIVE_STATE_RESPONSE;
			break;

		case GSM_PERMISSIVE_STATE_SHORT_PROMPT:
			if (byte == '\n') {
				syntax->state = GSM_PERMISSIVE_STATE_IDLE;
				i += 1;
				res = G_AT_SYNTAX_RESULT_PROMPT;
				goto out;
			}

			syntax->state = GSM_PERMISSIVE_STATE_RESPONSE;
			return G_AT_SYNTAX_RESULT_UNSURE;

		default:
			break;
		};

		i += 1;
	}

out:
	*len = i;
	return res;
}

GAtSyntax *g_at_syntax_new_full(GAtSyntaxFeedFunc feed,
					GAtSyntaxSetHintFunc hint,
					int initial_state)
{
	GAtSyntax *syntax;

	syntax = g_new0(GAtSyntax, 1);

	syntax->feed = feed;
	syntax->set_hint = hint;
	syntax->state = initial_state;
	syntax->ref_count = 1;

	return syntax;
}


GAtSyntax *g_at_syntax_new_gsmv1(void)
{
	return g_at_syntax_new_full(gsmv1_feed, gsmv1_hint, GSMV1_STATE_IDLE);
}

GAtSyntax *g_at_syntax_new_gsm_permissive(void)
{
	return g_at_syntax_new_full(gsm_permissive_feed, gsm_permissive_hint,
					GSM_PERMISSIVE_STATE_IDLE);
}

GAtSyntax *g_at_syntax_ref(GAtSyntax *syntax)
{
	if (syntax == NULL)
		return NULL;

	g_atomic_int_inc(&syntax->ref_count);

	return syntax;
}

void g_at_syntax_unref(GAtSyntax *syntax)
{
	gboolean is_zero;

	if (syntax == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&syntax->ref_count);

	if (is_zero)
		g_free(syntax);
}
