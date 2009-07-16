/*
 * oFono - GSM Telephony Stack for Linux
 *
 * Copyright (C) 2008-2009 Intel Corporation.  All rights reserved.
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

#include <glib.h>

#include <ofono/log.h>
#include "driver.h"
#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "at.h"


#define INDEX_INVALID -1

#define CHARSET_UTF8 1
#define CHARSET_UCS2 2
#define CHARSET_SUPPORT (CHARSET_UTF8 | CHARSET_UCS2)

static const char *none_prefix[] = { NULL };
static const char *entries_prefix[] = { "+CPBR:", NULL };
static const char *charset_prefix[] = { "+CSCS:", NULL };

static void at_select_storage(struct ofono_modem *modem,
				ofono_generic_cb_t cb, void *data);
static void at_select_charset(struct ofono_modem *modem,
				ofono_generic_cb_t cb, void *data);

struct pb_data {
	const char *storage;
	int index_min, index_max;
	int charset_origin;
	const char *charset_origin_str;
	int charset_current;
	int charset_list;
	int charset_need_restore;
	int has_charset_info;
};

static struct pb_data *phonebook_create()
{
	struct pb_data *pb = g_try_new0(struct pb_data, 1);
	return pb;
}

static void phonebook_destroy(struct pb_data *data)
{
	g_free((char *)data->charset_origin_str);
	g_free(data);
}

static const char *ucs2_to_utf8(const char *str)
{
	long len;
	unsigned char *ucs2;
	const char *utf8;
	ucs2 = decode_hex(str, -1, &len, 0);
	utf8 = g_convert(ucs2, len, "UTF-8//TRANSLIT", "UCS-2BE",
					NULL, NULL, NULL);
	g_free(ucs2);
	return utf8;
}

static void at_cpbr_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CPBR:")) {
		int index;
		const char *number;
		int type;
		const char *text;
		const char *text_utf8;
		int hidden = -1;
		const char *group = NULL;
		const char *group_utf8 = NULL;
		const char *adnumber = NULL;
		int adtype = -1;
		const char *secondtext = NULL;
		const char *secondtext_utf8 = NULL;
		const char *email = NULL;
		const char *email_utf8 = NULL;
		const char *sip_uri = NULL;
		const char *sip_uri_utf8 = NULL;
		const char *tel_uri = NULL;
		const char *tel_uri_utf8 = NULL;

		if (!g_at_result_iter_next_number(&iter, &index))
			continue;

		if (!g_at_result_iter_next_string(&iter, &number))
			continue;

		if (!g_at_result_iter_next_number(&iter, &type))
			continue;

		if (!g_at_result_iter_next_string(&iter, &text))
			continue;

		g_at_result_iter_next_number(&iter, &hidden);
		g_at_result_iter_next_string(&iter, &group);
		g_at_result_iter_next_string(&iter, &adnumber);
		g_at_result_iter_next_number(&iter, &adtype);
		g_at_result_iter_next_string(&iter, &secondtext);
		g_at_result_iter_next_string(&iter, &email);
		g_at_result_iter_next_string(&iter, &sip_uri);
		g_at_result_iter_next_string(&iter, &tel_uri);

		/* charset_current is either CHARSET_UCS2 or CHARSET_UTF8 */
		if (at->pb->charset_current & CHARSET_UCS2) {
			text_utf8 = ucs2_to_utf8(text);
			if (group)
				group_utf8 = ucs2_to_utf8(group);
			if (secondtext)
				secondtext_utf8 = ucs2_to_utf8(secondtext);
			if (email)
				email_utf8 = ucs2_to_utf8(email);
			if (sip_uri)
				sip_uri_utf8 = ucs2_to_utf8(sip_uri);
			if (tel_uri)
				tel_uri_utf8 = ucs2_to_utf8(tel_uri);
		} else {
			text_utf8 = text;
			group_utf8 = group;
			secondtext_utf8 = secondtext;
			email_utf8 = email;
			sip_uri_utf8 = sip_uri;
			tel_uri_utf8 = tel_uri;
		}

		ofono_phonebook_entry(cbd->modem, index, number, type,
				text_utf8, hidden, group_utf8, adnumber,
				adtype, secondtext_utf8, email_utf8,
				sip_uri_utf8, tel_uri_utf8);

		if (at->pb->charset_current & CHARSET_UCS2) {
			g_free((char *)text_utf8);
			g_free((char *)group_utf8);
			g_free((char *)secondtext_utf8);
			g_free((char *)email_utf8);
			g_free((char *)sip_uri_utf8);
			g_free((char *)tel_uri_utf8);
		}
	}
}

static void at_read_entries_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	ofono_generic_cb_t cb = cbd->cb;

	if (at->pb->charset_current != at->pb->charset_origin)
		at_select_charset(modem, cb, modem);
	else {
		struct ofono_error error;
		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, cbd->data);
	}
}

static void at_read_entries(struct ofono_modem *modem, ofono_generic_cb_t cb,
				void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CPBR=%d,%d", at->pb->index_min, at->pb->index_max);
	if (g_at_chat_send_listing(at->parser, buf, entries_prefix,
					at_cpbr_notify, at_read_entries_cb,
					cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	if (at->pb->charset_origin != at->pb->charset_current)
		/* restore the charset */
		at_select_charset(modem, cb, modem);
	else {
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_select_charset_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	if (!ok)
		goto out;

	at->pb->charset_need_restore ^= 1;
	if (at->pb->charset_need_restore) {
		at_read_entries(modem, cb, modem);
		return;
	}

out:
	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
	return;
}

static void at_select_charset(struct ofono_modem *modem,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];
	const char *charset;

	if (!cbd)
		goto error;

	if (at->pb->charset_need_restore)
		charset = at->pb->charset_origin_str;
	else if (at->pb->charset_current == CHARSET_UTF8)
		charset = "UTF-8";
	else
		charset = "UCS2";

	sprintf(buf, "AT+CSCS=%s", charset);
	if (g_at_chat_send(at->parser, buf, none_prefix,
				at_select_charset_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		if (at->pb->charset_need_restore)
			ofono_error("Phonebook: character can't be restored!");
		cb(&error, data);
	}
}

static void at_list_indices_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	ofono_generic_cb_t cb = cbd->cb;
	GAtResultIter iter;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CPBR:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto error;

	/* retrieve index_min and index_max from indices
	 * which seems like "(1-150),32,16"
	 */
	if (!g_at_result_iter_next_range(&iter, &at->pb->index_min,
						&at->pb->index_max))
		goto error;

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	if (at->pb->charset_origin != at->pb->charset_current)
		at_select_charset(modem, cb, modem);
	else
		at_read_entries(modem, cb, modem);

	return;

error:
	{
		DECLARE_FAILURE(e);
		cb(&e, cbd->data);
	}
}

static void at_list_indices(struct ofono_modem *modem,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CPBR=?", entries_prefix,
				at_list_indices_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_select_storage_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	ofono_generic_cb_t cb = cbd->cb;

	if (!ok) {
		DECLARE_FAILURE(error);
		cb(&error, cbd->data);
		return;
	}

	at_list_indices(modem, cb, modem);
}

static void at_select_storage(struct ofono_modem *modem,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CPBS=%s", at->pb->storage);
	if (g_at_chat_send(at->parser, buf, none_prefix,
				at_select_storage_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, cbd->data);
	}
}

static void at_read_charset_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	ofono_generic_cb_t cb = cbd->cb;
	GAtResultIter iter;
	const char *charset;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		goto error;

	g_at_result_iter_next_string(&iter, &charset);
	at->pb->charset_origin_str = g_strdup(charset);
	if (!strcmp(charset, "UCS2"))
		at->pb->charset_origin = CHARSET_UCS2;
	else if (!strcmp(charset, "UTF-8"))
		at->pb->charset_origin = CHARSET_UTF8;

	if (at->pb->charset_origin & CHARSET_SUPPORT)
		at->pb->charset_current = at->pb->charset_origin;
	else if (at->pb->charset_list & CHARSET_UTF8)
		at->pb->charset_current = CHARSET_UTF8;
	else
		at->pb->charset_current = CHARSET_UCS2;

	at_select_storage(modem, cb, modem);
	return;

error:
	ofono_error("Phonebook: at_read_charset_cb failed");
	{
		DECLARE_FAILURE(error);
		cb(&error, cbd->data);
	}
}

static void at_read_charset(struct ofono_modem *modem,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (g_at_chat_send(at->parser, "AT+CSCS?", charset_prefix,
				at_read_charset_cb, cbd, g_free) > 0)
		return;

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static gboolean is_valid_charset_list(struct pb_data *pb)
{
	if (!(pb->charset_list & CHARSET_SUPPORT)) {
		ofono_error("Phonebook: not a valid charset_list");
		return FALSE;
	}

	return TRUE;
}

static void at_list_charsets_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	ofono_generic_cb_t cb = cbd->cb;
	GAtResultIter iter;
	const char *charset;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		goto error;

	while (g_at_result_iter_next_string(&iter, &charset)) {
		if (!strcmp(charset, "UTF-8"))
			at->pb->charset_list |= CHARSET_UTF8;
		else if (!strcmp(charset, "UCS2"))
			at->pb->charset_list |= CHARSET_UCS2;
	}

	if (!is_valid_charset_list(at->pb))
		goto error;

	at_read_charset(modem, cb, modem);
	return;

error:
	ofono_error("Phonebook: at_list_charsets_cb failed");
	{
		DECLARE_FAILURE(error);
		cb(&error, cbd->data);
	}
}

static void at_list_charsets(struct ofono_modem *modem,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (g_at_chat_send(at->parser, "AT+CSCS=?", charset_prefix,
				at_list_charsets_cb, cbd, g_free) > 0)
		return;

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_export_entries(struct ofono_modem *modem, const char *storage,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);

	at->pb->storage = storage;

	if (at->pb->has_charset_info) {
		if (!is_valid_charset_list(at->pb))
			goto error;
		at_select_storage(modem, cb, modem);
	} else {
		at->pb->has_charset_info = 1;
		at_list_charsets(modem, cb, data);
	}
	return;

error:
	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static struct ofono_phonebook_ops ops = {
	.export_entries		= at_export_entries
};

void at_phonebook_init(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	ofono_phonebook_register(modem, &ops);
	at->pb = phonebook_create();
}

void at_phonebook_exit(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	if (!at->pb)
		return;

	phonebook_destroy(at->pb);
	at->pb = NULL;

	ofono_phonebook_unregister(modem);
}
