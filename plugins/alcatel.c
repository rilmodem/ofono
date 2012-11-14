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
#include <ofono/phonebook.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

struct alcatel_data {
	GAtChat *modem;
	GAtChat *aux;
	gboolean have_sim;
	struct at_util_sim_state_query *sim_state_query;
};

static int alcatel_probe(struct ofono_modem *modem)
{
	struct alcatel_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct alcatel_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void alcatel_remove(struct ofono_modem *modem)
{
	struct alcatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup potential SIM state polling */
	at_util_sim_state_query_free(data->sim_state_query);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->aux);

	g_free(data);
}

static void alcatel_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	const char *device;
	GIOChannel *channel;
	GAtSyntax *syntax;
	GAtChat *chat;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	DBG("%s %s", key, device);

	channel = g_at_tty_open(device, NULL);
	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, alcatel_debug, debug);

	return chat;
}

static void sim_state_cb(gboolean present, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct alcatel_data *data = ofono_modem_get_data(modem);

	at_util_sim_state_query_free(data->sim_state_query);
	data->sim_state_query = NULL;

	data->have_sim = present;

	ofono_modem_set_powered(modem, TRUE);

	/* AT&C0 needs to be send separate and on both channel */
	g_at_chat_send(data->modem, "AT&C0", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->aux, "AT&C0", NULL, NULL, NULL, NULL);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct alcatel_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;

		g_at_chat_unref(data->aux);
		data->aux = NULL;

		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	data->sim_state_query = at_util_sim_state_query_new(data->aux,
						2, 20, sim_state_cb, modem,
						NULL);
}

static int alcatel_enable(struct ofono_modem *modem)
{
	struct alcatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = open_device(modem, "Modem", "Modem: ");
	if (data->modem == NULL)
		return -EINVAL;

	data->aux = open_device(modem, "Aux", "Aux: ");
	if (data->aux == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_send(data->modem, "ATE0 +CMEE=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->aux, "ATE0 +CMEE=1", NULL, NULL, NULL, NULL);

	g_at_chat_send(data->aux, "AT+CFUN=1", NULL,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct alcatel_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->aux);
	data->aux = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int alcatel_disable(struct ofono_modem *modem)
{
	struct alcatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	g_at_chat_cancel_all(data->aux);
	g_at_chat_unregister_all(data->aux);

	g_at_chat_send(data->aux, "AT+CFUN=0", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void alcatel_pre_sim(struct ofono_modem *modem)
{
	struct alcatel_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->aux);
	sim = ofono_sim_create(modem, OFONO_VENDOR_ALCATEL,
				"atmodem", data->aux);

	if (sim && data->have_sim == TRUE)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void alcatel_post_sim(struct ofono_modem *modem)
{
	struct alcatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->aux);
}

static void alcatel_post_online(struct ofono_modem *modem)
{
	struct alcatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, 0, "atmodem", data->aux);
}

static struct ofono_modem_driver alcatel_driver = {
	.name		= "alcatel",
	.probe		= alcatel_probe,
	.remove		= alcatel_remove,
	.enable		= alcatel_enable,
	.disable	= alcatel_disable,
	.pre_sim	= alcatel_pre_sim,
	.post_sim	= alcatel_post_sim,
	.post_online	= alcatel_post_online,
};

static int alcatel_init(void)
{
	return ofono_modem_driver_register(&alcatel_driver);
}

static void alcatel_exit(void)
{
	ofono_modem_driver_unregister(&alcatel_driver);
}

OFONO_PLUGIN_DEFINE(alcatel, "Alcatel modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, alcatel_init, alcatel_exit)
