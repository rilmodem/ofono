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
#include <stdio.h>
#include <unistd.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *qss_prefix[] = { "#QSS:", NULL };

struct telit_data {
	GAtChat *chat;
	struct ofono_sim *sim;
	guint sim_inserted_source;
};

static void telit_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int telit_probe(struct ofono_modem *modem)
{
	struct telit_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct telit_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void telit_remove(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	if (data->sim_inserted_source > 0)
		g_source_remove(data->sim_inserted_source);

	g_free(data);
}

static gboolean sim_inserted_timeout_cb(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->sim_inserted_source = 0;

	ofono_sim_inserted_notify(data->sim, TRUE);

	return FALSE;
}

static void switch_sim_state_status(struct ofono_modem *modem, int status)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	switch (status) {
	case 0:
		DBG("SIM not inserted");
		ofono_sim_inserted_notify(data->sim, FALSE);
		break;
	case 1:
		DBG("SIM inserted");
		/* We need to sleep a bit */
		data->sim_inserted_source = g_timeout_add_seconds(1,
							sim_inserted_timeout_cb,
							modem);
		break;
	case 2:
		DBG("SIM inserted and PIN unlocked");
		break;
	case 3:
		DBG("SIM inserted and ready");
		break;
	}
}

static void telit_qss_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int status;
	GAtResultIter iter;

	DBG("%p", modem);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "#QSS:"))
		return;

	g_at_result_iter_next_number(&iter, &status);

	switch_sim_state_status(modem, status);
}

static void telit_qss_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int mode;
	int status;
	GAtResultIter iter;
	g_at_result_iter_init(&iter, result);

	DBG("%p", modem);

	if (!g_at_result_iter_next(&iter, "#QSS:"))
		return;

	g_at_result_iter_next_number(&iter, &mode);
	g_at_result_iter_next_number(&iter, &status);

	switch_sim_state_status(modem, status);
}

static void cfun_enable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!ok) {
		g_at_chat_unref(data->chat);
		data->chat = NULL;
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);

	/* Enable sim state notification */
	g_at_chat_send(data->chat, "AT#QSS=1", none_prefix, NULL, NULL, NULL);

	/* Follow sim state */
	g_at_chat_register(data->chat, "#QSS:", telit_qss_notify,
				FALSE, modem, NULL);

	/* Query current sim state */
	g_at_chat_send(data->chat, "AT#QSS?", qss_prefix,
				telit_qss_cb, modem, NULL);
}

static void cfun_disable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (data->sim_inserted_source > 0)
		g_source_remove(data->sim_inserted_source);

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
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
		g_at_chat_set_debug(chat, telit_debug, debug);

	return chat;
}

static int telit_enable(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->chat = open_device(modem, "Modem", "Modem: ");
	if (data->chat == NULL)
		return -EINVAL;

	/*
	 * Disable command echo and
	 * enable the Extended Error Result Codes
	 */
	g_at_chat_send(data->chat, "ATE0 +CMEE=1", none_prefix,
				NULL, NULL, NULL);

	/* Set phone functionality */
	g_at_chat_send(data->chat, "AT+CFUN=4", none_prefix,
				cfun_enable_cb, modem, NULL);

	return -EINPROGRESS;
}

static int telit_disable(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);
	DBG("%p", modem);

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);

	/* Power down modem */
	g_at_chat_send(data->chat, "AT+CFUN=0", none_prefix,
				cfun_disable_cb, modem, NULL);

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

static void telit_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct telit_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	g_at_chat_send(data->chat, command, none_prefix, set_online_cb,
						cbd, g_free);
}

static void telit_pre_sim(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	data->sim = ofono_sim_create(modem, 0, "atmodem", data->chat);
	ofono_voicecall_create(modem, 0, "atmodem", data->chat);
}

static void telit_post_sim(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_sms_create(modem, 0, "atmodem", data->chat);
}

static void telit_post_online(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_TELIT, "atmodem", data->chat);
	ofono_ussd_create(modem, 0, "atmodem", data->chat);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->chat);
	ofono_call_settings_create(modem, 0, "atmodem", data->chat);
	ofono_call_meter_create(modem, 0, "atmodem", data->chat);
	ofono_call_barring_create(modem, 0, "atmodem", data->chat);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->chat);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->chat);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver telit_driver = {
	.name		= "telit",
	.probe		= telit_probe,
	.remove		= telit_remove,
	.enable		= telit_enable,
	.disable	= telit_disable,
	.set_online	= telit_set_online,
	.pre_sim	= telit_pre_sim,
	.post_sim	= telit_post_sim,
	.post_online	= telit_post_online,
};

static int telit_init(void)
{
	return ofono_modem_driver_register(&telit_driver);
}

static void telit_exit(void)
{
	ofono_modem_driver_unregister(&telit_driver);
}

OFONO_PLUGIN_DEFINE(telit, "telit driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, telit_init, telit_exit)
