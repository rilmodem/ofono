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
#include <ofono/stk.h>
#include <ofono/sms.h>
#include <ofono/ssn.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

#include <drivers/atmodem/sim-poll.h>

static const char *tty_opts[] = {
	"Baud",
	"Read",
	"Local",
	"StopBits",
	"DataBits",
	"Parity",
	"XonXoff",
	"RtsCts",
	NULL,
};

static int atgen_probe(struct ofono_modem *modem)
{
	return 0;
}

static void atgen_remove(struct ofono_modem *modem)
{
}

static void atgen_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static int atgen_enable(struct ofono_modem *modem)
{
	GAtChat *chat;
	GIOChannel *channel;
	GAtSyntax *syntax;
	const char *device;
	const char *value;
	GHashTable *options;
	int i;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
		return -EINVAL;

	options = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, g_free);
	if (!options)
		return -ENOMEM;

	for (i = 0; tty_opts[i]; i++) {
		value = ofono_modem_get_string(modem, tty_opts[i]);

		if (value == NULL)
			continue;

		g_hash_table_insert(options, g_strdup(tty_opts[i]),
					g_strdup(value));
	}

	channel = g_at_tty_open(device, options);

	g_hash_table_destroy(options);

	if (!channel) {
		return -EIO;
	}

	value = ofono_modem_get_string(modem, "GsmSyntax");
	if (value) {
		if (g_str_equal(value, "V1"))
			syntax = g_at_syntax_new_gsmv1();
		else if (g_str_equal(value, "Permissive"))
			syntax = g_at_syntax_new_gsm_permissive();
		else
			return -EINVAL;
	} else {
		syntax = g_at_syntax_new_gsmv1();
	}

	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!chat)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, atgen_debug, NULL);

	ofono_modem_set_data(modem, chat);

	return 0;
}

static int atgen_disable(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(chat);

	return 0;
}

static void atgen_pre_sim(struct ofono_modem *modem)
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

static void atgen_post_sim(struct ofono_modem *modem)
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
	ofono_ssn_create(modem, 0, "atmodem", chat);
	ofono_sms_create(modem, 0, "atmodem", chat);
	ofono_phonebook_create(modem, 0, "atmodem", chat);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver atgen_driver = {
	.name		= "atgen",
	.probe		= atgen_probe,
	.remove		= atgen_remove,
	.enable		= atgen_enable,
	.disable	= atgen_disable,
	.pre_sim	= atgen_pre_sim,
	.post_sim	= atgen_post_sim,
};

static int atgen_init(void)
{
	return ofono_modem_driver_register(&atgen_driver);
}

static void atgen_exit(void)
{
	ofono_modem_driver_unregister(&atgen_driver);
}

OFONO_PLUGIN_DEFINE(atgen, "Generic AT driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, atgen_init, atgen_exit)
