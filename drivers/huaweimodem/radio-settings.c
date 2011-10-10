/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
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
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gatchat.h"
#include "gatresult.h"

#include "huaweimodem.h"

static const char *none_prefix[] = { NULL };
static const char *syscfg_prefix[] = { "^SYSCFG:", NULL };

#define HUAWEI_BAND_ANY 0x3FFFFFFF

struct radio_settings_data {
	GAtChat *chat;
};

static const struct huawei_band_gsm_table {
	enum ofono_radio_band_gsm band_gsm;
	unsigned int band_huawei;
} huawei_band_gsm_table[] = {
	{ OFONO_RADIO_BAND_GSM_ANY, 0x80000 | 0x200 | 0x100 | 0x80 | 0x200000 },
	{ OFONO_RADIO_BAND_GSM_850, 0x80000 },
	{ OFONO_RADIO_BAND_GSM_900P, 0x200 },
	{ OFONO_RADIO_BAND_GSM_900E, 0x100 },
	{ OFONO_RADIO_BAND_GSM_1800, 0x80 },
	{ OFONO_RADIO_BAND_GSM_1900, 0x200000 },
};

static const struct huawei_band_umts_table {
	enum ofono_radio_band_umts band_umts;
	unsigned int band_huawei;
} huawei_band_umts_table[] = {
	{ OFONO_RADIO_BAND_UMTS_ANY, 0x4000000 | 0x20000 | 800000 | 400000 },
	{ OFONO_RADIO_BAND_UMTS_850, 0x4000000 },
	{ OFONO_RADIO_BAND_UMTS_900, 0x20000 },
	{ OFONO_RADIO_BAND_UMTS_1900, 0x800000 },
	{ OFONO_RADIO_BAND_UMTS_2100, 0x400000 },
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static unsigned int band_gsm_to_huawei(enum ofono_radio_band_gsm band)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(huawei_band_gsm_table); i++) {
		if (huawei_band_gsm_table[i].band_gsm == band)
			return huawei_band_gsm_table[i].band_huawei;
	}

	return 0;
}

static unsigned int band_umts_to_huawei(enum ofono_radio_band_umts band)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(huawei_band_umts_table); i++) {
		if (huawei_band_umts_table[i].band_umts == band)
			return huawei_band_umts_table[i].band_huawei;
	}

	return 0;
}

static enum ofono_radio_band_gsm band_gsm_from_huawei(unsigned int band)
{
	size_t i;

	if (band == HUAWEI_BAND_ANY)
		return OFONO_RADIO_BAND_UMTS_ANY;

	for (i = ARRAY_SIZE(huawei_band_gsm_table) - 1; i > 0; i--) {
		if (huawei_band_gsm_table[i].band_huawei & band)
			return huawei_band_gsm_table[i].band_gsm;
	}

	return OFONO_RADIO_BAND_GSM_ANY;
}

static enum ofono_radio_band_umts band_umts_from_huawei(unsigned int band)
{
	size_t i;

	if (band == HUAWEI_BAND_ANY)
		return OFONO_RADIO_BAND_UMTS_ANY;

	for (i = ARRAY_SIZE(huawei_band_umts_table) - 1; i > 0; i--) {
		if (huawei_band_umts_table[i].band_huawei & band)
			return huawei_band_umts_table[i].band_umts;
	}

	return OFONO_RADIO_BAND_UMTS_ANY;
}

static void syscfg_query_mode_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	enum ofono_radio_access_mode mode;
	struct ofono_error error;
	GAtResultIter iter;
	int value;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "^SYSCFG:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &value) == FALSE)
		goto error;

	switch (value) {
	case 2:
		mode = OFONO_RADIO_ACCESS_MODE_ANY;
		break;
	case 13:
		mode = OFONO_RADIO_ACCESS_MODE_GSM;
		break;
	case 14:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	cb(&error, mode, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void huawei_query_rat_mode(struct ofono_radio_settings *rs,
			ofono_radio_settings_rat_mode_query_cb_t cb, void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(rsd->chat, "AT^SYSCFG?", syscfg_prefix,
				syscfg_query_mode_cb, cbd, g_free) == 0) {
		CALLBACK_WITH_FAILURE(cb, -1, data);
		g_free(cbd);
	}
}

static void syscfg_modify_mode_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void huawei_set_rat_mode(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[40];
	unsigned int value = 2, acq_order = 0;

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		value = 2;
		acq_order = 0;
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		value = 13;
		acq_order = 1;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		value = 14;
		acq_order = 2;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		goto error;
	}

	snprintf(buf, sizeof(buf), "AT^SYSCFG=%u,%u,40000000,2,4",
							value, acq_order);

	if (g_at_chat_send(rsd->chat, buf, none_prefix,
					syscfg_modify_mode_cb, cbd, g_free) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void syscfg_modify_band_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_band_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void huawei_set_band(struct ofono_radio_settings *rs,
					enum ofono_radio_band_gsm band_gsm,
					enum ofono_radio_band_umts band_umts,
					ofono_radio_settings_band_set_cb_t cb,
					void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[40];
	unsigned int huawei_band;

	if (band_gsm == OFONO_RADIO_BAND_GSM_ANY
			&& band_umts == OFONO_RADIO_BAND_UMTS_ANY) {
		huawei_band = HUAWEI_BAND_ANY;
	} else {
		unsigned int huawei_band_gsm;
		unsigned int huawei_band_umts;

		huawei_band_gsm = band_gsm_to_huawei(band_gsm);

		if (!huawei_band_gsm)
			goto error;

		huawei_band_umts = band_umts_to_huawei(band_umts);

		if (!huawei_band_umts)
			goto error;

		huawei_band = huawei_band_gsm | huawei_band_umts;
	}

	snprintf(buf, sizeof(buf), "AT^SYSCFG=16,3,%x,2,4", huawei_band);

	if (g_at_chat_send(rsd->chat, buf, none_prefix,
					syscfg_modify_band_cb, cbd, g_free) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void syscfg_query_band_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_band_query_cb_t cb = cbd->cb;
	enum ofono_radio_band_gsm band_gsm;
	enum ofono_radio_band_umts band_umts;
	struct ofono_error error;
	GAtResultIter iter;
	unsigned int band;
	const char *band_str;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "^SYSCFG:") == FALSE)
		goto error;

	if (g_at_result_iter_skip_next(&iter) == FALSE)
		goto error;

	if (g_at_result_iter_skip_next(&iter) == FALSE)
		goto error;

	if(g_at_result_iter_next_unquoted_string(&iter, &band_str) == FALSE)
		goto error;

	sscanf((const char *) band_str, "%x", &band);

	band_gsm = band_gsm_from_huawei(band);
	band_umts = band_umts_from_huawei(band);

	cb(&error, band_gsm, band_umts, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, cbd->data);
}

static void huawei_query_band(struct ofono_radio_settings *rs,
		ofono_radio_settings_band_query_cb_t cb, void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(rsd->chat, "AT^SYSCFG?", syscfg_prefix,
				syscfg_query_band_cb, cbd, g_free) == 0) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, data);
		g_free(cbd);
	}
}

static void syscfg_support_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;

	if (!ok) {
		ofono_radio_settings_remove(rs);
		return;
	}

	ofono_radio_settings_register(rs);
}

static int huawei_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct radio_settings_data *rsd;

	rsd = g_try_new0(struct radio_settings_data, 1);
	if (rsd == NULL)
		return -ENOMEM;

	rsd->chat = g_at_chat_clone(chat);

	ofono_radio_settings_set_data(rs, rsd);

	g_at_chat_send(rsd->chat, "AT^SYSCFG=?", syscfg_prefix,
					syscfg_support_cb, rs, NULL);

	return 0;
}

static void huawei_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);

	ofono_radio_settings_set_data(rs, NULL);

	g_at_chat_unref(rsd->chat);
	g_free(rsd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= "huaweimodem",
	.probe			= huawei_radio_settings_probe,
	.remove			= huawei_radio_settings_remove,
	.query_rat_mode		= huawei_query_rat_mode,
	.set_rat_mode		= huawei_set_rat_mode,
	.query_band             = huawei_query_band,
	.set_band               = huawei_set_band,
};

void huawei_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void huawei_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
