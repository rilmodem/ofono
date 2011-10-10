/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"

#include "mbmmodem.h"

#define MBM_E2NAP_DISCONNECTED 0
#define MBM_E2NAP_CONNECTED 1
#define MBM_E2NAP_CONNECTING 2

#define AUTH_BUF_LENGTH OFONO_GPRS_MAX_USERNAME_LENGTH + \
			OFONO_GPRS_MAX_PASSWORD_LENGTH + 128

#define MAX_DNS 5

#define STATIC_IP_NETMASK "255.255.255.248"

static const char *none_prefix[] = { NULL };
static const char *e2ipcfg_prefix[] = { "*E2IPCFG:", NULL };
static const char *enap_prefix[] = { "*ENAP:", NULL };

static gboolean mbm_enap_poll(gpointer user_data);

enum mbm_state {
	MBM_NONE = 0,
	MBM_ENABLING = 1,
	MBM_DISABLING = 2,
};

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	gboolean have_e2nap;
	gboolean have_e2ipcfg;
	unsigned int enap_source;
	enum mbm_state mbm_state;
	ofono_gprs_context_cb_t cb;
	void *cb_data;                                  /* Callback data */
	int enap;                                   /* State of the call */
};

static void mbm_e2ipcfg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	int numdns = 0;
	int type;
	const char *str;
	const char *ip = NULL;
	const char *gateway = NULL;
	const char *dns[MAX_DNS + 1];
	struct ofono_modem *modem;
	const char *interface;
	gboolean success = FALSE;

	DBG("ok %d", ok);

	if (!ok)
		goto out;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*E2IPCFG:") == FALSE)
		return;

	while (g_at_result_iter_open_list(&iter)) {
		if (g_at_result_iter_next_number(&iter, &type) == FALSE)
			break;

		if (g_at_result_iter_next_string(&iter, &str) == FALSE)
			break;

		switch (type) {
		case 1:
			ip = str;
			break;
		case 2:
			gateway = str;
			break;
		case 3:
			if (numdns < MAX_DNS)
				dns[numdns++] = str;
			break;
		default:
			break;
		}

		if (g_at_result_iter_close_list(&iter) == FALSE)
			break;
	}

	dns[numdns] = NULL;

	if (ip && gateway && numdns)
		success = TRUE;

out:
	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");

	ofono_info("IP: %s  Gateway: %s", ip, gateway);
	ofono_info("DNS: %s, %s", dns[0], dns[1]);

	ofono_gprs_context_set_interface(gc, interface);

	if (success) {
		ofono_gprs_context_set_ipv4_address(gc, ip, TRUE);
		ofono_gprs_context_set_ipv4_netmask(gc, STATIC_IP_NETMASK);
		ofono_gprs_context_set_ipv4_dns_servers(gc, dns);
		ofono_gprs_context_set_ipv4_gateway(gc, gateway);
	} else
		ofono_gprs_context_set_ipv4_address(gc, NULL, FALSE);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);

	gcd->mbm_state = MBM_NONE;
	gcd->cb = NULL;
	gcd->cb_data = NULL;
}

static void mbm_get_ip_details(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_modem *modem;
	const char *interface;
	char buf[64];

	DBG("");

	if (gcd->have_e2ipcfg) {
		g_at_chat_send(gcd->chat, "AT*E2IPCFG?", e2ipcfg_prefix,
				mbm_e2ipcfg_cb, gc, NULL);
		return;
	}

	snprintf(buf, sizeof(buf), "AT+CGPADDR=%u", gcd->active_context);
	g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL);

	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");

	ofono_gprs_context_set_interface(gc, interface);
	ofono_gprs_context_set_ipv4_address(gc, NULL, FALSE);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);

	gcd->mbm_state = MBM_NONE;
	gcd->cb = NULL;
	gcd->cb_data = NULL;
}

static void mbm_state_changed(struct ofono_gprs_context *gc, int state)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("state %d", state);

	if (gcd->active_context == 0)
		return;

	switch (state) {
	case MBM_E2NAP_DISCONNECTED:
		DBG("disconnected");

		if (gcd->mbm_state == MBM_DISABLING) {
			CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
			gcd->cb = NULL;
		} else if (gcd->mbm_state == MBM_ENABLING) {
			CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
			gcd->cb = NULL;
		} else {
			ofono_gprs_context_deactivated(gc, gcd->active_context);
		}

		gcd->mbm_state = MBM_NONE;
		gcd->cb_data = NULL;
		gcd->active_context = 0;

		break;

	case MBM_E2NAP_CONNECTED:
		DBG("connected");

		if (gcd->mbm_state == MBM_ENABLING)
			mbm_get_ip_details(gc);

		break;

	case MBM_E2NAP_CONNECTING:
		DBG("connecting");
		break;

	default:
		break;
	};

	gcd->enap = state;
}

static void mbm_enap_poll_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	int state;

	DBG("ok %d", ok);

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*ENAP:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &state);

	mbm_state_changed(gc, state);

	if ((state == MBM_E2NAP_CONNECTED && gcd->mbm_state == MBM_DISABLING) ||
			state == MBM_E2NAP_CONNECTING)
		gcd->enap_source = g_timeout_add_seconds(1, mbm_enap_poll, gc);
}

static gboolean mbm_enap_poll(gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	g_at_chat_send(gcd->chat, "AT*ENAP?", enap_prefix,
				mbm_enap_poll_cb, gc, NULL);

	gcd->enap_source = 0;

	return FALSE;
}

static void at_enap_down_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("ok %d", ok);

	/* Now we have to wait for the unsolicited notification to arrive */
	if (ok && gcd->enap != 0) {
		gcd->mbm_state = MBM_DISABLING;
		gcd->cb = cb;
		gcd->cb_data = cbd->data;

		if (gcd->have_e2nap == FALSE)
			g_at_chat_send(gcd->chat, "AT*ENAP?", enap_prefix,
					mbm_enap_poll_cb, gc, NULL);

		return;
	}

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void mbm_enap_up_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("ok %d", ok);

	if (ok) {
		gcd->mbm_state = MBM_ENABLING;
		gcd->cb = cb;
		gcd->cb_data = cbd->data;

		if (gcd->have_e2nap == FALSE)
			g_at_chat_send(gcd->chat, "AT*ENAP?", enap_prefix,
					mbm_enap_poll_cb, gc, NULL);

		return;
	}

	gcd->active_context = 0;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void mbm_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *ncbd;
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, cbd->data);
		return;
	}

	ncbd = g_memdup(cbd, sizeof(struct cb_data));

	snprintf(buf, sizeof(buf), "AT*ENAP=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				mbm_enap_up_cb, ncbd, g_free) > 0)
		return;

	g_free(ncbd);

	gcd->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void mbm_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[AUTH_BUF_LENGTH];
	int len;

	/* IPv6 support not implemented */
	if (ctx->proto != OFONO_GPRS_PROTO_IP)
		goto error;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;

	cbd->user = gc;

	len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"", ctx->cid);

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
				ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				mbm_cgdcont_cb, cbd, g_free) == 0)
		goto error;

	/*
	 * Set username and password, this should be done after CGDCONT
	 * or an error can occur.  We don't bother with error checking
	 * here
	 * */
	snprintf(buf, sizeof(buf), "AT*EIAAUW=%d,1,\"%s\",\"%s\"",
			ctx->cid, ctx->username, ctx->password);

	g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL);

	return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void mbm_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("cid %u", cid);

	cbd->user = gc;

	if (g_at_chat_send(gcd->chat, "AT*ENAP=0", none_prefix,
				at_enap_down_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void e2nap_notifier(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	GAtResultIter iter;
	int state;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*E2NAP:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &state);

	mbm_state_changed(gc, state);
}

static void mbm_e2nap_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	gcd->have_e2nap = ok;

	if (ok)
		g_at_chat_register(gcd->chat, "*E2NAP:", e2nap_notifier,
					FALSE, gc, NULL);
}

static void mbm_e2ipcfg_query_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	gcd->have_e2ipcfg = ok;
}

static int mbm_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;

	DBG("");

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->chat = g_at_chat_clone(chat);

	ofono_gprs_context_set_data(gc, gcd);

	g_at_chat_send(gcd->chat, "AT*ENAPDBG=1", none_prefix,
				NULL, NULL, NULL);

	g_at_chat_send(gcd->chat, "AT*E2NAP=1", none_prefix,
			mbm_e2nap_cb, gc, NULL);
	g_at_chat_send(gcd->chat, "AT*E2IPCFG=?", e2ipcfg_prefix,
			mbm_e2ipcfg_query_cb, gc, NULL);

	return 0;
}

static void mbm_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	if (gcd->enap_source) {
		g_source_remove(gcd->enap_source);
		gcd->enap_source = 0;
	}

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "mbmmodem",
	.probe			= mbm_gprs_context_probe,
	.remove			= mbm_gprs_context_remove,
	.activate_primary	= mbm_gprs_activate_primary,
	.deactivate_primary	= mbm_gprs_deactivate_primary,
};

void mbm_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void mbm_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
