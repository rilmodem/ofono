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
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *cgact_prefix[] = { "+CGACT:", NULL };
static const char *none_prefix[] = { NULL };

struct gprs_context_data {
	GAtChat *chat;
	unsigned active_context;
};

static void at_cgact_down_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	if (ok)
		gcd->active_context = 0;

	dump_response("cgact_down_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_cgact_up_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("cgact_up_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *ncbd;
	char buf[64];

	dump_response("cgdcont_cb", ok, result);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, cbd->data);
		return;
	}

	ncbd = g_memdup(cbd, sizeof(struct cb_data));

	sprintf(buf, "AT+CGACT=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgact_up_cb, ncbd, g_free) > 0)
		return;

	if (ncbd)
		g_free(ncbd);

	gcd->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void at_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len;

	if (!cbd)
		goto error;

	gcd->active_context = ctx->cid;

	cbd->user = gc;

	/* TODO: Handle username / password fields */
	len = sprintf(buf, "AT+CGDCONT=%u,\"IP\"", ctx->cid);

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
				ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgdcont_cb, cbd, g_free) > 0)
		return;
error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int id,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	cbd->user = gc;

	sprintf(buf, "AT+CGACT=0,%u", id);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgact_down_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_cgact_read_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	gint cid, state;
	GAtResultIter iter;

	dump_response("cgact_read_cb", ok, result);

	if (!ok)
		return;

	while (g_at_result_iter_next(&iter, "+CGACT:")) {
		if (!g_at_result_iter_next_number(&iter, &cid))
			continue;

		if ((unsigned int) cid != gcd->active_context)
			continue;

		if (!g_at_result_iter_next_number(&iter, &state))
			continue;

		if (state == 1)
			continue;

		ofono_gprs_context_deactivated(gc, gcd->active_context);
		gcd->active_context = 0;

		break;
	}
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	const char *event;

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_has_prefix(event, "NW REACT ") ||
			g_str_has_prefix(event, "NW DEACT ") ||
			g_str_has_prefix(event, "ME DEACT ")) {
		/* Ask what primary contexts are active now */
		g_at_chat_send(gcd->chat, "AT+CGACT?", cgact_prefix,
				at_cgact_read_cb, gc, NULL);

		return;
	}
}

static int at_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;

	gcd = g_new0(struct gprs_context_data, 1);
	gcd->chat = chat;

	g_at_chat_register(gcd->chat, "+CGEV:", cgev_notify, FALSE, gc, NULL);

	ofono_gprs_context_set_data(gc, gcd);

	return 0;
}

static void at_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	ofono_gprs_context_set_data(gc, NULL);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "atmodem",
	.probe			= at_gprs_context_probe,
	.remove			= at_gprs_context_remove,
	.activate_primary	= at_gprs_activate_primary,
	.deactivate_primary	= at_gprs_deactivate_primary,
};

void at_gprs_context_init()
{
	ofono_gprs_context_driver_register(&driver);
}

void at_gprs_context_exit()
{
	ofono_gprs_context_driver_unregister(&driver);
}
