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
#include <sys/stat.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"
#include "gattty.h"

#include "iceramodem.h"

#define ICERA_DISCONNECTED	0
#define ICERA_CONNECTED		1
#define ICERA_CONNECTING	2
#define ICERA_FAILED		3

#define AUTH_BUF_LENGTH OFONO_GPRS_MAX_USERNAME_LENGTH + \
			OFONO_GPRS_MAX_PASSWORD_LENGTH + 128

#define STATIC_IP_NETMASK "255.255.255.255"

static const char *none_prefix[] = { NULL };
static const char *ipdpact_prefix[] = { "%IPDPACT", NULL };
static const char *ipdpaddr_prefix[] = { "%IPDPADDR", NULL };

enum state {
	STATE_NONE,
	STATE_ENABLING,
	STATE_DISABLING,
};

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	enum state state;
	ofono_gprs_context_cb_t cb;
	void *cb_data;
};

static void ipdpaddr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	int cid;
	const char *ip = NULL;
	const char *gateway = NULL;
	const char *dns1 = NULL;
	const char *dns2 = NULL;
	const char *dns[3];
	struct ofono_modem *modem;
	const char *interface;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "%IPDPADDR:") == FALSE)
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

	dns[0] = dns1;
	dns[1] = dns2;
	dns[2] = 0;

	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");

	ofono_info("Got the following parameters for context: %d", cid);
	ofono_info("IP: %s, Gateway: %s", ip, gateway);
	ofono_info("DNS: %s, %s", dns1, dns2);

	ofono_gprs_context_set_interface(gc, interface);
	ofono_gprs_context_set_ipv4_address(gc, ip, TRUE);
	ofono_gprs_context_set_ipv4_netmask(gc, STATIC_IP_NETMASK);
	ofono_gprs_context_set_ipv4_gateway(gc, gateway);
	ofono_gprs_context_set_ipv4_dns_servers(gc, dns);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);

	gcd->state = STATE_NONE;
	gcd->cb = NULL;
	gcd->cb_data = NULL;
}

static void ipdpact_notifier(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	int cid, state;

	if (gcd->active_context == 0)
		return;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "%IPDPACT:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &cid);
	g_at_result_iter_next_number(&iter, &state);

	DBG("cid %d state %d", cid, state);

	if (gcd->active_context != (unsigned int) cid)
		return;

	switch (state) {
	case ICERA_DISCONNECTED:
		if (gcd->state == STATE_DISABLING) {
			CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
			gcd->state = STATE_NONE;
			gcd->cb = NULL;
			gcd->cb_data = NULL;
		} else
			ofono_gprs_context_deactivated(gc, gcd->active_context);

		gcd->active_context = 0;
		break;

	case ICERA_CONNECTED:
		if (gcd->state == STATE_ENABLING) {
			char buf[128];

			snprintf(buf, sizeof(buf), "AT%%IPDPADDR=%u",
							gcd->active_context);

			g_at_chat_send(gcd->chat, buf, ipdpaddr_prefix,
							ipdpaddr_cb, gc, NULL);
		}
		break;

	case ICERA_CONNECTING:
		break;

	case ICERA_FAILED:
		if (gcd->state == STATE_ENABLING) {
			CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
			gcd->state = STATE_NONE;
			gcd->cb = NULL;
			gcd->cb_data = NULL;
		}

		gcd->active_context = 0;
		break;
	}
}

static void ipdpact_down_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("ok %d", ok);

	if (ok) {
		gcd->state = STATE_DISABLING;
		gcd->cb = cb;
		gcd->cb_data = cbd->data;

		ipdpact_notifier(result, gc);
		return;
	}

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void ipdpact_up_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("ok %d", ok);

	if (ok) {
		gcd->state = STATE_ENABLING;
		gcd->cb = cb;
		gcd->cb_data = cbd->data;

		ipdpact_notifier(result, gc);
		return;
	}

	gcd->active_context = 0;

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
	char buf[AUTH_BUF_LENGTH];

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, cbd->data);
		return;
	}

	if (gcd->username[0] && gcd->password[0])
		sprintf(buf, "AT%%IPDPCFG=%u,0,1,\"%s\",\"%s\"",
			gcd->active_context, gcd->username, gcd->password);
	else
		sprintf(buf, "AT%%IPDPCFG=%u,0,0,\"\",\"\"",
						gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL) == 0)
		goto error;

	ncbd = g_memdup(cbd, sizeof(struct cb_data));

	snprintf(buf, sizeof(buf), "AT%%IPDPACT=%u,1", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, ipdpact_prefix,
				ipdpact_up_cb, ncbd, g_free) > 0)
		return;

	g_free(ncbd);

error:
	gcd->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void icera_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len = 0;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;
	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));

	cbd->user = gc;

	switch (ctx->proto) {
	case OFONO_GPRS_PROTO_IP:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"",
								ctx->cid);
		break;
	case OFONO_GPRS_PROTO_IPV6:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IPV6\"",
								ctx->cid);
		break;
	case OFONO_GPRS_PROTO_IPV4V6:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IPV4V6\"",
								ctx->cid);
		break;
	}

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3,
					",\"%s\"", ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgdcont_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void icera_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[128];

	DBG("cid %u", cid);

	cbd->user = gc;

	snprintf(buf, sizeof(buf), "AT%%IPDPACT=%u,0", cid);

	if (g_at_chat_send(gcd->chat, buf, ipdpact_prefix,
				ipdpact_down_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static int icera_gprs_context_probe(struct ofono_gprs_context *gc,
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

	g_at_chat_register(gcd->chat, "%IPDPACT:", ipdpact_notifier,
							FALSE, gc, NULL);

	return 0;
}

static void icera_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "iceramodem",
	.probe			= icera_gprs_context_probe,
	.remove			= icera_gprs_context_remove,
	.activate_primary	= icera_gprs_activate_primary,
	.deactivate_primary	= icera_gprs_deactivate_primary,
};

void icera_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void icera_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
