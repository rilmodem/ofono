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
#include <ofono/voicecall.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/ssn.h>
#include <ofono/phonebook.h>
#include <ofono/message-waiting.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *sysinfo_prefix[] = { "^SYSINFO:", NULL };
static const char *ussdmode_prefix[] = { "^USSDMODE:", NULL };
static const char *cvoice_prefix[] = { "^CVOICE:", NULL };

enum huawei_sim_state {
	HUAWEI_SIM_STATE_INVALID_OR_LOCKED =	0,
	HUAWEI_SIM_STATE_VALID =		1,
	HUAWEI_SIM_STATE_INVALID_CS =		2,
	HUAWEI_SIM_STATE_INVALID_PS =		3,
	HUAWEI_SIM_STATE_INVALID_PS_AND_CS =	4,
	HUAWEI_SIM_STATE_NOT_EXISTENT =		255
};

struct huawei_data {
	GAtChat *modem;
	GAtChat *pcui;
	struct ofono_sim *sim;
	enum huawei_sim_state sim_state;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	gboolean voice;
};

static int huawei_probe(struct ofono_modem *modem)
{
	struct huawei_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct huawei_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void huawei_remove(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->modem);
	g_at_chat_unref(data->pcui);
	g_free(data);
}

static void huawei_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;
	ofono_info("%s%s", prefix, str);
}

static void ussdmode_query_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct huawei_data *data = user_data;
	GAtResultIter iter;
	gint ussdmode;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^USSDMODE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &ussdmode))
		return;

	if (ussdmode == 0)
		return;

	/* set USSD mode to text mode */
	g_at_chat_send(data->pcui, "AT^USSDMODE=0", none_prefix,
						NULL, NULL, NULL);
}

static void ussdmode_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct huawei_data *data = user_data;
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^USSDMODE:"))
		return;

	/* query current USSD mode */
	g_at_chat_send(data->pcui, "AT^USSDMODE?", ussdmode_prefix,
					ussdmode_query_cb, data, NULL);
}

static void notify_sim_state(struct ofono_modem *modem,
				enum huawei_sim_state sim_state)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	if (sim_state == HUAWEI_SIM_STATE_NOT_EXISTENT)
		ofono_sim_inserted_notify(data->sim, FALSE);
	else
		ofono_sim_inserted_notify(data->sim, TRUE);

	data->sim_state = sim_state;
}

static void sysinfo_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	gint sim_state;
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SYSINFO:"))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_next_number(&iter, &sim_state))
		return;

	notify_sim_state(modem, (enum huawei_sim_state) sim_state);
}

static void simst_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GAtResultIter iter;
	gint sim_state;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SIMST:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &sim_state))
		return;

	notify_sim_state(modem, (enum huawei_sim_state) sim_state);
}

static void orig_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	gint call_id, call_type;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^ORIG:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_type))
		return;

	ofono_info("Call origin: id %d type %d", call_id, call_type);
}

static void conf_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	gint call_id;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CONF:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	ofono_info("Call setup: id %d", call_id);
}

static void conn_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	gint call_id, call_type;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CONN:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_type))
		return;

	ofono_info("Call connect: id %d type %d", call_id, call_type);
}

static void cend_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	gint call_id, duration, end_status, cc_pause;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CEND:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	if (!g_at_result_iter_next_number(&iter, &duration))
		return;

	if (!g_at_result_iter_next_number(&iter, &end_status))
		return;

	if (!g_at_result_iter_next_number(&iter, &cc_pause))
		return;

	ofono_info("Call end: id %d duration %ds status %d control %d",
				call_id, duration, end_status, cc_pause);
}


static void cvoice_query_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	gint mode, rate, bits, period;

	if (!ok)
		goto done;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CVOICE:"))
		goto done;

	if (!g_at_result_iter_next_number(&iter, &mode))
		goto done;

	if (!g_at_result_iter_next_number(&iter, &rate))
		goto done;

	if (!g_at_result_iter_next_number(&iter, &bits))
		goto done;

	if (!g_at_result_iter_next_number(&iter, &period))
		goto done;

	data->voice = TRUE;

	ofono_info("Voice channel: %d Hz, %d bits, %dms period",
						rate, bits, period);

	g_at_chat_register(data->pcui, "^ORIG:", orig_notify,
						FALSE, NULL, NULL);
	g_at_chat_register(data->pcui, "^CONF:", conf_notify,
						FALSE, NULL, NULL);
	g_at_chat_register(data->pcui, "^CONN:", conn_notify,
						FALSE, NULL, NULL);
	g_at_chat_register(data->pcui, "^CEND:", cend_notify,
						FALSE, NULL, NULL);

	/* check available voice ports */
	g_at_chat_send(data->pcui, "AT^DDSETEX=?", none_prefix,
						NULL, NULL, NULL);

done:
	ofono_modem_set_powered(modem, TRUE);

	/* query current sim state */
        g_at_chat_send(data->pcui, "AT^SYSINFO", sysinfo_prefix,
						sysinfo_cb, modem, NULL);
}

static void cvoice_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;

	if (!ok)
		goto done;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CVOICE:"))
		goto done;

	/* query current voice setting */
	g_at_chat_send(data->pcui, "AT^CVOICE?", cvoice_prefix,
					cvoice_query_cb, modem, NULL);

	return;

done:
	ofono_modem_set_powered(modem, TRUE);

	/* query current sim state */
	g_at_chat_send(data->pcui, "AT^SYSINFO", sysinfo_prefix,
						sysinfo_cb, modem, NULL);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	/* follow sim state */
	g_at_chat_register(data->pcui, "^SIMST:", simst_notify,
						FALSE, modem, NULL);

	/* query current device settings */
	g_at_chat_send(data->pcui, "AT^U2DIAG?", none_prefix,
						NULL, NULL, NULL);

	/* query current port settings */
	g_at_chat_send(data->pcui, "AT^GETPORTMODE", none_prefix,
						NULL, NULL, NULL);

	/* check USSD mode support */
	g_at_chat_send(data->pcui, "AT^USSDMODE=?", ussdmode_prefix,
					ussdmode_support_cb, data, NULL);

	/* check for voice support */
	g_at_chat_send(data->pcui, "AT^CVOICE=?", cvoice_prefix,
					cvoice_support_cb, modem, NULL);
}

static GAtChat *create_port(const char *device)
{
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!chat)
		return NULL;

	return chat;
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	const char *device;
	GAtChat *chat;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	DBG("%s %s", key, device);

	chat = create_port(device);
	if (chat == NULL)
		return NULL;

	g_at_chat_add_terminator(chat, "COMMAND NOT SUPPORT", -1, FALSE);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, huawei_debug, debug);

	return chat;
}

static void huawei_disconnect(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (data->gc)
		ofono_gprs_context_remove(data->gc);

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	data->modem = open_device(modem, "Modem", "Modem:");
	if (data->modem == NULL)
		return;

	g_at_chat_set_disconnect_function(data->modem,
						huawei_disconnect, modem);

	if (data->sim_state == HUAWEI_SIM_STATE_VALID ||
			data->sim_state == HUAWEI_SIM_STATE_INVALID_CS) {
		ofono_info("Reopened GPRS context channel");

		data->gc = ofono_gprs_context_create(modem, 0, "atmodem",
								data->modem);

		if (data->gprs && data->gc)
			ofono_gprs_add_context(data->gprs, data->gc);
	}
}

static int huawei_enable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = open_device(modem, "Modem", "Modem:");
	if (data->modem == NULL)
		return -EINVAL;

	g_at_chat_set_disconnect_function(data->modem,
						huawei_disconnect, modem);

	data->pcui = open_device(modem, "Pcui", "Pcui:");
	if (data->pcui == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	if (ofono_modem_get_boolean(modem, "HasVoice") == TRUE)
		data->voice = TRUE;

	data->sim_state = 0;

	g_at_chat_send(data->pcui, "ATE0", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(data->pcui, "AT+CFUN=1;+CFUN=5", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->pcui);
	data->pcui = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int huawei_disable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->modem) {
		g_at_chat_cancel_all(data->modem);
		g_at_chat_unregister_all(data->modem);
		g_at_chat_unref(data->modem);
		data->modem = NULL;
	}

	if (!data->pcui)
		return 0;

	g_at_chat_cancel_all(data->pcui);
	g_at_chat_unregister_all(data->pcui);
	g_at_chat_send(data->pcui, "AT+CFUN=0", none_prefix,
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

static void huawei_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtChat *chat = data->pcui;
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=5";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, command, NULL, set_online_cb, cbd, g_free))
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void huawei_pre_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->pcui);
	data->sim = ofono_sim_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
					"atmodem", data->pcui);

	if (data->voice == TRUE)
		ofono_voicecall_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem", data->pcui);
}

static void huawei_post_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->pcui);
}

static void huawei_post_online(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);
	struct ofono_netreg *netreg;
	struct ofono_message_waiting *mw;

	if (data->sim_state == HUAWEI_SIM_STATE_INVALID_PS_AND_CS)
		return;

	netreg = ofono_netreg_create(modem, OFONO_VENDOR_HUAWEI, "atmodem",
								data->pcui);

	ofono_sms_create(modem, OFONO_VENDOR_HUAWEI, "atmodem", data->pcui);
	ofono_cbs_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
						"atmodem", data->pcui);
	ofono_ussd_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
						"atmodem", data->pcui);

	if (data->sim_state == HUAWEI_SIM_STATE_VALID ||
			data->sim_state == HUAWEI_SIM_STATE_INVALID_CS) {
		data->gprs = ofono_gprs_create(modem, 0, "atmodem", data->pcui);
		data->gc = ofono_gprs_context_create(modem, 0, "atmodem",
								data->modem);

		if (data->gprs && data->gc)
			ofono_gprs_add_context(data->gprs, data->gc);
	}

	if ((data->sim_state == HUAWEI_SIM_STATE_VALID ||
			data->sim_state == HUAWEI_SIM_STATE_INVALID_PS) &&
							data->voice == TRUE) {
		ofono_call_forwarding_create(modem, 0, "atmodem", data->pcui);
		ofono_call_settings_create(modem, 0, "atmodem", data->pcui);
		ofono_call_barring_create(modem, 0, "atmodem", data->pcui);
		ofono_ssn_create(modem, 0, "atmodem", data->pcui);

		mw = ofono_message_waiting_create(modem);
		if (mw)
			ofono_message_waiting_register(mw);
	}
}

static struct ofono_modem_driver huawei_driver = {
	.name		= "huawei",
	.probe		= huawei_probe,
	.remove		= huawei_remove,
	.enable		= huawei_enable,
	.disable	= huawei_disable,
	.set_online     = huawei_set_online,
	.pre_sim	= huawei_pre_sim,
	.post_sim	= huawei_post_sim,
	.post_online    = huawei_post_online,
};

static int huawei_init(void)
{
	return ofono_modem_driver_register(&huawei_driver);
}

static void huawei_exit(void)
{
	ofono_modem_driver_unregister(&huawei_driver);
}

OFONO_PLUGIN_DEFINE(huawei, "HUAWEI Mobile modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, huawei_init, huawei_exit)
