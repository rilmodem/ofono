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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/phonebook.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };

struct gobi_data {
	GAtChat *chat;
	struct ofono_sim *sim;
	gboolean have_sim;
};

static void gobi_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int gobi_probe(struct ofono_modem *modem)
{
	struct gobi_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct gobi_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void gobi_remove(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_free(data);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	const char *device;
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	DBG("%s %s", key, device);

	channel = g_at_tty_open(device, NULL);
	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, gobi_debug, debug);

	return chat;
}

static void simstat_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);

	GAtResultIter iter;
	const char *state, *tmp;

	if (data->sim == NULL)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "$QCSIMSTAT:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &tmp))
		return;

	/*
	 * When receiving an unsolicited notification, the comma
	 * is missing ($QCSIMSTAT: 1 SIM INIT COMPLETED). Handle
	 * this gracefully.
	 */
	if (g_str_has_prefix(tmp, "1 ") == TRUE)
		state = tmp + 2;
	else if (!g_at_result_iter_next_unquoted_string(&iter, &state))
		return;

	DBG("state %s", state);

	if (g_str_equal(state, "SIM INIT COMPLETED") == TRUE) {
		if (data->have_sim == FALSE) {
			ofono_sim_inserted_notify(data->sim, TRUE);
			data->have_sim = TRUE;
		}
	}
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("");

	data->have_sim = FALSE;

	ofono_modem_set_powered(modem, ok);

	g_at_chat_register(data->chat, "$QCSIMSTAT:", simstat_notify,
						FALSE, modem, NULL);

	g_at_chat_send(data->chat, "AT$QCSIMSTAT=1", none_prefix,
						NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT$QCSIMSTAT?", none_prefix,
						NULL, NULL, NULL);
}

static int gobi_enable(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->chat = open_device(modem, "Device", "Device: ");
	if (data->chat == NULL)
		return -EINVAL;

	g_at_chat_send(data->chat, "ATE0 +CMEE=1", none_prefix,
						NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=4", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int gobi_disable(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->chat == NULL)
		return 0;

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);

	g_at_chat_send(data->chat, "AT+CFUN=0", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	if (ok)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void gobi_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct gobi_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (cbd == NULL || data->chat == NULL)
		goto error;

	if (g_at_chat_send(data->chat, command, NULL,
					set_online_cb, cbd, g_free))
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void gobi_pre_sim(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	data->sim = ofono_sim_create(modem, 0, "atmodem", data->chat);
}

static void gobi_post_sim(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->chat);

	ofono_sms_create(modem, OFONO_VENDOR_GOBI, "atmodem", data->chat);
}

static void gobi_post_online(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_GOBI, "atmodem", data->chat);

	ofono_cbs_create(modem, OFONO_VENDOR_GOBI, "atmodem", data->chat);
	ofono_ussd_create(modem, OFONO_VENDOR_GOBI, "atmodem", data->chat);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_GOBI,
						"atmodem", data->chat);
}

static struct ofono_modem_driver gobi_driver = {
	.name		= "gobi",
	.probe		= gobi_probe,
	.remove		= gobi_remove,
	.enable		= gobi_enable,
	.disable	= gobi_disable,
	.set_online     = gobi_set_online,
	.pre_sim	= gobi_pre_sim,
	.post_sim	= gobi_post_sim,
	.post_online    = gobi_post_online,
};

static int gobi_init(void)
{
	return ofono_modem_driver_register(&gobi_driver);
}

static void gobi_exit(void)
{
	ofono_modem_driver_unregister(&gobi_driver);
}

OFONO_PLUGIN_DEFINE(gobi, "Qualcomm Gobi modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, gobi_init, gobi_exit)
