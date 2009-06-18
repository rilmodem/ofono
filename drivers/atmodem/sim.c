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
static const char *cnum_prefix[] = { "+CNUM:", NULL };

static void at_crsm_info_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct ofono_error error;
	const guint8 *response;
	gint sw1, sw2, len;
	int flen, rlen;
	enum ofono_sim_file_structure str;

	dump_response("at_crsm_info_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:")) {
		DECLARE_FAILURE(e);

		cb(&e, -1, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &sw2);

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len) ||
		(sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92) ||
		(sw1 == 0x90 && sw2 != 0x00) ||
		len < 14 || response[6] != 0x04 ||
		(response[13] == 0x01 && len < 15)) {
		DECLARE_FAILURE(e);

		cb(&e, -1, -1, -1, cbd->data);
		return;
	}

	ofono_debug("crsm_info_cb: %02x, %02x, %i", sw1, sw2, len);

	flen = (response[2] << 8) | response[3];
	str = response[13];
	if (str == 0x01)
		rlen = response[14];
	else
		rlen = 0;

	cb(&error, flen, str, rlen, cbd->data);
}

static void at_sim_read_info(struct ofono_modem *modem, int fileid,
					ofono_sim_file_info_cb_t cb,
					void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=192,%i,0,0,15", fileid);
	if (g_at_chat_send(at->parser, buf, crsm_prefix,
				at_crsm_info_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, -1, -1, -1, data);
	}
}

static void at_crsm_read_cb(gboolean ok, GAtResult *result,
		gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct ofono_error error;
	const guint8 *response;
	gint sw1, sw2, len;

	dump_response("at_crsm_read_cb", ok, result);
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
		(sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		DECLARE_FAILURE(e);

		cb(&e, NULL, 0, cbd->data);
		return;
	}

	ofono_debug("crsm_read_cb: %02x, %02x, %d", sw1, sw2, len);

	cb(&error, response, len, cbd->data);
}

static void at_sim_read_binary(struct ofono_modem *modem, int fileid,
					int start, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=176,%i,%i,%i,%i", fileid,
			start >> 8, start & 0xff, length);
	if (g_at_chat_send(at->parser, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, 0, data);
	}
}

static void at_sim_read_record(struct ofono_modem *modem, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=178,%i,%i,4,%i", fileid,
			record + 1, length);
	if (g_at_chat_send(at->parser, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, 0, data);
	}
}

static void at_crsm_update_cb(gboolean ok, GAtResult *result,
		gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;
	gint sw1, sw2;

	dump_response("at_crsm_update_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:")) {
		DECLARE_FAILURE(e);

		cb(&e, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &sw2);
	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		DECLARE_FAILURE(e);

		cb(&e, cbd->data);
		return;
	}

	ofono_debug("crsm_update_cb: %02x, %02x", sw1, sw2);

	cb(&error, cbd->data);
}

static void at_sim_update_binary(struct ofono_modem *modem, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=214,%i,%i,%i,%i,", fileid,
			start >> 8, start & 0xff, length);
	for (; length; length--)
		len += sprintf(buf + len, "%02hhx", *value++);
	ret = g_at_chat_send(at->parser, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_sim_update_record(struct ofono_modem *modem, int fileid,
					int record, int length,
					const unsigned char *value,
					ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=220,%i,%i,4,%i,", fileid,
			record + 1, length);
	for (; length; length--)
		len += sprintf(buf + len, "%02hhx", *value++);
	ret = g_at_chat_send(at->parser, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_cimi_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_imsi_cb_t cb = cbd->cb;
	struct ofono_error error;
	const char *imsi;
	int i;

	dump_response("at_cimi_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));
	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (i = 0; i < g_at_result_num_response_lines(result); i++)
		g_at_result_iter_next(&iter, NULL);

	imsi = g_at_result_iter_raw_line(&iter);

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

	if (g_at_chat_send(at->parser, "AT+CIMI", NULL,
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
	ofono_own_numbers_cb_t cb = cbd->cb;
	struct ofono_error error;
	struct ofono_own_number *numbers;
	int count;
	const char *str;

	dump_response("at_cnum_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, 0, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (count = 0; g_at_result_iter_next(&iter, "+CNUM:"); count++);
	ofono_debug("Got %i elements", count);

	numbers = g_try_new0(struct ofono_own_number, count);
	if (!numbers) {
		DECLARE_FAILURE(e);
		cb(&e, 0, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	count = 0;
	while (g_at_result_iter_next(&iter, "+CNUM")) {
		/* Skip alnum */
		g_at_result_iter_skip_next(&iter);

		if (!g_at_result_iter_next_string(&iter, &str))
			continue;

		g_strlcpy(numbers[count].phone_number.number,
				str[0] == '+' ? str+1 : str,
				OFONO_MAX_PHONE_NUMBER_LENGTH);

		g_at_result_iter_next_number(&iter,
					&numbers[count].phone_number.type);

		numbers[count].speed = -1;
		numbers[count].service = -1;
		numbers[count].itc = -1;
		numbers[count].npi = -1;

		g_at_result_iter_skip_next(&iter);
		g_at_result_iter_next_number(&iter, &numbers[count].service);
		g_at_result_iter_next_number(&iter, &numbers[count].itc);

		count++;
	}

	cb(&error, count, numbers, cbd->data);

	g_free(numbers);
}

static void at_read_msisdn(struct ofono_modem *modem, ofono_own_numbers_cb_t cb,
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
		cb(&error, 0, NULL, data);
	}
}

static struct ofono_sim_ops ops = {
	.read_file_info		= at_sim_read_info,
	.read_file_transparent	= at_sim_read_binary,
	.read_file_linear	= at_sim_read_record,
	.write_file_transparent	= at_sim_update_binary,
	.write_file_linear	= at_sim_update_record,
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
