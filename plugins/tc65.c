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

#include <errno.h>
#include <stdlib.h>

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
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

#include <drivers/atmodem/atutil.h>

#include <ofono/gprs.h>
#include <ofono/gprs-context.h>

static int tc65_probe(struct ofono_modem *modem)
{
	return 0;
}

static void tc65_remove(struct ofono_modem *modem)
{
}

static void tc65_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int tc65_enable(struct ofono_modem *modem)
{
	GAtChat *chat;
	GIOChannel *channel;
	GAtSyntax *syntax;
	GHashTable *options;
	const char *device;

	DBG("%p", modem);

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		return -ENOMEM;

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		return -EINVAL;

	g_hash_table_insert(options, "Baud", "115200");
	g_hash_table_insert(options, "StopBits", "1");
	g_hash_table_insert(options, "DataBits", "8");
	g_hash_table_insert(options, "Parity", "none");
	g_hash_table_insert(options, "XonXoff", "off");
	g_hash_table_insert(options, "RtsCts", "on");
	g_hash_table_insert(options, "Local", "on");
	g_hash_table_insert(options, "Read", "on");

	channel = g_at_tty_open(device, options);
	g_hash_table_destroy(options);

	if (channel == NULL)
		return -EIO;

	/*
	 * TC65 works almost as the 27.007 says. But for example after
	 * AT+CRSM the modem replies with the data in the queried EF and
	 * writes three pairs of <CR><LF> after the data and before OK.
	 */
	syntax = g_at_syntax_new_gsm_permissive();

	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, tc65_debug, "");

	ofono_modem_set_data(modem, chat);

	return 0;
}

static int tc65_disable(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_send(chat, "AT+CFUN=7", NULL, NULL, NULL, NULL);

	g_at_chat_unref(chat);

	return 0;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void tc65_set_online(struct ofono_modem *modem, ofono_bool_t online,
			ofono_modem_online_cb_t cb, void *user_data)
{
	GAtChat *chat = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=7";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(chat, command, NULL, set_online_cb, cbd, g_free))
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void tc65_pre_sim(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", chat);
	sim = ofono_sim_create(modem, 0, "atmodem", chat);
	ofono_voicecall_create(modem, 0, "atmodem", chat);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void tc65_post_sim(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", chat);

	ofono_sms_create(modem, 0, "atmodem", chat);
}

static void tc65_post_online(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_ussd_create(modem, 0, "atmodem", chat);
	ofono_call_forwarding_create(modem, 0, "atmodem", chat);
	ofono_call_settings_create(modem, 0, "atmodem", chat);
	ofono_netreg_create(modem, 0, "atmodem", chat);
	ofono_call_meter_create(modem, 0, "atmodem", chat);
	ofono_call_barring_create(modem, 0, "atmodem", chat);

	gprs = ofono_gprs_create(modem, 0, "atmodem", chat);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", chat);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver tc65_driver = {
	.name                   = "tc65",
	.probe                  = tc65_probe,
	.remove                 = tc65_remove,
	.enable                 = tc65_enable,
	.disable                = tc65_disable,
	.set_online             = tc65_set_online,
	.pre_sim                = tc65_pre_sim,
	.post_sim               = tc65_post_sim,
	.post_online            = tc65_post_online,
};

static int tc65_init(void)
{
	return ofono_modem_driver_register(&tc65_driver);
}

static void tc65_exit(void)
{
	ofono_modem_driver_unregister(&tc65_driver);
}

OFONO_PLUGIN_DEFINE(tc65, "Cinterion TC65 driver plugin", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, tc65_init, tc65_exit)
