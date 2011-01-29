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
#include <ofono/phonebook.h>
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
static const char *opmn_prefix[] = { "_OPMN:", NULL };
static const char *obls_prefix[] = { "_OBLS:", NULL };

struct hso_data {
	GAtChat *app;
	GAtChat *control;
	guint sim_poll_source;
	guint sim_poll_count;
	gboolean have_sim;
};

static int hso_probe(struct ofono_modem *modem)
{
	struct hso_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct hso_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void hso_remove(struct ofono_modem *modem)
{
	struct hso_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->control);

	if (data->sim_poll_source > 0)
		g_source_remove(data->sim_poll_source);

	g_free(data);
}

static void hso_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static gboolean init_sim_check(gpointer user_data);

static void sim_status(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hso_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int sim, pb, sms;

	DBG("");

	if (data->sim_poll_source > 0) {
		g_source_remove(data->sim_poll_source);
		data->sim_poll_source = 0;
	}

	if (!ok) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "_OBLS:")) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	if (!g_at_result_iter_next_number(&iter, &sim)) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	if (!g_at_result_iter_next_number(&iter, &pb)) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	if (!g_at_result_iter_next_number(&iter, &sms)) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	DBG("status sim %d pb %d sms %d", sim, pb, sms);

	if (sim == 0) {
		data->have_sim = FALSE;

		if (data->sim_poll_count++ < 5) {
			data->sim_poll_source = g_timeout_add_seconds(1,
							init_sim_check, modem);
			return;
		}
	} else
		data->have_sim = TRUE;

	data->sim_poll_count = 0;

	ofono_modem_set_powered(modem, TRUE);

	/*
	 * Option has the concept of Speech Service versus
	 * Data Service. Problem is that in Data Service mode
	 * the card will reject all voice calls. This is a
	 * problem for Multi-SIM cards where one of the SIM
	 * cards is used in a mobile phone and thus incoming
	 * calls would be not signalled on the phone.
	 *
	 *   0 = Speech Service enabled
	 *   1 = Data Service only mode
	 */
	g_at_chat_send(data->app, "AT_ODO?", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->app, "AT_ODO=0", none_prefix, NULL, NULL, NULL);
}

static gboolean init_sim_check(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hso_data *data = ofono_modem_get_data(modem);

	data->sim_poll_source = 0;

	g_at_chat_send(data->control, "AT_OBLS", obls_prefix,
					sim_status, modem, NULL);

	return FALSE;
}

static void check_model(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GAtResultIter iter;
	char const *model;

	DBG("");

	if (!ok)
		goto done;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "_OPMN:"))
		goto done;

	if (g_at_result_iter_next_unquoted_string(&iter, &model))
		ofono_info("Model is %s", model);

done:
	init_sim_check(modem);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hso_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_at_chat_send(data->control, "AT_OPMN", opmn_prefix,
					check_model, modem, NULL);
}

static GAtChat *create_port(const char *device)
{
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	channel = g_at_tty_open(device, NULL);
	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	return chat;
}

static int hso_enable(struct ofono_modem *modem)
{
	struct hso_data *data = ofono_modem_get_data(modem);
	const char *app;
	const char *control;

	DBG("%p", modem);

	control = ofono_modem_get_string(modem, "ControlPort");
	app = ofono_modem_get_string(modem, "ApplicationPort");

	if (app == NULL || control == NULL)
		return -EINVAL;

	data->control = create_port(control);

	if (data->control == NULL)
		return -EIO;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->control, hso_debug, "Control: ");

	data->app = create_port(app);

	if (data->app == NULL) {
		g_at_chat_unref(data->control);
		data->control = NULL;

		return -EIO;
	}

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->app, hso_debug, "App: ");

	g_at_chat_send(data->control, "ATE0", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->app, "ATE0", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(data->control, "AT+CFUN=4", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hso_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->control);
	data->control = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int hso_disable(struct ofono_modem *modem)
{
	struct hso_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->control == NULL)
		return 0;

	g_at_chat_cancel_all(data->control);
	g_at_chat_unregister_all(data->control);

	g_at_chat_unref(data->app);
	data->app = NULL;

	g_at_chat_send(data->control, "AT+CFUN=0", none_prefix,
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

static void hso_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct hso_data *data = ofono_modem_get_data(modem);
	GAtChat *chat = data->control;
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(chat, command, NULL, set_online_cb, cbd, g_free))
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void hso_pre_sim(struct ofono_modem *modem)
{
	struct hso_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->control);
	sim = ofono_sim_create(modem, OFONO_VENDOR_OPTION_HSO,
				"atmodem", data->control);

	if (sim && data->have_sim == TRUE)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void hso_post_sim(struct ofono_modem *modem)
{
	struct hso_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->app);
	ofono_radio_settings_create(modem, 0, "hsomodem", data->app);

	ofono_sms_create(modem, OFONO_VENDOR_OPTION_HSO, "atmodem", data->app);
}

static void hso_post_online(struct ofono_modem *modem)
{
	struct hso_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_OPTION_HSO,
				"atmodem", data->app);

	ofono_cbs_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
				"atmodem", data->app);
	ofono_ussd_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
				"atmodem", data->app);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->app);
	gc = ofono_gprs_context_create(modem, 0, "hsomodem", data->control);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static struct ofono_modem_driver hso_driver = {
	.name		= "hso",
	.probe		= hso_probe,
	.remove		= hso_remove,
	.enable		= hso_enable,
	.disable	= hso_disable,
	.set_online     = hso_set_online,
	.pre_sim	= hso_pre_sim,
	.post_sim	= hso_post_sim,
	.post_online	= hso_post_online,
};

static int hso_init(void)
{
	return ofono_modem_driver_register(&hso_driver);
}

static void hso_exit(void)
{
	ofono_modem_driver_unregister(&hso_driver);
}

OFONO_PLUGIN_DEFINE(hso, "Option HSO modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hso_init, hso_exit)
