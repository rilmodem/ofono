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
#include <ofono/voicecall.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/call-volume.h>
#include <ofono/message-waiting.h>
#include <ofono/ssn.h>
#include <ofono/sim.h>
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/stk.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

#define NUM_DLC  4

#define VOICE_DLC   0
#define NETREG_DLC  1
#define GPRS_DLC    2
#define AUX_DLC     3

struct ifx_data {
	GAtChat *master;
	GAtChat *dlcs[NUM_DLC];
};

static void ifx_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int ifx_probe(struct ofono_modem *modem)
{
	struct ifx_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct ifx_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void ifx_remove(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_free(data);
}

static GAtChat *create_port(const char *device)
{
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return NULL;

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!chat)
		return NULL;

	return chat;
}

static GAtChat *open_device(const char *device, char *debug)
{
	GAtChat *chat;

	chat = create_port(device);
	if (!chat)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, ifx_debug, debug);

	return chat;
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (!ok) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);
}

static int ifx_enable(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	const char *device;
	int i;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		return -EINVAL;

	DBG("%s", device);

	data->master = open_device(device, "");
	if (!data->master)
		return -EIO;

	for (i = 0; i < NUM_DLC; i++)
		data->dlcs[i] = data->master;

	g_at_chat_send(data->master, "ATE0 +CMEE=1", NULL,
					NULL, NULL, NULL);

	g_at_chat_send(data->master, "AT+CFUN=4", NULL,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("");

	for (i = 0; i < NUM_DLC; i++)
		data->dlcs[i] = NULL;

	g_at_chat_unref(data->master);
	data->master = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int ifx_disable(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->master)
		return 0;

	g_at_chat_cancel_all(data->master);
	g_at_chat_unregister_all(data->master);

	g_at_chat_send(data->master, "AT+CFUN=4", NULL,
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

static void ifx_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("%p %s", modem, online ? "online" : "offline");

	if (!cbd)
		goto error;

	if (g_at_chat_send(data->dlcs[AUX_DLC], command, NULL,
					set_online_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ifx_pre_sim(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	sim = ofono_sim_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_voicecall_create(modem, 0, "ifxmodem", data->dlcs[VOICE_DLC]);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void ifx_post_sim(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_stk_create(modem, 0, "ifxmodem", data->dlcs[AUX_DLC]);
	ofono_phonebook_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
}

static void ifx_post_online(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	DBG("%p", modem);

	ofono_radio_settings_create(modem, 0, "ifxmodem", data->dlcs[AUX_DLC]);
	ofono_netreg_create(modem, OFONO_VENDOR_IFX,
					"atmodem", data->dlcs[NETREG_DLC]);

	ofono_sms_create(modem, OFONO_VENDOR_IFX,
					"atmodem", data->dlcs[AUX_DLC]);
	ofono_cbs_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_ussd_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);

	ofono_ssn_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_settings_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_meter_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_barring_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_volume_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver ifx_driver = {
	.name		= "ifx",
	.probe		= ifx_probe,
	.remove		= ifx_remove,
	.enable		= ifx_enable,
	.disable	= ifx_disable,
	.set_online	= ifx_set_online,
	.pre_sim	= ifx_pre_sim,
	.post_sim	= ifx_post_sim,
	.post_online	= ifx_post_online,
};

static int ifx_init(void)
{
	return ofono_modem_driver_register(&ifx_driver);
}

static void ifx_exit(void)
{
	ofono_modem_driver_unregister(&ifx_driver);
}

OFONO_PLUGIN_DEFINE(ifx, "Infineon modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ifx_init, ifx_exit)
