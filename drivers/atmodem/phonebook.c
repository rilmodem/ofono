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

static void at_read_entries_cb(gboolean ok, GAtResult *result,
			       gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_phonebook_export_entries_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int num_entries_max = 0;
	int num_entries_real;
	int num;
	struct ofono_phonebook_entry *entries;

	decode_at_error(&error, g_at_result_final_response(result));
	if (!ok) {
		cb(&error, 0, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CPBR:"))
		num_entries_max += 1;

	entries = g_new(struct ofono_phonebook_entry, num_entries_max);

	g_at_result_iter_init(&iter, result);
	for (num_entries_real = 0; g_at_result_iter_next(&iter, "+CPBR:");) {
		int index, type, hidden, adtype;
		const char *number, *text, *group, *adnumber,
			*secondtext, *email, *sip_uri, *tel_uri;

		if (!g_at_result_iter_next_number(&iter, &index))
			continue;
		entries[num_entries_real].index = index;

		if (!g_at_result_iter_next_string(&iter, &number))
			continue;
		entries[num_entries_real].number = g_strdup(number);

		if (!g_at_result_iter_next_number(&iter, &type)) {
			g_free(entries[num_entries_real].number);
			continue;
		}
		entries[num_entries_real].type = type;

		if (!g_at_result_iter_next_string(&iter, &text)) {
			g_free(entries[num_entries_real].number);
			continue;
		}
		entries[num_entries_real].text = g_strdup(text);

		if (!g_at_result_iter_next_number(&iter, &hidden))
			hidden = -1;
		entries[num_entries_real].hidden = hidden;

		if (!g_at_result_iter_next_string(&iter, &group))
			group = NULL;
		entries[num_entries_real].group = g_strdup(group);

		if (!g_at_result_iter_next_string(&iter, &adnumber))
			adnumber = NULL;
		entries[num_entries_real].adnumber = g_strdup(adnumber);

		if (!g_at_result_iter_next_number(&iter, &adtype))
			adtype = -1;
		entries[num_entries_real].adtype = adtype;

		if (!g_at_result_iter_next_string(&iter, &secondtext))
			secondtext = NULL;
		entries[num_entries_real].secondtext = g_strdup(secondtext);

		if (!g_at_result_iter_next_string(&iter, &email))
			email = NULL;
		entries[num_entries_real].email = g_strdup(email);

		if (!g_at_result_iter_next_string(&iter, &sip_uri))
			sip_uri = NULL;
		entries[num_entries_real].sip_uri = g_strdup(sip_uri);

		if (!g_at_result_iter_next_string(&iter, &tel_uri))
			tel_uri = NULL;
		entries[num_entries_real].tel_uri = g_strdup(tel_uri);

		num_entries_real++;
	}
	cb(&error, num_entries_real, entries, cbd->data);

	for (num = 0; num < num_entries_real; num++) {
		g_free(entries[num].number);
		g_free(entries[num].text);
		g_free(entries[num].group);
		g_free(entries[num].adnumber);
		g_free(entries[num].secondtext);
		g_free(entries[num].email);
		g_free(entries[num].sip_uri);
		g_free(entries[num].tel_uri);
	}
	g_free(entries);
	entries = NULL;
}

static void at_read_entries(struct ofono_modem *modem, int index_min,
			    int index_max, ofono_phonebook_export_entries_t cb,
			    void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CPBR=%d,%d", index_min, index_max);
	if (g_at_chat_send(at->parser, buf, entries_prefix,
				at_read_entries_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, 0, NULL, data);
	}
}

static void at_list_indices_cb(gboolean ok, GAtResult *result,
			       gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	ofono_phonebook_export_entries_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	const char *indices;
	int index_min = 0, index_max = 0;
	int i = 0, integer_index = 0;

	decode_at_error(&error, g_at_result_final_response(result));
	if (!ok) {
		cb(&error, 0, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CPBR:")) {
		DECLARE_FAILURE(e);
		cb(&e, 0, NULL, cbd->data);
		return;
	}

	indices = g_at_result_iter_raw_line(&iter);
	/* retrieve index_min and index_max from indices
	 * which seems like "(1-150),32,16"
	 */
	while (indices[i] != ')') {
		if (indices[i] == '(')
			integer_index = 1;
		else if (indices[i] == '-')
			integer_index = 2;
		if ((indices[i] >= '0') && (indices[i] <= '9')) {
			if (integer_index == 1)
				index_min = index_min*10 + (indices[i] - '0');
			else if (integer_index == 2)
				index_max = index_max*10 + (indices[i] - '0');
		}
		i++;
	}

	at_read_entries(modem, index_min, index_max, cb, modem);
}

static void at_list_indices(struct ofono_modem *modem,
			ofono_phonebook_export_entries_t cb, void *data)
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
		cb(&error, 0, NULL, data);
	}
}

static void at_select_storage_cb(gboolean ok, GAtResult *result,
				 gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	ofono_phonebook_export_entries_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	if (!ok) {
		cb(&error, 0, NULL, cbd->data);
		return;
	} else
		at_list_indices(modem, cb, modem);
}

static void at_select_storage(struct ofono_modem *modem, char *storage,
			ofono_phonebook_export_entries_t cb, void *data)
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
		cb(&error, 0, NULL, data);
	}
}

static void at_export_entries(struct ofono_modem *modem, char *storage,
			ofono_phonebook_export_entries_t cb, void *data)
{
	at_select_storage(modem, storage, cb, modem);
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
