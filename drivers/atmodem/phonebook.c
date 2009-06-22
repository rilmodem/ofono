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

static const char *none_prefix[] = { NULL };
static const char *entries_prefix[] = { "+CPBR:", NULL };

static void at_cpbr_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CPBR:")) {
		int index;
		const char *number;
		int type;
		const char *text;
		int hidden = -1;
		const char *group = NULL;
		const char *adnumber = NULL;
		int adtype = -1;
		const char *secondtext = NULL;
		const char *email = NULL;
		const char *sip_uri = NULL;
		const char *tel_uri = NULL;

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

		ofono_phonebook_entry(cbd->modem, index, number, type, text,
					hidden, group, adnumber, adtype,
					secondtext, email, sip_uri, tel_uri);
	}
}

static void at_read_entries_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void at_read_entries(struct ofono_modem *modem, int index_min,
				int index_max, ofono_generic_cb_t cb,
				void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CPBR=%d,%d", index_min, index_max);
	if (g_at_chat_send_listing(at->parser, buf, entries_prefix,
					at_cpbr_notify, at_read_entries_cb,
					cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_list_indices_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	ofono_generic_cb_t cb = cbd->cb;
	GAtResultIter iter;
	int index_min;
	int index_max;

	if (!ok) {
		struct ofono_error error;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CPBR:"))
		goto fail;

	if (!g_at_result_iter_open_list(&iter))
		goto fail;

	/* retrieve index_min and index_max from indices
	 * which seems like "(1-150),32,16"
	 */
	if (!g_at_result_iter_next_range(&iter, &index_min, &index_max))
		goto fail;

	if (!g_at_result_iter_close_list(&iter))
		goto fail;

	at_read_entries(modem, index_min, index_max, cb, modem);

	return;

fail:
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
		struct ofono_error error;
		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, cbd->data);
		return;
	}

	at_list_indices(modem, cb, modem);
}

static void at_export_entries(struct ofono_modem *modem, const char *storage,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CPBS=%s", storage);
	if (g_at_chat_send(at->parser, buf, none_prefix,
				at_select_storage_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

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
	ofono_phonebook_register(modem, &ops);
}

void at_phonebook_exit(struct ofono_modem *modem)
{
	ofono_phonebook_unregister(modem);
}
