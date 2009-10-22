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
#include <ofono/gprs.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *cgdcont_prefix[] = { "+CGDCONT:", NULL };
static const char *none_prefix[] = { NULL };

struct gprs_data {
	GAtChat *chat;
};

static void at_cgatt_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("cgatt_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CGATT=%i", attached ? 1 : 0);

	if (g_at_chat_send(gd->chat, buf, none_prefix,
				at_cgatt_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct set_active_req *req = user_data;
	struct gprs_data *gd =
		ofono_gprs_get_data(req->gprs);
	struct ofono_error error;
	char buf[64];

	dump_response("cgdcont_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		req->cb(&error, req->data);

		g_free(req);
		return;
	}

	/* Context is no longer undefined */
	gd->new_contexts = g_slist_remove(gd->new_contexts, req->ctx);

	sprintf(buf, "AT+CGACT=1,%u", req->ctx->id);

	if (g_at_chat_send(gd->chat, buf, none_prefix,
				at_cgact_cb, req, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(req->cb, req->data);

	g_free(req);
}

static void at_pdp_set_active(struct ofono_gprs *gprs, unsigned id,
				int active, ofono_gprs_cb_t cb,
				void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct set_active_req *req = NULL;
	char buf[64];
	struct ofono_gprs_primary_context *ctx;
	gint cid = id;
	int len;
	GSList *l;

	l = g_slist_find_custom(gd->contexts, &cid, context_id_compare);
	if (!l)
		goto error;

	ctx = l->data;

	req = g_new0(struct set_active_req, 1);
	if (!req)
		goto error;

	req->gprs = gprs;
	req->ctx = ctx;
	req->active = active;
	req->cb = cb;
	req->data = data;

	if (active) {
		len = sprintf(buf, "AT+CGDCONT=%u,\"IP\"", id);
		if (ctx->apn)
			snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
					ctx->apn);

		if (g_at_chat_send(gd->chat, buf, none_prefix,
					at_cgdcont_cb, req, NULL) > 0)
			return;
	} else {
		sprintf(buf, "AT+CGACT=0,%u", id);

		if (g_at_chat_send(gd->chat, buf, none_prefix,
					at_cgact_cb, req, g_free) > 0)
			return;
	}

error:
	if (req)
		g_free(req);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_pdp_set_active_all(struct ofono_gprs *gprs,
				int active, ofono_gprs_cb_t cb,
				void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct set_active_req *req;
	char buf[64];

	req = g_new0(struct set_active_req, 1);
	if (!req)
		goto error;

	req->gprs = gprs;
	req->active = active;
	req->cb = cb;
	req->data = data;

	sprintf(buf, "AT+CGACT=%i", active ? 1 : 0);

	if (g_at_chat_send(gd->chat, buf, none_prefix,
				at_cgact_cb, req, g_free) > 0)
		return;

error:
	if (req)
		g_free(req);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_pdp_alloc(struct ofono_gprs *gprs,
				ofono_gprs_alloc_cb_t cb,
				void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ofono_gprs_primary_context *ctx;
	struct ofono_error e;
	unsigned id = find_next_primary_id(gd);

	if (!id) {
		CALLBACK_WITH_FAILURE(cb, NULL, data);

		return;
	}

	ctx = g_try_new0(struct ofono_gprs_primary_context, 1);
	if (!ctx) {
		CALLBACK_WITH_FAILURE(cb, NULL, data);

		return;
	}

	ctx->id = id;
	ctx->apn = g_strdup("");
	ctx->username = g_strdup("");
	ctx->password = g_strdup("");

	gd->new_contexts = g_slist_insert_sorted(gd->new_contexts,
						ctx, context_compare);
	gd->contexts = g_slist_insert_sorted(gd->contexts,
						ctx, context_compare);

	/* The context will be defined (+CGDCONT) lazily, once it's needed
	 * and the parameters are already set in ctx.  Right now just call
	 * back */
	e.type = OFONO_ERROR_TYPE_NO_ERROR;
	e.error = 0;
	cb(&e, ctx, data);

	ofono_gprs_notify(gprs, ctx);
}

static void at_pdp_undefine_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	dump_response("undefine_cb", ok, result);

	if (!ok)
		ofono_error("Undefining primary context failed");
}

static void at_pdp_free(struct ofono_gprs *gprs, unsigned id,
			ofono_gprs_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ofono_error e;
	char buf[64];
	struct ofono_gprs_primary_context *ctx;
	GSList *l;
	gint cid = id;

	l = g_slist_find_custom(gd->contexts, &cid, context_id_compare);
	if (!l) {
		CALLBACK_WITH_FAILURE(cb, data);

		return;
	}

	ctx = l->data;
	if (ctx->active) {
		CALLBACK_WITH_FAILURE(cb, data);

		return;
	}

	/* We can call back already -- even if the request to undefine
	 * the context fails, the ID can be re-used.  */
	e.type = OFONO_ERROR_TYPE_NO_ERROR;
	e.error = 0;
	cb(&e, data);

	context_free(ctx);
	gd->contexts = g_slist_remove(gd->contexts, ctx);

	if (g_slist_find(gd->new_contexts, ctx)) {
		gd->new_contexts = g_slist_remove(gd->new_contexts, ctx);
		return;
	}

	sprintf(buf, "AT+CGDCONT=%u", id);

	g_at_chat_send(gd->chat, buf, none_prefix,
			at_pdp_undefine_cb, NULL, NULL);
}

static void at_cgact_read_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	gint cid, state;
	GAtResultIter iter;
	struct ofono_gprs_primary_context *ctx;
	GSList *l;

	dump_response("cgact_read_cb", ok, result);

	if (!ok)
		return;

	while (g_at_result_iter_next(&iter, "+CGACT:")) {
		if (!g_at_result_iter_next_number(&iter, &cid))
			continue;

		if (!g_at_result_iter_next_number(&iter, &state))
			continue;

		l = g_slist_find_custom(gd->contexts, &cid,
					context_id_compare);
		if (!l)
			continue;

		ctx = l->data;
		if (ctx->active != state) {
			ctx->active = state;

			if (state)
				continue;

			ofono_gprs_deactivated(gprs, ctx->id);
		}
	}
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GAtResultIter iter;
	const char *event;

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_has_prefix(event, "REJECT "))
		return;

	if (g_str_has_prefix(event, "NW REACT ") ||
			g_str_has_prefix(event, "NW DEACT ") ||
			g_str_has_prefix(event, "ME DEACT ")) {
		/* Ask what primary contexts are active now */
		g_at_chat_send(gd->chat, "AT+CGACT?", cgact_prefix,
				at_cgact_read_cb, gprs, NULL);

		return;
	}

	if (g_str_has_prefix(event, "NW DETACH ") ||
			g_str_has_prefix(event, "ME DETACH ")) {
		detached(gprs);

		ofono_gprs_detached(gprs);

		return;
	}

	if (g_str_has_prefix(event, "NW CLASS ") ||
			g_str_has_prefix(event, "ME CLASS "))
		return;
}

static void cgreg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	gint status, tech = -1;
	int lac = -1, ci = -1;
	const char *str;

	dump_response("cgreg_notify", TRUE, result);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGREG:"))
		return;

	g_at_result_iter_next_number(&iter, &status);

	if (g_at_result_iter_next_string(&iter, &str))
		lac = strtol(str, NULL, 16);
	else
		goto out;

	if (g_at_result_iter_next_string(&iter, &str))
		ci = strtol(str, NULL, 16);
	else
		goto out;

	g_at_result_iter_next_number(&iter, &tech);

out:
	ofono_debug("cgreg_notify: %d, %d, %d, %d", status, lac, ci, tech);

	if (status != 1 && status != 5)
		detached(gprs);

	ofono_gprs_status_notify(gprs, status, lac, ci, tech);
}

static void at_cgdcont_test_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GAtResultIter iter;
	gint range[2];
	GSList *ranges = NULL;
	const char *pdp_type;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGDCONT:")) {
		if (!g_at_result_iter_open_list(&iter))
			goto next;

		while (g_at_result_iter_next_range(&iter, &range[0],
							&range[1]))
			ranges = g_slist_prepend(ranges,
					g_memdup(range, sizeof(range)));

		if (!g_at_result_iter_close_list(&iter))
			goto next;

		if (!ranges || range[1] < range[0])
			goto next;

		if (!g_at_result_iter_next_string(&iter, &pdp_type))
			goto next;

		/* We look for IP PDPs */
		if (!strcmp(pdp_type, "IP"))
			break;

next:
		if (ranges) {
			g_slist_foreach(ranges, (GFunc) g_free, NULL);
			g_slist_free(ranges);
			ranges = NULL;
		}
	}
	if (!ranges)
		goto error;

	gd->primary_id_range = g_slist_reverse(ranges);

	ofono_debug("gprs_init: registering to notifications");

	g_at_chat_register(gd->chat, "+CGEV:", cgev_notify, FALSE, gprs, NULL);
	g_at_chat_register(gd->chat, "+CGREG:", cgreg_notify, FALSE, gprs, NULL);

	ofono_gprs_register(gprs);

	return;

error:
	ofono_gprs_remove(gprs);
}

static int at_gprs_probe(struct ofono_gprs *gprs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_data *gd;

	gd = g_new0(struct gprs_data, 1);
	gd->chat = chat;

	ofono_gprs_set_data(gprs, gd);

	g_at_chat_send(chat, "AT+CGREG=2", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CGAUTO=0", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CGEREP=2,1", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CGDCONT=?", cgdcont_prefix,
			at_cgdcont_test_cb, gprs, NULL);
	return 0;
}

static void at_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	ofono_gprs_set_data(gprs, NULL);
	g_free(gd);
}

static struct ofono_gprs_driver driver = {
	.name			= "atmodem",
	.probe			= at_gprs_probe,
	.remove			= at_gprs_remove,
	.set_attached		= at_gprs_set_attached,
};

void at_gprs_init()
{
	ofono_gprs_driver_register(&driver);
}

void at_gprs_exit()
{
	ofono_gprs_driver_unregister(&driver);
}
