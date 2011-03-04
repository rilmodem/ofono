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

#include <drivers/atmodem/vendor.h>


static int wavecom_probe(struct ofono_modem *modem)
{
	return 0;
}

static void wavecom_remove(struct ofono_modem *modem)
{
}

static void wavecom_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int wavecom_enable(struct ofono_modem *modem)
{
	GAtChat *chat;
	GIOChannel *channel;
	GAtSyntax *syntax;
	const char *device;
	GHashTable *options;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		return -EINVAL;

	options = g_hash_table_new(g_str_hash, g_str_equal);

	if (options == NULL)
		return -ENOMEM;

	g_hash_table_insert(options, "Baud", "115200");
	g_hash_table_insert(options, "Parity", "none");
	g_hash_table_insert(options, "StopBits", "1");
	g_hash_table_insert(options, "DataBits", "8");

	channel = g_at_tty_open(device, options);

	g_hash_table_destroy(options);

	if (channel == NULL)
		return -EIO;

	/*
	 * Could not figure out whether it is fully compliant or not, use
	 * permissive for now
	 * */
	syntax = g_at_syntax_new_gsm_permissive();

	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, wavecom_debug, "");

	ofono_modem_set_data(modem, chat);

	return 0;
}

static int wavecom_disable(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(chat);

	return 0;
}

static void wavecom_pre_sim(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", chat);
	ofono_sim_create(modem, OFONO_VENDOR_WAVECOM, "atmodem", chat);
	ofono_voicecall_create(modem, 0, "atmodem", chat);
}

static void wavecom_post_sim(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	DBG("%p", modem);

	ofono_ussd_create(modem, 0, "atmodem", chat);
	ofono_call_forwarding_create(modem, 0, "atmodem", chat);
	ofono_call_settings_create(modem, 0, "atmodem", chat);
	ofono_netreg_create(modem, 0, "atmodem", chat);
	ofono_call_meter_create(modem, 0, "atmodem", chat);
	ofono_call_barring_create(modem, 0, "atmodem", chat);
	ofono_sms_create(modem, 0, "atmodem", chat);
	ofono_phonebook_create(modem, 0, "atmodem", chat);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver wavecom_driver = {
	.name		= "wavecom",
	.probe		= wavecom_probe,
	.remove		= wavecom_remove,
	.enable		= wavecom_enable,
	.disable	= wavecom_disable,
	.pre_sim	= wavecom_pre_sim,
	.post_sim	= wavecom_post_sim,
};

static int wavecom_init(void)
{
	return ofono_modem_driver_register(&wavecom_driver);
}

static void wavecom_exit(void)
{
	ofono_modem_driver_unregister(&wavecom_driver);
}

OFONO_PLUGIN_DEFINE(wavecom, "Wavecom driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, wavecom_init, wavecom_exit)
