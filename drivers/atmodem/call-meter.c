/*
 *
 *  oFono - Open Source Telephony
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-meter.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *none_prefix[] = { NULL };
static const char *caoc_prefix[] = { "+CAOC:", NULL };
static const char *cacm_prefix[] = { "+CACM:", NULL };
static const char *camm_prefix[] = { "+CAMM:", NULL };
static const char *cpuc_prefix[] = { "+CPUC:", NULL };

static void caoc_cacm_camm_query_cb(gboolean ok,
		GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_meter_query_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	const char *meter_hex;
	char *end;
	int meter;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, cbd->user))
		goto error;

	if (g_at_result_iter_next_string(&iter, &meter_hex) == FALSE)
		goto error;

	meter = strtol(meter_hex, &end, 16);
	if (*end)
		goto error;

	cb(&error, meter, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void cccm_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_call_meter *cm = user_data;
	GAtResultIter iter;
	const char *meter_hex;
	char *end;
	int meter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CCCM:"))
		return;

	if (g_at_result_iter_next_string(&iter, &meter_hex) == FALSE)
		goto error;

	meter = strtol(meter_hex, &end, 16);
	if (*end)
		goto error;

	ofono_call_meter_changed_notify(cm, meter);
	return;

error:
	ofono_error("Invalid CCCM value");
}

static void at_caoc_query(struct ofono_call_meter *cm,
				ofono_call_meter_query_cb_t cb,
				void *data)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = "+CAOC:";
	if (g_at_chat_send(chat, "AT+CAOC=0", caoc_prefix,
				caoc_cacm_camm_query_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void at_cacm_query(struct ofono_call_meter *cm,
				ofono_call_meter_query_cb_t cb,
				void *data)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = "+CACM:";
	if (g_at_chat_send(chat, "AT+CACM?", cacm_prefix,
				caoc_cacm_camm_query_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void generic_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_meter_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_cacm_set(struct ofono_call_meter *cm, const char *passwd,
			ofono_call_meter_set_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	snprintf(buf, sizeof(buf), "AT+CACM=\"%s\"", passwd);

	if (g_at_chat_send(chat, buf, none_prefix,
				generic_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_camm_query(struct ofono_call_meter *cm,
				ofono_call_meter_query_cb_t cb,
				void *data)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = "+CAMM:";
	if (g_at_chat_send(chat, "AT+CAMM?", camm_prefix,
				caoc_cacm_camm_query_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void at_camm_set(struct ofono_call_meter *cm,
			int accmax, const char *passwd,
			ofono_call_meter_set_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	snprintf(buf, sizeof(buf), "AT+CAMM=\"%06X\",\"%s\"", accmax, passwd);

	if (g_at_chat_send(chat, buf, none_prefix,
				generic_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void cpuc_query_cb(gboolean ok,
				GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_meter_puct_query_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	const char *currency, *ppu;
	char currency_buf[64];
	double ppuval;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, 0, 0, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, cbd->user) != TRUE)
		goto error;

	if (g_at_result_iter_next_string(&iter, &currency) != TRUE)
		goto error;

	strncpy(currency_buf, currency, sizeof(currency_buf));

	if (g_at_result_iter_next_string(&iter, &ppu) != TRUE)
		goto error;

	ppuval = strtod(ppu, NULL);

	cb(&error, currency_buf, ppuval, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, 0, cbd->data);
}

static void at_cpuc_query(struct ofono_call_meter *cm,
				ofono_call_meter_puct_query_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = "+CPUC:";
	if (g_at_chat_send(chat, "AT+CPUC?", cpuc_prefix,
				cpuc_query_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, 0, 0, data);
}

static void at_cpuc_set(struct ofono_call_meter *cm, const char *currency,
			double ppu, const char *passwd,
			ofono_call_meter_set_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	snprintf(buf, sizeof(buf), "AT+CPUC=\"%s\",\"%f\",\"%s\"",
			currency, ppu, passwd);

	if (g_at_chat_send(chat, buf, none_prefix,
				generic_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ccwv_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_call_meter *cm = user_data;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CCWV"))
		return;

	ofono_call_meter_maximum_notify(cm);
}

static void at_call_meter_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_call_meter *cm = user_data;
	GAtChat *chat = ofono_call_meter_get_data(cm);

	g_at_chat_register(chat, "+CCCM:", cccm_notify, FALSE, cm, NULL);
	g_at_chat_register(chat, "+CCWV", ccwv_notify, FALSE, cm, NULL);

	ofono_call_meter_register(cm);
}

static int at_caoc_probe(struct ofono_call_meter *cm, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;

	chat = g_at_chat_clone(chat);
	ofono_call_meter_set_data(cm, chat);

	g_at_chat_send(chat, "AT+CAOC=2", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CCWE=1", NULL,
			at_call_meter_initialized, cm, NULL);

	return 0;
}

static void at_caoc_remove(struct ofono_call_meter *cm)
{
	GAtChat *chat = ofono_call_meter_get_data(cm);

	g_at_chat_unref(chat);
	ofono_call_meter_set_data(cm, NULL);
}

static struct ofono_call_meter_driver driver = {
	.name = "atmodem",
	.probe = at_caoc_probe,
	.remove = at_caoc_remove,
	.call_meter_query = at_caoc_query,
	.acm_query = at_cacm_query,
	.acm_reset = at_cacm_set,
	.acm_max_query = at_camm_query,
	.acm_max_set = at_camm_set,
	.puct_query = at_cpuc_query,
	.puct_set = at_cpuc_set,
};

void at_call_meter_init(void)
{
	ofono_call_meter_driver_register(&driver);
}

void at_call_meter_exit(void)
{
	ofono_call_meter_driver_unregister(&driver);
}
