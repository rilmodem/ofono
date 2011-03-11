/*
 * oFono - GSM Telephony Stack for Linux
 *
 * Copyright (C) 2008-2010 Intel Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/phonebook.h>
#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"
#include "vendor.h"

#define INDEX_INVALID -1

#define CHARSET_UTF8 1
#define CHARSET_UCS2 2
#define CHARSET_IRA  4
#define CHARSET_SUPPORT (CHARSET_UTF8 | CHARSET_UCS2)

static const char *none_prefix[] = { NULL };
static const char *cpbr_prefix[] = { "+CPBR:", NULL };
static const char *cscs_prefix[] = { "+CSCS:", NULL };
static const char *cpbs_prefix[] = { "+CPBS:", NULL };

struct pb_data {
	int index_min, index_max;
	char *old_charset;
	int supported;
	GAtChat *chat;
	unsigned int vendor;
	guint ready_id;
};

static void warn_bad(void)
{
	ofono_warn("Name field conversion to UTF8 failed, this can indicate a"
			" problem with modem integration, as this field"
			" is required by 27.007.");
}

static gboolean parse_text(GAtResultIter *iter, char **str, int encoding)
{
	const char *string;
	const guint8 *hex;
	int len;
	char *utf8;
	/* charset_current is CHARSET_UCS2, CHARSET_IRA or CHARSET_UTF8 */
	if (encoding == CHARSET_UCS2) {
		/*
		 * Some devices omit the quotes, so use next_hexstring,
		 * which handles quoted or unquoted hex strings
		 */
		if (g_at_result_iter_next_hexstring(iter, &hex, &len) == FALSE)
			return FALSE;

		utf8 = g_convert((const gchar*) hex, len,
					"UTF-8//TRANSLIT", "UCS-2BE",
					NULL, NULL, NULL);

		if (utf8) {
			*str = utf8;
			return TRUE;
		}

		return FALSE;
	}

	/*
	 * In the case of IRA charset, assume these are Latin1
	 * characters, same as in UTF8
	 */
	if (g_at_result_iter_next_string(iter, &string)) {
		*str = g_strdup(string);
		return TRUE;
	}

	return FALSE;
}

static const char *best_charset(int supported)
{
	const char *charset = "Invalid";

	if (supported & CHARSET_IRA)
		charset = "IRA";

	if (supported & CHARSET_UCS2)
		charset = "UCS2";

	if (supported & CHARSET_UTF8)
		charset = "UTF-8";

	return charset;
}

static void at_cpbr_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	GAtResultIter iter;
	int current;

	if (pbd->supported & CHARSET_IRA)
		current = CHARSET_IRA;

	if (pbd->supported & CHARSET_UCS2)
		current = CHARSET_UCS2;

	if (pbd->supported & CHARSET_UTF8)
		current = CHARSET_UTF8;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CPBR:")) {
		int index;
		const char *number;
		int type;
		char *text;
		int hidden = -1;
		char *group = NULL;
		const char *adnumber = NULL;
		int adtype = -1;
		char *secondtext = NULL;
		char *email = NULL;
		char *sip_uri = NULL;
		char *tel_uri = NULL;

		if (!g_at_result_iter_next_number(&iter, &index))
			continue;

		if (!g_at_result_iter_next_string(&iter, &number))
			continue;

		if (!g_at_result_iter_next_number(&iter, &type))
			continue;

		if (!parse_text(&iter, &text, current)) {
			warn_bad();
			continue;
		}

		g_at_result_iter_next_number_default(&iter, 0, &hidden);
		parse_text(&iter, &group, current);
		g_at_result_iter_next_string(&iter, &adnumber);
		g_at_result_iter_next_number_default(&iter, 0, &adtype);
		parse_text(&iter, &secondtext, current);
		parse_text(&iter, &email, current);
		parse_text(&iter, &sip_uri, current);
		parse_text(&iter, &tel_uri, current);

		ofono_phonebook_entry(pb, index, number, type,
			text, hidden, group, adnumber,
			adtype, secondtext, email,
			sip_uri, tel_uri);

		g_free(text);
		g_free(group);
		g_free(secondtext);
		g_free(email);
		g_free(sip_uri);
		g_free(tel_uri);
	}
}

static void export_failed(struct cb_data *cbd)
{
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	ofono_phonebook_cb_t cb = cbd->cb;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);

	if (pbd->old_charset) {
		g_free(pbd->old_charset);
		pbd->old_charset = NULL;
	}
}

static void at_read_entries_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	ofono_phonebook_cb_t cb = cbd->cb;
	const char *charset;
	struct ofono_error error;
	char buf[32];

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
	g_free(cbd);

	charset = best_charset(pbd->supported);

	if (strcmp(pbd->old_charset, charset)) {
		snprintf(buf, sizeof(buf), "AT+CSCS=\"%s\"", pbd->old_charset);
		g_at_chat_send(pbd->chat, buf, none_prefix, NULL, NULL, NULL);
	}

	g_free(pbd->old_charset);
	pbd->old_charset = NULL;
}

static void at_read_entries(struct cb_data *cbd)
{
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	char buf[32];

	snprintf(buf, sizeof(buf), "AT+CPBR=%d,%d",
			pbd->index_min, pbd->index_max);
	if (g_at_chat_send_listing(pbd->chat, buf, cpbr_prefix,
					at_cpbr_notify, at_read_entries_cb,
					cbd, NULL) > 0)
		return;

	/* If we get here, then most likely connection to the modem dropped
	 * and we can't really restore the charset anyway
	 */
	export_failed(cbd);
}

static void at_set_charset_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;

	if (!ok) {
		export_failed(cbd);
		return;
	}

	at_read_entries(cbd);
}

static void at_read_charset_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	GAtResultIter iter;
	const char *charset;
	char buf[32];

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		goto error;

	g_at_result_iter_next_string(&iter, &charset);

	pbd->old_charset = g_strdup(charset);

	charset = best_charset(pbd->supported);

	if (!strcmp(pbd->old_charset, charset)) {
		at_read_entries(cbd);
		return;
	}

	snprintf(buf, sizeof(buf), "AT+CSCS=\"%s\"", charset);
	if (g_at_chat_send(pbd->chat, buf, none_prefix,
				at_set_charset_cb, cbd, NULL) > 0)
		return;

error:
	export_failed(cbd);
}

static void at_list_indices_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	GAtResultIter iter;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CPBR:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto error;

	/* Retrieve index_min and index_max from indices
	 * which seems like "(1-150),32,16"
	 */
	if (!g_at_result_iter_next_range(&iter, &pbd->index_min,
						&pbd->index_max))
		goto error;

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	if (g_at_chat_send(pbd->chat, "AT+CSCS?", cscs_prefix,
				at_read_charset_cb, cbd, NULL) > 0)
		return;

error:
	export_failed(cbd);
}

static void at_select_storage_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);

	if (!ok)
		goto error;

	if (g_at_chat_send(pbd->chat, "AT+CPBR=?", cpbr_prefix,
				at_list_indices_cb, cbd, NULL) > 0)
		return;

error:
	export_failed(cbd);
}

static void at_export_entries(struct ofono_phonebook *pb, const char *storage,
				ofono_phonebook_cb_t cb, void *data)
{
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[32];

	cbd->user = pb;

	snprintf(buf, sizeof(buf), "AT+CPBS=\"%s\"", storage);
	if (g_at_chat_send(pbd->chat, buf, none_prefix,
				at_select_storage_cb, cbd, NULL) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void phonebook_not_supported(struct ofono_phonebook *pb)
{
	ofono_error("Phonebook not supported by this modem.  If this is in "
			"error please submit patches to support this hardware");

	ofono_phonebook_remove(pb);
}

static void at_list_storages_cb(gboolean ok, GAtResult *result,
						gpointer user_data);

static void ifx_pbready_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_phonebook *pb = user_data;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);

	g_at_chat_unregister(pbd->chat, pbd->ready_id);
	pbd->ready_id = 0;

	if (g_at_chat_send(pbd->chat, "AT+CPBS=?", cpbs_prefix,
				at_list_storages_cb, pb, NULL) > 0)
		return;

	phonebook_not_supported(pb);
}

static void at_list_storages_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_phonebook *pb = user_data;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	gboolean sm_supported = FALSE;
	gboolean me_supported = FALSE;
	gboolean in_list = FALSE;
	GAtResultIter iter;
	const char *storage;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CPBS:"))
		goto error;

	/* Some modems don't report CPBS in a proper list */
	if (g_at_result_iter_open_list(&iter))
		in_list = TRUE;

	while (g_at_result_iter_next_string(&iter, &storage)) {
		if (!strcmp(storage, "ME"))
			me_supported = TRUE;
		else if (!strcmp(storage, "SM"))
			sm_supported = TRUE;
	}

	if (in_list && !g_at_result_iter_close_list(&iter))
		goto vendor;

	if (!me_supported && !sm_supported)
		goto vendor;

	ofono_phonebook_register(pb);
	return;

vendor:
	switch (pbd->vendor) {
	case OFONO_VENDOR_IFX:
		pbd->ready_id = g_at_chat_register(pbd->chat, "+PBREADY",
					ifx_pbready_notify, FALSE, pb, NULL);
		return;
	}

error:
	phonebook_not_supported(pb);
}

static void at_list_charsets_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_phonebook *pb = user_data;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	gboolean in_list = FALSE;
	GAtResultIter iter;
	const char *charset;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		goto error;

	/* Some modems don't report CSCS in a proper list */
	if (g_at_result_iter_open_list(&iter))
		in_list = TRUE;

	while (g_at_result_iter_next_string(&iter, &charset)) {
		if (!strcmp(charset, "UTF-8"))
			pbd->supported |= CHARSET_UTF8;
		else if (!strcmp(charset, "UCS2"))
			pbd->supported |= CHARSET_UCS2;
		else if (!strcmp(charset, "IRA"))
			pbd->supported |= CHARSET_IRA;
	}

	if (in_list && !g_at_result_iter_close_list(&iter))
		goto error;

	if (!(pbd->supported & CHARSET_SUPPORT)) {
		/* Some modems, like the Google G1, do not support UCS2 or UTF8
		 * Such modems are effectively junk, but we can still get some
		 * useful information out of them by using IRA charset, which
		 * is essentially Latin1.  Still, all bets are off if a SIM
		 * with UCS2 encoded entries is present.
		 */
		if (pbd->supported & CHARSET_IRA) {
			ofono_error("This modem does not support UCS2 or UTF8 "
					"character sets.  This means no i18n "
					"phonebook is possible on this modem,"
					" if this is in error, submit patches "
					"to properly support this hardware");
		} else {
			goto error;
		}
	}

	if (g_at_chat_send(pbd->chat, "AT+CPBS=?", cpbs_prefix,
				at_list_storages_cb, pb, NULL) > 0)
		return;

error:
	phonebook_not_supported(pb);
}

static void at_list_charsets(struct ofono_phonebook *pb)
{
	struct pb_data *pbd = ofono_phonebook_get_data(pb);

	if (g_at_chat_send(pbd->chat, "AT+CSCS=?", cscs_prefix,
				at_list_charsets_cb, pb, NULL) > 0)
		return;

	phonebook_not_supported(pb);
}

static int at_phonebook_probe(struct ofono_phonebook *pb, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct pb_data *pbd;

	pbd = g_try_new0(struct pb_data, 1);
	if (pbd == NULL)
		return -ENOMEM;

	pbd->chat = g_at_chat_clone(chat);
	pbd->vendor = vendor;

	ofono_phonebook_set_data(pb, pbd);

	at_list_charsets(pb);

	return 0;
}

static void at_phonebook_remove(struct ofono_phonebook *pb)
{
	struct pb_data *pbd = ofono_phonebook_get_data(pb);

	if (pbd->old_charset)
		g_free(pbd->old_charset);

	ofono_phonebook_set_data(pb, NULL);

	g_at_chat_unref(pbd->chat);
	g_free(pbd);
}

static struct ofono_phonebook_driver driver = {
	.name		= "atmodem",
	.probe		= at_phonebook_probe,
	.remove		= at_phonebook_remove,
	.export_entries	= at_export_entries
};

void at_phonebook_init(void)
{
	ofono_phonebook_driver_register(&driver);
}

void at_phonebook_exit(void)
{
	ofono_phonebook_driver_unregister(&driver);
}
