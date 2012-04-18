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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/stk.h>
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/log.h>
#include <ofono/location-reporting.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *cfun_prefix[] = { "+CFUN:", NULL };
static const char *none_prefix[] = { NULL };

enum mbm_variant {
	MBM_GENERIC,
	MBM_DELL_D5530,		/* OEM of F3507g */
};

struct mbm_data {
	GAtChat *modem_port;
	GAtChat *data_port;
	gboolean have_sim;
	struct ofono_location_reporting *lr;
	enum mbm_variant variant;
	struct at_util_sim_state_query *sim_state_query;
};

static int mbm_probe(struct ofono_modem *modem)
{
	struct mbm_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct mbm_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void mbm_remove(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup potential SIM state polling */
	at_util_sim_state_query_free(data->sim_state_query);

	g_at_chat_unref(data->data_port);
	g_at_chat_unref(data->modem_port);

	g_free(data);
}

static void mbm_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void d5530_notify(GAtResult *result, gpointer user_data)
{
	DBG("D5530");
}

static void mbm_quirk_d5530(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	data->variant = MBM_DELL_D5530;

	/* This Dell modem sends some unsolicated messages when it boots. */
	/* Try to ignore them. */
	g_at_chat_register(data->modem_port, "D5530", d5530_notify,
				FALSE, NULL, NULL);
	g_at_chat_register(data->modem_port, "+GCAP:", d5530_notify,
				FALSE, NULL, NULL);
}

static void sim_state_cb(gboolean present, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);

	at_util_sim_state_query_free(data->sim_state_query);
	data->sim_state_query = NULL;

	data->have_sim = present;
	ofono_modem_set_powered(modem, TRUE);
}

static void check_model(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	char const *model;

	DBG("");

	if (!ok)
		goto done;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, NULL)) {
		if (!g_at_result_iter_next_unquoted_string(&iter, &model))
			continue;

		if (g_str_equal(model, "D5530"))
			mbm_quirk_d5530(modem);
	}

done:
	data->sim_state_query = at_util_sim_state_query_new(data->modem_port,
								1, 5,
								sim_state_cb,
								modem, NULL);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_at_chat_send(data->modem_port, "AT+CGMM", NULL,
					check_model, modem, NULL);
}

static void cfun_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int status;

	DBG("%d", ok);

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+CFUN:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &status);

	if (status != 4) {
		g_at_chat_send(data->modem_port, "AT+CFUN=4", none_prefix,
				cfun_enable, modem, NULL);
		return;
	}

	cfun_enable(TRUE, NULL, modem);
}

static void emrdy_notifier(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int status;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*EMRDY:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &status);

	if (status != 1)
		return;

	g_at_chat_send(data->modem_port, "AT+CFUN?", cfun_prefix,
					cfun_query, modem, NULL);
}

static void emrdy_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%d", ok);

	if (ok)
		return;

	/* On some MBM hardware the EMRDY cannot be queried, so if this fails
	 * we try to run CFUN? to check the state.  CFUN? will fail unless
	 * EMRDY: 1 has been sent, in which case the emrdy_notifier should be
	 * triggered eventually and we send CFUN? again.
	 */
	g_at_chat_send(data->modem_port, "AT+CFUN?", cfun_prefix,
					cfun_query, modem, NULL);
};

static GAtChat *create_port(const char *device)
{
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;
	GHashTable *options;

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

	return chat;
}

static int mbm_enable(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);
	const char *modem_dev;
	const char *data_dev;

	DBG("%p", modem);

	modem_dev = ofono_modem_get_string(modem, "ModemDevice");
	data_dev = ofono_modem_get_string(modem, "DataDevice");

	DBG("%s, %s", modem_dev, data_dev);

	if (modem_dev == NULL || data_dev == NULL)
		return -EINVAL;

	data->modem_port = create_port(modem_dev);
	if (data->modem_port == NULL)
		return -EIO;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->modem_port, mbm_debug, "Modem: ");

	data->data_port = create_port(data_dev);
	if (data->data_port == NULL) {
		g_at_chat_unref(data->modem_port);
		data->modem_port = NULL;

		return -EIO;
	}

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->data_port, mbm_debug, "Data: ");

	g_at_chat_register(data->modem_port, "*EMRDY:", emrdy_notifier,
					FALSE, modem, NULL);

	g_at_chat_send(data->modem_port, "AT&F E0 V1 X4 &C0 +CMEE=1", NULL,
					NULL, NULL, NULL);
	g_at_chat_send(data->data_port, "AT&F E0 V1 X4 &C0 +CMEE=1", NULL,
					NULL, NULL, NULL);

	g_at_chat_send(data->modem_port, "AT*E2CFUN=1", none_prefix,
					NULL, NULL, NULL);
	g_at_chat_send(data->modem_port, "AT*EMRDY?", none_prefix,
				emrdy_query, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->modem_port);
	data->modem_port = NULL;

	g_at_chat_unref(data->data_port);
	data->data_port = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int mbm_disable(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->modem_port == NULL)
		return 0;

	g_at_chat_cancel_all(data->modem_port);
	g_at_chat_unregister_all(data->modem_port);

	g_at_chat_send(data->modem_port, "AT+CFUN=4", NULL,
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

static void mbm_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct mbm_data *data = ofono_modem_get_data(modem);
	GAtChat *chat = data->modem_port;
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(chat, command, none_prefix,
				set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void mbm_pre_sim(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->modem_port);
	sim = ofono_sim_create(modem, OFONO_VENDOR_MBM,
					"atmodem", data->modem_port);

	if (data->have_sim && sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void mbm_post_sim(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_stk_create(modem, 0, "mbmmodem", data->modem_port);
	ofono_radio_settings_create(modem, 0, "stemodem", data->modem_port);

	ofono_sms_create(modem, 0, "atmodem", data->modem_port);
}

static void mbm_post_online(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	const char *gps_dev;

	DBG("%p", modem);

	gps_dev = ofono_modem_get_string(modem, "GPSDevice");
	if (gps_dev)
		data->lr = ofono_location_reporting_create(modem, 0,
					"mbmmodem", data->modem_port);

	ofono_netreg_create(modem, OFONO_VENDOR_MBM,
					"atmodem", data->modem_port);

	switch (data->variant) {
	case MBM_GENERIC:
		ofono_cbs_create(modem, 0, "atmodem", data->modem_port);
		break;
	case MBM_DELL_D5530:
		/* DELL D5530 crashes when it processes CBSs */
		break;
	}

	ofono_ussd_create(modem, 0, "atmodem", data->modem_port);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_MBM,
					"atmodem", data->modem_port);
	if (gprs == NULL)
		return;

	gc = ofono_gprs_context_create(modem, 0,
					"mbmmodem", data->modem_port);
	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}

	gc = ofono_gprs_context_create(modem, 0,
					"atmodem", data->data_port);
	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_MMS);
		ofono_gprs_add_context(gprs, gc);
	}
}

static struct ofono_modem_driver mbm_driver = {
	.name		= "mbm",
	.probe		= mbm_probe,
	.remove		= mbm_remove,
	.enable		= mbm_enable,
	.disable	= mbm_disable,
	.set_online	= mbm_set_online,
	.pre_sim	= mbm_pre_sim,
	.post_sim	= mbm_post_sim,
	.post_online	= mbm_post_online,
};

static int mbm_init(void)
{
	return ofono_modem_driver_register(&mbm_driver);
}

static void mbm_exit(void)
{
	ofono_modem_driver_unregister(&mbm_driver);
}

OFONO_PLUGIN_DEFINE(mbm, "Ericsson MBM modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, mbm_init, mbm_exit)
