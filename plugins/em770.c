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
#include <ofono/ssn.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/voicecall.h>

#include <drivers/atmodem/vendor.h>

struct em770_data {
	GAtChat *chat;
};

static int em770_probe(struct ofono_modem *modem)
{
	struct em770_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct em770_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void em770_remove(struct ofono_modem *modem)
{
	struct em770_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->chat);
	g_free(data);
}

static void em770_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, TRUE);
}

static int em770_enable(struct ofono_modem *modem)
{
	struct em770_data *data = ofono_modem_get_data(modem);
	GAtSyntax *syntax;
	GIOChannel *channel;
	const char *device;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
			return -EINVAL;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return -EIO;

	syntax = g_at_syntax_new_gsmv1();
	data->chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!data->chat)
		return -EIO;

	g_at_chat_add_terminator(data->chat, "COMMAND NOT SUPPORT", -1, FALSE);
	g_at_chat_add_terminator(data->chat, "TOO MANY PARAMETERS", -1, FALSE);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, em770_debug, NULL);

	g_at_chat_send(data->chat, "ATE0", NULL, NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=1", NULL,
					cfun_enable, modem, NULL);

	return 0;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct em770_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_shutdown(data->chat);
	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int em770_disable(struct ofono_modem *modem)
{
	struct em770_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->chat)
		return 0;

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);
	g_at_chat_send(data->chat, "AT+CFUN=0", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void em770_pre_sim(struct ofono_modem *modem)
{
	struct em770_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	ofono_sim_create(modem, 0, "atmodem", data->chat);
	ofono_voicecall_create(modem, 0, "atmodem", data->chat);
}

static void em770_post_sim(struct ofono_modem *modem)
{
	struct em770_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	DBG("%p", modem);

	ofono_ussd_create(modem, 0, "atmodem", data->chat);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->chat);
	ofono_call_settings_create(modem, 0, "atmodem", data->chat);
	ofono_netreg_create(modem, 0, "atmodem", data->chat);
	ofono_call_meter_create(modem, 0, "atmodem", data->chat);
	ofono_call_barring_create(modem, 0, "atmodem", data->chat);
	ofono_ssn_create(modem, 0, "atmodem", data->chat);
	ofono_sms_create(modem, OFONO_VENDOR_QUALCOMM_MSM, "atmodem", data->chat);
	ofono_phonebook_create(modem, 0, "atmodem", data->chat);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver em770_driver = {
	.name		= "em770",
	.probe		= em770_probe,
	.remove		= em770_remove,
	.enable		= em770_enable,
	.disable	= em770_disable,
	.pre_sim	= em770_pre_sim,
	.post_sim	= em770_post_sim,
};

static int em770_init(void)
{
	return ofono_modem_driver_register(&em770_driver);
}

static void em770_exit(void)
{
	ofono_modem_driver_unregister(&em770_driver);
}

OFONO_PLUGIN_DEFINE(em770, "HUAWEI EM770 modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, em770_init, em770_exit)
