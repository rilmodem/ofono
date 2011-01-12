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
#include <sys/stat.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"
#include "gatppp.h"

#include "atmodem.h"

#define TUN_SYSFS_DIR "/sys/devices/virtual/misc/tun"

#define STATIC_IP_NETMASK "255.255.255.255"

static const char *none_prefix[] = { NULL };

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	GAtPPP *ppp;
	enum state state;
	union {
		ofono_gprs_context_cb_t down_cb;        /* Down callback */
		ofono_gprs_context_up_cb_t up_cb;       /* Up callback */
	};
	void *cb_data;                                  /* Callback data */
};

static void ppp_debug(const char *str, void *data)
{
	ofono_info("%s: %s", (const char *) data, str);
}

static void ppp_connect(const char *interface, const char *local,
			const char *remote,
			const char *dns1, const char *dns2,
			gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	const char *dns[3];

	DBG("");

	dns[0] = dns1;
	dns[1] = dns2;
	dns[2] = 0;

	ofono_info("IP: %s", local);
	ofono_info("DNS: %s, %s", dns1, dns2);

	gcd->state = STATE_ACTIVE;
	CALLBACK_WITH_SUCCESS(gcd->up_cb, interface, TRUE, local,
					STATIC_IP_NETMASK, NULL,
					dns, gcd->cb_data);
}

static void ppp_disconnect(GAtPPPDisconnectReason reason, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	g_at_ppp_unref(gcd->ppp);
	gcd->ppp = NULL;

	switch (gcd->state) {
	case STATE_ENABLING:
		CALLBACK_WITH_FAILURE(gcd->up_cb, NULL, FALSE, NULL,
					NULL, NULL, NULL, gcd->cb_data);
		break;
	case STATE_DISABLING:
		CALLBACK_WITH_SUCCESS(gcd->down_cb, gcd->cb_data);
		break;
	default:
		ofono_gprs_context_deactivated(gc, gcd->active_context);
		break;
	}

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;
	/*
	 * If the channel of gcd->chat is NULL, it might cause
	 * gprs_context_remove get called and the gprs context will be
	 * removed.
	 */
	g_at_chat_resume(gcd->chat);
}

static gboolean setup_ppp(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtIO *io;

	DBG("");

	io = g_at_chat_get_io(gcd->chat);

	g_at_chat_suspend(gcd->chat);

	/* open ppp */
	gcd->ppp = g_at_ppp_new_from_io(io);

	if (gcd->ppp == NULL) {
		g_at_chat_resume(gcd->chat);
		return FALSE;
	}

	if (getenv("OFONO_PPP_DEBUG"))
		g_at_ppp_set_debug(gcd->ppp, ppp_debug, "PPP");

	g_at_ppp_set_credentials(gcd->ppp, gcd->username, gcd->password);

	/* set connect and disconnect callbacks */
	g_at_ppp_set_connect_function(gcd->ppp, ppp_connect, gc);
	g_at_ppp_set_disconnect_function(gcd->ppp, ppp_disconnect, gc);

	/* open the ppp connection */
	g_at_ppp_open(gcd->ppp);

	return TRUE;
}

static void at_cgdata_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		ofono_info("Unable to enter data state");

		gcd->active_context = 0;
		gcd->state = STATE_IDLE;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->up_cb(&error, NULL, 0, NULL, NULL, NULL, NULL,
				gcd->cb_data);
		return;
	}

	setup_ppp(gc);
}

static void at_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;
		gcd->state = STATE_IDLE;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->up_cb(&error, NULL, 0, NULL, NULL, NULL, NULL,
				gcd->cb_data);
		return;
	}

	sprintf(buf, "AT+CGDATA=\"PPP\",%u", gcd->active_context);
	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgdata_cb, gc, NULL) > 0)
		return;

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;

	CALLBACK_WITH_FAILURE(gcd->up_cb, NULL, 0, NULL, NULL, NULL, NULL,
				gcd->cb_data);
}

static void at_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_up_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;
	gcd->up_cb = cb;
	gcd->cb_data = data;
	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));

	gcd->state = STATE_ENABLING;

	len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"", ctx->cid);

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
				ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgdcont_cb, gc, NULL) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, data);
}

static void at_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("cid %u", cid);

	gcd->state = STATE_DISABLING;
	gcd->down_cb = cb;
	gcd->cb_data = data;

	g_at_ppp_shutdown(gcd->ppp);
}

static int at_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;
	struct stat st;

	DBG("");

	if (stat(TUN_SYSFS_DIR, &st) < 0) {
		ofono_error("Missing support for TUN/TAP devices");
		return -ENODEV;
	}

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->chat = g_at_chat_clone(chat);

	ofono_gprs_context_set_data(gc, gcd);

	return 0;
}

static void at_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	if (gcd->state != STATE_IDLE && gcd->ppp) {
		g_at_ppp_unref(gcd->ppp);
		g_at_chat_resume(gcd->chat);
	}

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "atmodem",
	.probe			= at_gprs_context_probe,
	.remove			= at_gprs_context_remove,
	.activate_primary	= at_gprs_activate_primary,
	.deactivate_primary	= at_gprs_deactivate_primary,
};

void at_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void at_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
