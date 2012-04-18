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

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/sim.h>
#include <ofono/netreg.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };

struct samsung_data {
	GAtChat *chat;
	gboolean have_sim;
	struct at_util_sim_state_query *sim_state_query;
};

static void samsung_debug(const char *str, void *data)
{
	const char *prefix = data;

	ofono_info("%s%s", prefix, str);
}

static int samsung_probe(struct ofono_modem *modem)
{
	struct samsung_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct samsung_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void samsung_remove(struct ofono_modem *modem)
{
	struct samsung_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup potential SIM state polling */
	at_util_sim_state_query_free(data->sim_state_query);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->chat);

	g_free(data);
}

static void mode_select(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct samsung_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!ok) {
		g_at_chat_unref(data->chat);
		data->chat = NULL;

		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_at_chat_send(data->chat, "AT+VERSNAME=1,0", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT+VERSNAME=1,1", NULL, NULL, NULL, NULL);

	ofono_modem_set_powered(modem, TRUE);
}

static void sim_state_cb(gboolean present, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct samsung_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	at_util_sim_state_query_free(data->sim_state_query);
	data->sim_state_query = NULL;

	data->have_sim = present;

	g_at_chat_send(data->chat, "AT+MODESELECT=3", none_prefix,
						mode_select, modem, NULL);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct samsung_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!ok) {
		g_at_chat_unref(data->chat);
		data->chat = NULL;

		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	data->sim_state_query = at_util_sim_state_query_new(data->chat,
						1, 5, sim_state_cb, modem,
						NULL);
}

static int samsung_enable(struct ofono_modem *modem)
{
	struct samsung_data *data = ofono_modem_get_data(modem);
	GAtSyntax *syntax;
	GIOChannel *channel;
	GHashTable *options;
	const char *device;

	device = ofono_modem_get_string(modem, "ControlPort");
	if (device == NULL)
		return -EINVAL;

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		return -ENOMEM;

	g_hash_table_insert(options, "Baud", "115200");
	g_hash_table_insert(options, "Parity", "none");
	g_hash_table_insert(options, "StopBits", "1");
	g_hash_table_insert(options, "DataBits", "8");
	g_hash_table_insert(options, "XonXoff", "off");
	g_hash_table_insert(options, "RtsCts", "on");
	g_hash_table_insert(options, "Local", "on");
	g_hash_table_insert(options, "Read", "on");

	channel = g_at_tty_open(device, options);

	g_hash_table_destroy(options);

	if (channel == NULL)
		return -EIO;

	syntax = g_at_syntax_new_gsm_permissive();
	data->chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_unref(channel);

	if (data->chat == NULL)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, samsung_debug, "Device: ");

	g_at_chat_send(data->chat, "ATE0", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT+CMEE=1", NULL, NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=?", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT+CFUN?", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=5", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct samsung_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int samsung_disable(struct ofono_modem *modem)
{
	struct samsung_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);

	g_at_chat_send(data->chat, "AT+MODESELECT=2", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void samsung_pre_sim(struct ofono_modem *modem)
{
	struct samsung_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	sim = ofono_sim_create(modem, 0, "atmodem", data->chat);

	if (sim && data->have_sim == TRUE)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void samsung_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void samsung_post_online(struct ofono_modem *modem)
{
	struct samsung_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_SAMSUNG, "atmodem", data->chat);
}

static struct ofono_modem_driver samsung_driver = {
	.name		= "samsung",
	.probe		= samsung_probe,
	.remove		= samsung_remove,
	.enable		= samsung_enable,
	.disable	= samsung_disable,
	.pre_sim	= samsung_pre_sim,
	.post_sim	= samsung_post_sim,
	.post_online	= samsung_post_online,
};

static int samsung_init(void)
{
	return ofono_modem_driver_register(&samsung_driver);
}

static void samsung_exit(void)
{
	ofono_modem_driver_unregister(&samsung_driver);
}

OFONO_PLUGIN_DEFINE(samsung, "Samsung modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, samsung_init, samsung_exit)
