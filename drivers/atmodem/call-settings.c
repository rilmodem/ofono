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
#include <ofono/call-settings.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *none_prefix[] = { NULL };
static const char *clir_prefix[] = { "+CLIR:", NULL };
static const char *colp_prefix[] = { "+COLP:", NULL };
static const char *clip_prefix[] = { "+CLIP:", NULL };
static const char *ccwa_prefix[] = { "+CCWA:", NULL };

static void ccwa_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	int conditions = 0;
	int status;
	int cls;
	struct ofono_error error;
	GAtResultIter iter;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto out;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CCWA:")) {
		g_at_result_iter_next_number(&iter, &status);
		g_at_result_iter_next_number(&iter, &cls);

		if (status == 1)
			conditions |= cls;
	}

	DBG("CW enabled for: %d", conditions);

out:
	cb(&error, conditions, cbd->data);
}

static void at_ccwa_query(struct ofono_call_settings *cs, int cls,
				ofono_call_settings_status_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	cbd->user = GINT_TO_POINTER(cls);

	if (cls == 7)
		snprintf(buf, sizeof(buf), "AT+CCWA=1,2");
	else
		snprintf(buf, sizeof(buf), "AT+CCWA=1,2,%d", cls);

	if (g_at_chat_send(chat, buf, ccwa_prefix,
				ccwa_query_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, 0, data);
}

static void ccwa_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_ccwa_set(struct ofono_call_settings *cs, int mode, int cls,
				ofono_call_settings_set_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CCWA=1,%d,%d", mode, cls);

	if (g_at_chat_send(chat, buf, none_prefix,
				ccwa_set_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}


static void clip_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int status = -1;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLIP:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	/* Skip the local presentation setting */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &status);

	DBG("clip_query_cb: network: %d", status);

	cb(&error, status, cbd->data);
}

static void at_clip_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, "AT+CLIP?", clip_prefix,
				clip_query_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void colp_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int status;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COLP:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	/* Skip the local presentation setting */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &status);

	DBG("colp_query_cb: network: %d", status);

	cb(&error, status, cbd->data);
}

static void at_colp_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, "AT+COLP?", colp_prefix,
				colp_query_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void clir_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_clir_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int override = 0, network = 2;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLIR:")) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &override);
	g_at_result_iter_next_number(&iter, &network);

	DBG("clir_query_cb: override: %d, network: %d", override, network);

	cb(&error, override, network, cbd->data);
}

static void at_clir_query(struct ofono_call_settings *cs,
				ofono_call_settings_clir_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, "AT+CLIR?", clir_prefix,
				clir_query_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, -1, data);
}

static void clir_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_clir_set(struct ofono_call_settings *cs, int mode,
				ofono_call_settings_set_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CLIR=%d", mode);

	if (g_at_chat_send(chat, buf, none_prefix,
				clir_set_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean at_call_settings_register(gpointer user)
{
	struct ofono_call_settings *cs = user;

	ofono_call_settings_register(cs);

	return FALSE;
}

static int at_call_settings_probe(struct ofono_call_settings *cs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;

	ofono_call_settings_set_data(cs, g_at_chat_clone(chat));
	g_idle_add(at_call_settings_register, cs);

	return 0;
}

static void at_call_settings_remove(struct ofono_call_settings *cs)
{
	GAtChat *chat = ofono_call_settings_get_data(cs);

	g_at_chat_unref(chat);
	ofono_call_settings_set_data(cs, NULL);
}

static struct ofono_call_settings_driver driver = {
	.name = "atmodem",
	.probe = at_call_settings_probe,
	.remove = at_call_settings_remove,
	.clip_query = at_clip_query,
	.colp_query = at_colp_query,
	.clir_query = at_clir_query,
	.clir_set = at_clir_set,
	.colr_query = NULL,
	.cw_query = at_ccwa_query,
	.cw_set = at_ccwa_set,
};

void at_call_settings_init()
{
	ofono_call_settings_driver_register(&driver);
}

void at_call_settings_exit()
{
	ofono_call_settings_driver_unregister(&driver);
}
