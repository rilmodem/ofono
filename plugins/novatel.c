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
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *nwdmat_prefix[] = { "$NWDMAT:", NULL };

struct novatel_data {
	GAtChat *primary;
	GAtChat *secondary;
	gint dmat_mode;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
};

static int novatel_probe(struct ofono_modem *modem)
{
	struct novatel_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct novatel_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void novatel_remove(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->primary);
	g_free(data);
}

static void novatel_debug(const char *str, void *user_data)
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
	const char *device;

	device = ofono_modem_get_string(modem, key);
	if (!device)
		return NULL;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!chat)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, novatel_debug, debug);

	return chat;
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, TRUE);
}

static void nwdmat_action(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok)
		goto done;

	data->dmat_mode = 1;

	data->secondary = open_device(modem, "SecondaryDevice", "2nd:");
	if (!data->secondary)
		goto done;

	g_at_chat_send(data->secondary, "ATE0 +CMEE=1", none_prefix,
							NULL, NULL, NULL);

	/* Check for all supported technologies */
	g_at_chat_send(data->secondary, "AT$CNTI=2", none_prefix,
							NULL, NULL, NULL);

done:
	g_at_chat_send(data->primary, "AT+CFUN=4", none_prefix,
						cfun_enable, modem, NULL);
}

static void nwdmat_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct novatel_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	gint dmat_mode;

	DBG("");

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "$NWDMAT:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &dmat_mode))
		goto error;

	if (dmat_mode == 1) {
		nwdmat_action(TRUE, result, user_data);
		return;
	}

	g_at_chat_send(data->primary, "AT$NWDMAT=1", nwdmat_prefix,
						nwdmat_action, modem, NULL);

	return;

error:
	nwdmat_action(FALSE, result, user_data);
}

static void novatel_disconnect(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("");

	ofono_gprs_context_remove(data->gc);

	g_at_chat_unref(data->primary);
	data->primary = NULL;

	data->primary = open_device(modem, "PrimaryDevice", "1st:");
	if (!data->primary)
		return;

	g_at_chat_set_disconnect_function(data->primary,
						novatel_disconnect, modem);

	ofono_info("Reopened GPRS context channel");

	data->gc = ofono_gprs_context_create(modem, 0, "atmodem",
							data->primary);

	if (data->gprs && data->gc)
		ofono_gprs_add_context(data->gprs, data->gc);
}

static int novatel_enable(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->primary = open_device(modem, "PrimaryDevice", "1st:");
	if (!data->primary)
		return -EIO;

	g_at_chat_set_disconnect_function(data->primary,
						novatel_disconnect, modem);

	g_at_chat_send(data->primary, "ATE0 +CMEE=1", none_prefix,
							NULL, NULL, NULL);

	/* Check mode of seconday port */
	g_at_chat_send(data->primary, "AT$NWDMAT?", nwdmat_prefix,
						nwdmat_query, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->primary);
	data->primary = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int novatel_disable(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->primary)
		return 0;

	if (data->secondary) {
		g_at_chat_cancel_all(data->secondary);
		g_at_chat_unregister_all(data->secondary);

		g_at_chat_unref(data->secondary);
		data->secondary = NULL;
	}

	g_at_chat_cancel_all(data->primary);
	g_at_chat_unregister_all(data->primary);

	g_at_chat_send(data->primary, "AT$NWDMAT=0", nwdmat_prefix,
							NULL, NULL, NULL);

	g_at_chat_send(data->primary, "AT+CFUN=0", none_prefix,
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

static void novatel_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct novatel_data *data = ofono_modem_get_data(modem);
	GAtChat *chat = data->primary;
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (!cbd || !chat)
		goto error;

	if (g_at_chat_send(chat, command, NULL, set_online_cb, cbd, g_free))
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void novatel_pre_sim(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	if (!data->secondary) {
		ofono_devinfo_create(modem, 0, "atmodem", data->primary);
		sim = ofono_sim_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
						"atmodem", data->primary);
	} else {
		ofono_devinfo_create(modem, 0, "atmodem", data->secondary);
		sim = ofono_sim_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
						"atmodem", data->secondary);
	}

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void novatel_post_online(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->secondary) {
		ofono_netreg_create(modem, OFONO_VENDOR_NOVATEL, "atmodem",
							data->primary);

		data->gprs = ofono_gprs_create(modem, OFONO_VENDOR_NOVATEL,
						"atmodem", data->primary);
	} else {
		ofono_netreg_create(modem, OFONO_VENDOR_NOVATEL, "atmodem",
							data->secondary);

		ofono_radio_settings_create(modem, 0, "nwmodem",
							data->secondary);

		ofono_sms_create(modem, OFONO_VENDOR_NOVATEL, "atmodem",
							data->secondary);
		ofono_cbs_create(modem, OFONO_VENDOR_QUALCOMM_MSM, "atmodem",
							data->secondary);
		ofono_ussd_create(modem, 0, "atmodem", data->secondary);

		data->gprs = ofono_gprs_create(modem, OFONO_VENDOR_NOVATEL,
						"atmodem", data->secondary);
	}

	data->gc = ofono_gprs_context_create(modem, 0, "atmodem",
							data->primary);

	if (data->gprs && data->gc)
		ofono_gprs_add_context(data->gprs, data->gc);
}

static struct ofono_modem_driver novatel_driver = {
	.name		= "novatel",
	.probe		= novatel_probe,
	.remove		= novatel_remove,
	.enable		= novatel_enable,
	.disable	= novatel_disable,
	.set_online     = novatel_set_online,
	.pre_sim	= novatel_pre_sim,
	.post_online	= novatel_post_online,
};

static int novatel_init(void)
{
	return ofono_modem_driver_register(&novatel_driver);
}

static void novatel_exit(void)
{
	ofono_modem_driver_unregister(&novatel_driver);
}

OFONO_PLUGIN_DEFINE(novatel, "Novatel Wireless modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, novatel_init, novatel_exit)
