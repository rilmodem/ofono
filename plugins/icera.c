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

#include <errno.h>
#include <stdlib.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *siminit_prefix[] = { "%ISIMINIT:", NULL };
static const char *ussdmode_prefix[] = { "%IUSSDMODE:", NULL };

struct icera_data {
	GAtChat *chat;
	struct ofono_sim *sim;
	gboolean have_sim;
	gboolean have_ussdmode;
};

static int icera_probe(struct ofono_modem *modem)
{
	struct icera_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct icera_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void icera_remove(struct ofono_modem *modem)
{
	struct icera_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->chat);

	g_free(data);
}

static void icera_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static GAtChat *open_device(struct ofono_modem *modem,
					const char *key, char *debug)
{
	GAtChat *chat;
	GAtSyntax *syntax;
	GIOChannel *channel;
	GHashTable *options;
	const char *device;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		return NULL;

	g_hash_table_insert(options, "Baud", "115200");

	channel = g_at_tty_open(device, options);

	g_hash_table_destroy(options);

	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, icera_debug, debug);

	return chat;
}

static void ussdmode_query(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct icera_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int mode;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%IUSSDMODE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	DBG("mode %d", mode);

	if (mode == 1)
		data->have_ussdmode = TRUE;
}

static void ussdmode_support(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct icera_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%IUSSDMODE:"))
		return;

	g_at_chat_send(data->chat, "AT%IUSSDMODE?", ussdmode_prefix,
					ussdmode_query, modem, NULL);
}

static void icera_set_sim_state(struct icera_data *data, int state)
{
	DBG("state %d", state);

	switch (state) {
	case 1:
		if (data->have_sim == FALSE) {
			ofono_sim_inserted_notify(data->sim, TRUE);
			data->have_sim = TRUE;
		}
		break;
	case 0:
	case 2:
		if (data->have_sim == TRUE) {
			ofono_sim_inserted_notify(data->sim, FALSE);
			data->have_sim = FALSE;
		}
		break;
	default:
		ofono_warn("Unknown SIM state %d received", state);
		break;
	}
}

static void siminit_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct icera_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int state;

	if (data->sim == NULL)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%ISIMINIT:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	icera_set_sim_state(data, state);
}

static void siminit_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct icera_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int state;

	DBG("");

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%ISIMINIT:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	icera_set_sim_state(data, state);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct icera_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		g_at_chat_unref(data->chat);
		data->chat = NULL;

		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	/* switch to GSM character set instead of IRA */
	g_at_chat_send(data->chat, "AT+CSCS=\"GSM\"", none_prefix,
						NULL, NULL, NULL);

        data->have_sim = FALSE;

	/* notify that the modem is ready so that pre_sim gets called */
	ofono_modem_set_powered(modem, TRUE);

	/* register for SIM init notifications */
	g_at_chat_register(data->chat, "%ISIMINIT:", siminit_notify,
						FALSE, modem, NULL);

	g_at_chat_send(data->chat, "AT%ISIMINIT=1", none_prefix,
						NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT%ISIMINIT", siminit_prefix,
					siminit_query, modem, NULL);

	g_at_chat_send(data->chat, "AT%IAIRCRAFT?", none_prefix,
						NULL, NULL, NULL);
}

static int icera_enable(struct ofono_modem *modem)
{
	struct icera_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->chat = open_device(modem, "Aux", "Aux: ");
	if (data->chat == NULL)
		return -EIO;

	g_at_chat_send(data->chat, "ATE0 +CMEE=1", NULL, NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT%IFWR", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT%ISWIN", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT%IUSSDMODE=?", ussdmode_prefix,
					ussdmode_support, modem, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=4", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct icera_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int icera_disable(struct ofono_modem *modem)
{
	struct icera_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

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
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void icera_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct icera_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("%p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(data->chat, command, none_prefix,
					set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void icera_pre_sim(struct ofono_modem *modem)
{
	struct icera_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	data->sim = ofono_sim_create(modem, OFONO_VENDOR_ICERA,
					"atmodem", data->chat);
}

static void icera_post_sim(struct ofono_modem *modem)
{
	struct icera_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_radio_settings_create(modem, 0, "iceramodem", data->chat);

	ofono_sms_create(modem, OFONO_VENDOR_ICERA, "atmodem", data->chat);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_ICERA,
						"atmodem", data->chat);
	gc = ofono_gprs_context_create(modem, 0, "iceramodem", data->chat);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static void icera_post_online(struct ofono_modem *modem)
{
	struct icera_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_ICERA, "atmodem", data->chat);

	if (data->have_ussdmode == TRUE)
		ofono_ussd_create(modem, 0, "huaweimodem", data->chat);
	else
		ofono_ussd_create(modem, 0, "atmodem", data->chat);
}

static struct ofono_modem_driver icera_driver = {
	.name		= "icera",
	.probe		= icera_probe,
	.remove		= icera_remove,
	.enable		= icera_enable,
	.disable	= icera_disable,
	.set_online	= icera_set_online,
	.pre_sim	= icera_pre_sim,
	.post_sim	= icera_post_sim,
	.post_online	= icera_post_online,
};

static int icera_init(void)
{
	return ofono_modem_driver_register(&icera_driver);
}

static void icera_exit(void)
{
	ofono_modem_driver_unregister(&icera_driver);
}

OFONO_PLUGIN_DEFINE(icera, "Icera modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, icera_init, icera_exit)
