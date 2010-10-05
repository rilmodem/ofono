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
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"

#include "huaweimodem.h"

#define AUTH_BUF_LENGTH OFONO_GPRS_MAX_USERNAME_LENGTH + \
			OFONO_GPRS_MAX_PASSWORD_LENGTH + 128

static const char *none_prefix[] = { NULL };
static const char *dhcp_prefix[] = { "^DHCP:", NULL };

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	unsigned int dhcp_source;
	unsigned int dhcp_count;
	union {
		ofono_gprs_context_cb_t down_cb;	/* Down callback */
		ofono_gprs_context_up_cb_t up_cb;	/* Up callback */
	};
	void *cb_data;					/* Callback data */
};

static void check_dhcp(struct ofono_gprs_context *gc);

static gboolean dhcp_poll(gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	if (gcd->dhcp_count > 10)
		CALLBACK_WITH_FAILURE(gcd->up_cb, NULL, 0, NULL, NULL,
						NULL, NULL, gcd->cb_data);
	else
		check_dhcp(gc);

	gcd->dhcp_count++;
	gcd->dhcp_source = 0;

	return FALSE;
}

static gboolean get_next_addr(GAtResultIter *iter, char **addr)
{
	const char *str;
	guint32 val;

	if (g_at_result_iter_next_unquoted_string(iter, &str) == FALSE)
		return FALSE;

	val = strtol(str, NULL, 16);

	if (addr)
	        *addr = g_strdup_printf("%u.%u.%u.%u",
					(val & 0x000000ff),
					(val & 0x0000ff00) >> 8,
					(val & 0x00ff0000) >> 16,
					(val & 0xff000000) >> 24);

	return TRUE;
}

static void dhcp_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	const char *interface;
	char *ip = NULL;
	char *netmask = NULL;
	char *gateway = NULL;
	char *dns1 = NULL;
	char *dns2 = NULL;
	const char *dns[3];
	struct ofono_modem *modem;
	const char *devnode;

	DBG("");

	if (!ok) {
		gcd->dhcp_source = g_timeout_add_seconds(1, dhcp_poll, gc);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "^DHCP:") == FALSE)
		return;

	get_next_addr(&iter, &ip);
	get_next_addr(&iter, &netmask);
	get_next_addr(&iter, &gateway);
	get_next_addr(&iter, NULL);
	get_next_addr(&iter, &dns1);
	get_next_addr(&iter, &dns2);

	dns[0] = dns1;
	dns[1] = dns2;
	dns[2] = 0;

	ofono_info("Got the following parameters for context: %d",
							gcd->active_context);
	ofono_info("IP: %s, Gateway: %s", ip, gateway);
	ofono_info("DNS: %s, %s", dns1, dns2);

	modem = ofono_gprs_context_get_modem(gc);
	devnode = ofono_modem_get_string(modem, "NDIS");

	ofono_info("NDIS: %s", devnode);

	interface = "invalid";

	CALLBACK_WITH_SUCCESS(gcd->up_cb, interface, TRUE, ip,
					netmask, gateway, dns, gcd->cb_data);
	gcd->up_cb = NULL;
	gcd->cb_data = NULL;

	g_free(ip);
	g_free(netmask);
	g_free(gateway);
	g_free(dns1);
	g_free(dns2);
}

static void check_dhcp(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	g_at_chat_send(gcd->chat, "AT^DHCP?", dhcp_prefix,
					dhcp_query_cb, gc, NULL);
}

static void at_ndisdup_down_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("");

	if (ok) {
		gcd->down_cb = cb;
		gcd->cb_data = cbd->data;
	}

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void at_ndisdup_up_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_up_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("");

	if (ok) {
		gcd->up_cb = cb;
		gcd->cb_data = cbd->data;

		gcd->dhcp_count = 0;

		check_dhcp(gc);
		return;
	}

	gcd->active_context = 0;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, NULL, FALSE, NULL, NULL, NULL, NULL, cbd->data);
}

static void at_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_up_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *ncbd;
	char buf[64];

	DBG("");

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, NULL, 0, NULL, NULL, NULL, NULL, cbd->data);
		return;
	}

	ncbd = g_memdup(cbd, sizeof(struct cb_data));

	snprintf(buf, sizeof(buf), "AT^NDISDUP=%u,1", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_ndisdup_up_cb, ncbd, g_free) > 0)
		return;

	g_free(ncbd);

	gcd->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, cbd->data);
}

static void huawei_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_up_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[AUTH_BUF_LENGTH];
	int len;

	DBG("");

	if (!cbd)
		goto error;

	gcd->active_context = ctx->cid;

	cbd->user = gc;

	len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"", ctx->cid);

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
				ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgdcont_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, data);
}

static void huawei_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[128];

	DBG("cid %u", cid);

	if (!cbd)
		goto error;

	cbd->user = gc;

	snprintf(buf, sizeof(buf), "AT^NDISDUP=%u,0", cid);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_ndisdup_down_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static int huawei_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (!gcd)
		return -ENOMEM;

	gcd->chat = g_at_chat_clone(chat);

	ofono_gprs_context_set_data(gc, gcd);

	return 0;
}

static void huawei_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "huaweimodem",
	.probe			= huawei_gprs_context_probe,
	.remove			= huawei_gprs_context_remove,
	.activate_primary	= huawei_gprs_activate_primary,
	.deactivate_primary	= huawei_gprs_deactivate_primary,
};

void huawei_gprs_context_init()
{
	ofono_gprs_context_driver_register(&driver);
}

void huawei_gprs_context_exit()
{
	ofono_gprs_context_driver_unregister(&driver);
}
