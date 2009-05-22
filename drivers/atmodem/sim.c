/*
 *
 *  oFono - Open Source Telephony
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include "driver.h"

#include "gatchat.h"
#include "gatresult.h"

#include "at.h"

static const char *crsm_prefix[] = { "+CRSM:", NULL };
static const char *empty_prefix[] = { "", NULL };
static const char *cnum_prefix[] = { "+CNUM:", NULL };

static void at_crsm_len_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_file_len_cb_t cb = cbd->cb;
	gint sw1, len;
	struct ofono_error error;

	dump_response("at_crsm_len_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:")) {
		DECLARE_FAILURE(e);

		cb(&e, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &len);

	ofono_debug("crsm_len_cb: %i, %i", sw1, len);

	if (sw1 != 0x67) {
		DECLARE_FAILURE(e);

		cb(&e, -1, cbd->data);
		return;
	}

	cb(&error, len, cbd->data);
}

static void at_sim_read_file_len(struct ofono_modem *modem, int fileid,
					ofono_sim_file_len_cb_t cb,
					void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=176,%i,0,0,0", fileid);
	if (g_at_chat_send(at->parser, buf, crsm_prefix,
				at_crsm_len_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, -1, data);
	}
}

static void at_crsm_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct ofono_error error;
	const guint8 *response;
	gint sw1, sw2, len;

	dump_response("at_crsm_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, 0, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:")) {
		DECLARE_FAILURE(e);

		cb(&e, NULL, 0, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &sw2);
	if (!g_at_result_iter_next_hexstring(&iter, &response, &len) ||
		(sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		DECLARE_FAILURE(e);

		cb(&e, NULL, 0, cbd->data);
		return;
	}

	ofono_debug("crsm_cb: %02x, %02x, %d", sw1, sw2, len);

	cb(&error, response, len, cbd->data);
}

static void at_sim_read_file(struct ofono_modem *modem, int fileid, int start,
					int length, ofono_sim_read_cb_t cb,
					void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=176,%i,%i,%i,%i", fileid,
			start >> 8, start & 0xff, length);
	if (g_at_chat_send(at->parser, buf, crsm_prefix,
				at_crsm_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, 0, data);
	}
}

static void at_cimi_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_imsi_cb_t cb = cbd->cb;
	struct ofono_error error;
	const char *imsi;

	dump_response("at_cimi_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));
	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);
	g_at_result_iter_next(&iter, "");

	imsi = g_at_result_iter_raw_line(&iter);
	if (imsi == NULL) {
		DECLARE_FAILURE(e);
		cb(&e, NULL, cbd->data);
		return;
	}

	ofono_debug("cimi_cb: %s", imsi);

	cb(&error, imsi, cbd->data);
}

static void at_read_imsi(struct ofono_modem *modem, ofono_imsi_cb_t cb,
			void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CIMI", empty_prefix,
				at_cimi_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static void at_cnum_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_numbers_cb_t cb = cbd->cb;
	struct ofono_error error;
	struct ofono_own_number *numbers;
	GSList *l = NULL;
	int count;
	const char *str;

	dump_response("at_cnum_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (count = 0; g_at_result_iter_next(&iter, "+CNUM:"); count++);
	ofono_debug("Got %i elements", count);

	numbers = g_try_new0(struct ofono_own_number, count);
	if (!numbers) {
		DECLARE_FAILURE(e);
		cb(&e, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (count = 0; g_at_result_iter_next(&iter, "+CNUM:"); count++) {
		g_at_result_iter_skip_next(&iter);

		if (!g_at_result_iter_next_string(&iter, &str))
			continue;

		g_strlcpy(numbers[count].phone_number, str,
				OFONO_MAX_PHONE_NUMBER_LENGTH);

		g_at_result_iter_next_number(&iter,
				&numbers[count].number_type);

		l = g_slist_append(l, &numbers[count]);
	}

	cb(&error, l, cbd->data);

	g_free(numbers);
	g_slist_free(l);
}

static void at_read_msisdn(struct ofono_modem *modem, ofono_numbers_cb_t cb,
			void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CNUM", cnum_prefix,
				at_cnum_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static struct ofono_sim_ops ops = {
	.read_file_len 		= at_sim_read_file_len,
	.read_file 		= at_sim_read_file,
	.read_imsi		= at_read_imsi,
	.read_own_numbers	= at_read_msisdn,
};

void at_sim_init(struct ofono_modem *modem)
{
	ofono_sim_manager_register(modem, &ops);
}

void at_sim_exit(struct ofono_modem *modem)
{
	ofono_sim_manager_unregister(modem);
}
