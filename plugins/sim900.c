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

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/phonebook.h>
#include <ofono/history.h>
#include <ofono/log.h>
#include <ofono/voicecall.h>

#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };

struct sim900_data {
	GAtChat *modem;
};

static int sim900_probe(struct ofono_modem *modem)
{
	struct sim900_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct sim900_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void sim900_remove(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->modem);

	g_free(data);
}

static void sim900_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	const char *device;
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;
	GHashTable *options;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	DBG("%s %s", key, device);

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		return NULL;

	g_hash_table_insert(options, "Baud", "115200");
	g_hash_table_insert(options, "Parity", "none");
	g_hash_table_insert(options, "StopBits", "1");
	g_hash_table_insert(options, "DataBits", "8");
	g_hash_table_insert(options, "XonXoff", "off");
	g_hash_table_insert(options, "Local", "off");
	g_hash_table_insert(options, "RtsCts", "off");

	channel = g_at_tty_open(device, options);
	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, sim900_debug, debug);

	return chat;
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
	}

	ofono_modem_set_powered(modem, ok);
}

static int sim900_enable(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = open_device(modem, "Device", "Device: ");
	if (data->modem == NULL) {
		DBG("return -EINVAL");
		return -EINVAL;
	}

	g_at_chat_send(data->modem, "ATE0", NULL, NULL, NULL, NULL);

	/* For obtain correct sms service number */
	g_at_chat_send(data->modem, "AT+CSCS=\"GSM\"", NULL,
					NULL, NULL, NULL);

	g_at_chat_send(data->modem, "AT+CFUN=1", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}


static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int sim900_disable(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_send(data->modem, "AT+CFUN=4", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void sim900_pre_sim(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->modem);
	sim = ofono_sim_create(modem, OFONO_VENDOR_SIMCOM, "atmodem",
				data->modem);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void sim900_post_sim(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->modem);
	ofono_sms_create(modem, OFONO_VENDOR_SIMCOM, "atmodem",
				data->modem);
}

static void sim900_post_online(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_SIMCOM, "atmodem", data->modem);
	ofono_ussd_create(modem, 0, "atmodem", data->modem);
	ofono_voicecall_create(modem, 0, "atmodem", data->modem);
}

static struct ofono_modem_driver sim900_driver = {
	.name		= "sim900",
	.probe		= sim900_probe,
	.remove		= sim900_remove,
	.enable		= sim900_enable,
	.disable	= sim900_disable,
	.pre_sim	= sim900_pre_sim,
	.post_sim	= sim900_post_sim,
	.post_online	= sim900_post_online,
};

static int sim900_init(void)
{
	return ofono_modem_driver_register(&sim900_driver);
}

static void sim900_exit(void)
{
	ofono_modem_driver_unregister(&sim900_driver);
}

OFONO_PLUGIN_DEFINE(sim900, "SIM900 modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, sim900_init, sim900_exit)
