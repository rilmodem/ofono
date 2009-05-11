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

static const char *none_prefix[] = { NULL };
static const char *clir_prefix[] = { "+CLIR:", NULL };
static const char *colp_prefix[] = { "+COLP:", NULL };
static const char *clip_prefix[] = { "+CLIP:", NULL };

static void clip_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_setting_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int status = -1;

	dump_response("clip_query_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLIP:")) {
		DECLARE_FAILURE(e);

		cb(&e, -1, cbd->data);
		return;
	}

	/* Skip the local presentation setting */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &status);

	ofono_debug("clip_query_cb: network: %d", status);

	cb(&error, status, cbd->data);
}

static void at_clip_query(struct ofono_modem *modem,
				ofono_call_setting_status_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CLIP?", clip_prefix,
				clip_query_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, -1, data);
	}
}

static void colp_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_setting_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int status;

	dump_response("colp_query_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COLP:")) {
		DECLARE_FAILURE(e);

		cb(&e, -1, cbd->data);
		return;
	}

	/* Skip the local presentation setting */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &status);

	ofono_debug("colp_query_cb: network: %d", status);

	cb(&error, status, cbd->data);
}

static void at_colp_query(struct ofono_modem *modem,
				ofono_call_setting_status_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+COLP?", colp_prefix,
				colp_query_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, -1, data);
	}
}

static void clir_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_clir_setting_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int override = 0, network = 2;

	dump_response("clir_query_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLIR:")) {
		DECLARE_FAILURE(e);

		cb(&e, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &override);
	g_at_result_iter_next_number(&iter, &network);

	ofono_debug("clir_query_cb: override: %d, network: %d",
			override, network);

	cb(&error, override, network, cbd->data);
}

static void at_clir_query(struct ofono_modem *modem,
				ofono_clir_setting_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CLIR?", clir_prefix,
				clir_query_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, -1, -1, data);
	}
}

static void clir_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("clir_set_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_clir_set(struct ofono_modem *modem, int mode,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CLIR=%d", mode);

	if (g_at_chat_send(at->parser, buf, none_prefix,
				clir_set_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static struct ofono_call_settings_ops ops = {
	.clip_query = at_clip_query,
	.colp_query = at_colp_query,
	.clir_query = at_clir_query,
	.clir_set = at_clir_set,
	.colr_query = NULL
};

void at_call_settings_init(struct ofono_modem *modem)
{
	ofono_call_settings_register(modem, &ops);
}

void at_call_settings_exit(struct ofono_modem *modem)
{
	ofono_call_settings_unregister(modem);
}
