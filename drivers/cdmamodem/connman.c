/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011  Nokia Corporation and/or its subsidiary(-ies).
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
#include <ofono/cdma-connman.h>

#include "gatchat.h"
#include "gatresult.h"
#include "gatppp.h"

#include "cdmamodem.h"
#include "drivers/atmodem/vendor.h"

#define TUN_SYSFS_DIR "/sys/devices/virtual/misc/tun"

#define STATIC_IP_NETMASK "255.255.255.255"

static const char *none_prefix[] = { NULL };

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct connman_data {
	GAtChat *chat;
	GAtPPP *ppp;
	unsigned int vendor;
	enum state state;
	char username[OFONO_CDMA_CONNMAN_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_CDMA_CONNMAN_MAX_PASSWORD_LENGTH + 1];
	union {
		ofono_cdma_connman_cb_t down_cb;	/* Down callback */
		ofono_cdma_connman_up_cb_t up_cb;	/* Up callback */
	};
	void *cb_data;					/* Callback data */
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
	struct ofono_cdma_connman *cm = user_data;
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);
	const char *dns[3];

	DBG("");

	dns[0] = dns1;
	dns[1] = dns2;
	dns[2] = 0;

	ofono_info("IP: %s", local);
	ofono_info("DNS: %s, %s", dns1, dns2);

	cd->state = STATE_ACTIVE;
	CALLBACK_WITH_SUCCESS(cd->up_cb, interface, TRUE, local,
					STATIC_IP_NETMASK, NULL,
					dns, cd->cb_data);
}

static void ppp_disconnect(GAtPPPDisconnectReason reason, gpointer user_data)
{
	struct ofono_cdma_connman *cm = user_data;
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);

	DBG("");

	g_at_ppp_unref(cd->ppp);
	cd->ppp = NULL;

	switch (cd->state) {
	case STATE_ENABLING:
		CALLBACK_WITH_FAILURE(cd->up_cb, NULL, FALSE, NULL,
					NULL, NULL, NULL, cd->cb_data);
		break;
	case STATE_DISABLING:
		CALLBACK_WITH_SUCCESS(cd->down_cb, cd->cb_data);
		break;
	default:
		ofono_cdma_connman_deactivated(cm);
		break;
	}

	cd->state = STATE_IDLE;
	g_at_chat_resume(cd->chat);
}

static gboolean setup_ppp(struct ofono_cdma_connman *cm)
{
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);
	GAtIO *io;

	DBG("");

	io = g_at_chat_get_io(cd->chat);

	g_at_chat_suspend(cd->chat);

	/* open ppp */
	cd->ppp = g_at_ppp_new();

	if (cd->ppp == NULL) {
		g_at_chat_resume(cd->chat);
		return FALSE;
	}

	if (getenv("OFONO_PPP_DEBUG"))
		g_at_ppp_set_debug(cd->ppp, ppp_debug, "PPP");

	/* set connect and disconnect callbacks */
	g_at_ppp_set_connect_function(cd->ppp, ppp_connect, cm);
	g_at_ppp_set_disconnect_function(cd->ppp, ppp_disconnect, cm);

	g_at_ppp_set_credentials(cd->ppp, cd->username, cd->password);

	/* open the ppp connection */
	g_at_ppp_open(cd->ppp, io);

	return TRUE;
}

static void atd_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_connman *cm = user_data;
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);

	DBG("ok %d", ok);

	if (ok == FALSE) {
		struct ofono_error error;

		ofono_info("Unable to enter data state");

		cd->state = STATE_IDLE;

		decode_at_error(&error, g_at_result_final_response(result));
		cd->up_cb(&error, NULL, 0, NULL, NULL, NULL, NULL,
				cd->cb_data);
		return;
	}

	setup_ppp(cm);
}

static void cdma_connman_activate(struct ofono_cdma_connman *cm,
					const char *username,
					const char *password,
					ofono_cdma_connman_up_cb_t cb,
					void *data)
{
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);

	DBG("");

	cd->up_cb = cb;
	cd->cb_data = data;
	strcpy(cd->username, username);
	strcpy(cd->password, password);

	cd->state = STATE_ENABLING;

	if (g_at_chat_send(cd->chat, "ATD#777", none_prefix,
				atd_cb, cm, NULL) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, data);
}

static void cdma_connman_deactivate(struct ofono_cdma_connman *cm,
					ofono_cdma_connman_cb_t cb,
					void *data)
{
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);

	DBG("");

	cd->state = STATE_DISABLING;
	cd->down_cb = cb;
	cd->cb_data = data;

	g_at_ppp_shutdown(cd->ppp);
}

static void huawei_dsdormant_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_connman *cm = user_data;
	int dormant;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^DSDORMANT:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &dormant))
		return;

	switch (dormant) {
	case 0:
		ofono_cdma_connman_dormant_notify(cm, FALSE);
		break;
	case 1:
		ofono_cdma_connman_dormant_notify(cm, TRUE);
		break;
	default:
		ofono_error("Invalid DSDORMANT value");
		break;
	}
}

static void at_c0_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_connman *cm = user_data;
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);
	GAtChat *chat;

	DBG("ok %d", ok);

	if (ok == FALSE) {
		ofono_info("Unable to configure circuit 109");
		ofono_cdma_connman_remove(cm);
		return;
	}

	switch (cd->vendor) {
	case OFONO_VENDOR_HUAWEI:
		chat = g_at_chat_get_slave(cd->chat);
		g_at_chat_register(chat, "^DSDORMANT", huawei_dsdormant_notify,
					FALSE, cm, NULL);
		break;
	default:
		break;
	}

	ofono_cdma_connman_register(cm);
}

static int cdma_connman_probe(struct ofono_cdma_connman *cm,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct connman_data *cd;
	struct stat st;

	DBG("");

	if (stat(TUN_SYSFS_DIR, &st) < 0) {
		ofono_error("Missing support for TUN/TAP devices");
		return -ENODEV;
	}

	cd = g_try_new0(struct connman_data, 1);
	if (cd == NULL)
		return -ENOMEM;

	cd->chat = g_at_chat_clone(chat);
	cd->vendor = vendor;

	ofono_cdma_connman_set_data(cm, cd);

	/* Turn off any modem-initiated dormancy timeout */
	g_at_chat_send(cd->chat, "AT+CTA=0", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(cd->chat, "AT&C0", none_prefix, at_c0_cb, cm, NULL);

	return 0;
}

static void cdma_connman_remove(struct ofono_cdma_connman *cm)
{
	struct connman_data *cd = ofono_cdma_connman_get_data(cm);

	DBG("");

	if (cd->state != STATE_IDLE && cd->ppp) {
		g_at_ppp_unref(cd->ppp);
		g_at_chat_resume(cd->chat);
	}

	ofono_cdma_connman_set_data(cm, NULL);

	g_at_chat_unref(cd->chat);
	g_free(cd);
}

static struct ofono_cdma_connman_driver driver = {
	.name			= "cdmamodem",
	.probe			= cdma_connman_probe,
	.remove			= cdma_connman_remove,
	.activate		= cdma_connman_activate,
	.deactivate		= cdma_connman_deactivate,
};

void cdma_connman_init(void)
{
	ofono_cdma_connman_driver_register(&driver);
}

void cdma_connman_exit(void)
{
	ofono_cdma_connman_driver_unregister(&driver);
}
