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
#include "gattty.h"

#include "huaweimodem.h"

static const char *none_prefix[] = { NULL };
static const char *dhcp_prefix[] = { "^DHCP:", NULL };

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	unsigned int dhcp_source;
	unsigned int dhcp_count;
	guint ndis_watch;
	ofono_gprs_context_cb_t cb;
	void *cb_data;					/* Callback data */
};

static void check_dhcp(struct ofono_gprs_context *gc);

static gboolean dhcp_poll(gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	if (gcd->dhcp_count > 20)
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
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
	struct ofono_modem *modem;
	const char *interface;
	char *ip = NULL;
	char *netmask = NULL;
	char *gateway = NULL;
	char *dns1 = NULL;
	char *dns2 = NULL;
	const char *dns[3];

	DBG("ok %d", ok);

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
	ofono_info("IP: %s  Gateway: %s", ip, gateway);
	ofono_info("DNS: %s, %s", dns1, dns2);

	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");

	ofono_gprs_context_set_interface(gc, interface);
	ofono_gprs_context_set_ipv4_address(gc, ip, TRUE);
	ofono_gprs_context_set_ipv4_netmask(gc, netmask);
	ofono_gprs_context_set_ipv4_gateway(gc, gateway);
	ofono_gprs_context_set_ipv4_dns_servers(gc, dns);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
	gcd->cb = NULL;
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

	DBG("ok %d", ok);

	if (ok) {
		gcd->cb = cb;
		gcd->cb_data = cbd->data;

		if (gcd->ndis_watch > 0) {
			g_source_remove(gcd->ndis_watch);
			gcd->ndis_watch = 0;
		}
	}

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void at_ndisdup_up_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("ok %d", ok);

	if (ok) {
		gcd->cb = cb;
		gcd->cb_data = cbd->data;

		gcd->dhcp_count = 0;

		check_dhcp(gc);
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

	snprintf(buf, sizeof(buf), "AT^NDISDUP=%u,1", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_ndisdup_up_cb, ncbd, g_free) > 0)
		return;

	g_free(ncbd);

	gcd->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void huawei_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
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
				at_cgdcont_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void huawei_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[128];

	DBG("cid %u", cid);

	cbd->user = gc;

	snprintf(buf, sizeof(buf), "AT^NDISDUP=%u,0", cid);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_ndisdup_down_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static int huawei_gprs_context_probe(struct ofono_gprs_context *gc,
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

	return 0;
}

static void huawei_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

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

void huawei_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void huawei_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
