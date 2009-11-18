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

#include "hsomodem.h"

#define HSO_DISCONNECTED 0
#define HSO_CONNECTED 1
#define HSO_FAILED 2

static const char *none_prefix[] = { NULL };
static const char *owandata_prefix[] = { "_OWANDATA:", NULL };

struct gprs_context_data {
	GAtChat *chat;
	unsigned active_context;
};

static void at_owancall_down_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	if (ok)
		gcd->active_context = 0;

	dump_response("owancall_down_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void hso_owancall_up_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_up_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	const char *interface = NULL;
	struct ofono_error error;

	dump_response("owancall_up_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (ok) {
		struct ofono_modem *modem = ofono_gprs_context_get_modem(gc);
		interface = ofono_modem_get_string(modem, "NetworkInterface");
	} else {
		struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
		gcd->active_context = 0;
	}

	cb(&error, interface, FALSE, NULL, NULL, NULL, NULL, cbd->data);
}

static void hso_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_up_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *ncbd;
	char buf[64];

	dump_response("cgdcont_cb", ok, result);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, NULL, 0, NULL, NULL, NULL, NULL, cbd->data);
		return;
	}

	ncbd = g_memdup(cbd, sizeof(struct cb_data));

	sprintf(buf, "AT_OWANCALL=%u,1,1", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				hso_owancall_up_cb, ncbd, g_free) > 0)
		return;

	if (ncbd)
		g_free(ncbd);

	gcd->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, cbd->data);
}

static void hso_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_up_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len;

	if (!cbd)
		goto error;

	gcd->active_context = ctx->cid;

	cbd->user = gc;

	if (ctx->username[0] && ctx->password[0])
		sprintf(buf, "AT$QCPDPP=%u,1,\"%s\",\"%s\"",
			ctx->cid, ctx->username, ctx->password);
	else if (ctx->password[0])
		sprintf(buf, "AT$QCPDPP=%u,2,,\"%s\"",
			ctx->cid, ctx->password);
	else
		sprintf(buf, "AT$QCPDPP=%u,0", ctx->cid);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				NULL, NULL, NULL) == 0)
		goto error;

	len = sprintf(buf, "AT+CGDCONT=%u,\"IP\"", ctx->cid);

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
				ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				hso_cgdcont_cb, cbd, g_free) > 0)
		return;
error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, data);
}

static void hso_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[128];

	if (!cbd)
		goto error;

	cbd->user = gc;

	sprintf(buf, "AT_OWANCALL=%u,0,1", cid);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_owancall_down_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void owandata_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	unsigned int cid;
	const char *ip = NULL;
	const char *gateway = NULL;
	const char *dns1 = NULL;
	const char *dns2 = NULL;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "_OWANDATA:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &cid);
	g_at_result_iter_next_unquoted_string(&iter, &ip);
	g_at_result_iter_next_unquoted_string(&iter, &gateway);
	g_at_result_iter_next_unquoted_string(&iter, &dns1);
	g_at_result_iter_next_unquoted_string(&iter, &dns2);

	if (ip && ip[0] == ' ')
		ip += 1;

	if (gateway && gateway[0] == ' ')
		gateway += 1;

	if (dns1 && dns1[0] == ' ')
		dns1 += 1;

	if (dns2 && dns2[0] == ' ')
		dns2 += 1;

	/* Don't bother reporting the same DNS twice */
	if (g_str_equal(dns1, dns2))
		dns2 = NULL;

	ofono_info("Got the following parameters for context: %d", cid);
	ofono_info("IP: %s, Gateway: %s", ip, gateway);
	ofono_info("DNS: %s, %s", dns1, dns2);
}

static void owancall_notifier(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	int state;
	unsigned int cid;

	if (gcd->active_context == 0)
		return;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "_OWANCALL:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &cid);
	g_at_result_iter_next_number(&iter, &state);

	if (gcd->active_context != cid)
		return;

	switch (state) {
	case HSO_DISCONNECTED:
		ofono_gprs_context_deactivated(gc, gcd->active_context);
		gcd->active_context = 0;
		break;
	case HSO_CONNECTED:
	{
		char buf[128];

		ofono_debug("HSO Context: connected");

		sprintf(buf, "AT_OWANDATA=%u", gcd->active_context);

		g_at_chat_send(gcd->chat, buf, owandata_prefix,
				owandata_cb, gc, NULL);

		break;
	}
	case HSO_FAILED:
		ofono_debug("HSO Context: failed");
		break;
	default:
		break;
	};
}

static int hso_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;

	gcd = g_new0(struct gprs_context_data, 1);
	gcd->chat = chat;

	g_at_chat_register(chat, "_OWANCALL:", owancall_notifier,
				FALSE, gc, NULL);

	ofono_gprs_context_set_data(gc, gcd);

	return 0;
}

static void hso_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	ofono_gprs_context_set_data(gc, NULL);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "hso",
	.probe			= hso_gprs_context_probe,
	.remove			= hso_gprs_context_remove,
	.activate_primary	= hso_gprs_activate_primary,
	.deactivate_primary	= hso_gprs_deactivate_primary,
};

void hso_gprs_context_init()
{
	ofono_gprs_context_driver_register(&driver);
}

void hso_gprs_context_exit()
{
	ofono_gprs_context_driver_unregister(&driver);
}
