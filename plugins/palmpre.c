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
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/phonebook.h>
#include <ofono/voicecall.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/sms.h>

#include <drivers/atmodem/vendor.h>

struct palmpre_data {
	GAtChat *chat;
};

static int palmpre_probe(struct ofono_modem *modem)
{
	struct palmpre_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct palmpre_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void palmpre_remove(struct ofono_modem *modem)
{
	struct palmpre_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->chat);
	g_free(data);
}

static void palmpre_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void cfun_set_on_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	ofono_modem_set_powered(modem, ok);
}

static int palmpre_enable(struct ofono_modem *modem)
{
	struct palmpre_data *data = ofono_modem_get_data(modem);
	GIOChannel *io;
	GAtSyntax *syntax;
	const char *device;
	GHashTable *options;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		device = "/dev/modem0";

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		return -ENOMEM;

	g_hash_table_insert(options, "Baud", "115200");

	io = g_at_tty_open(device, options);
	g_hash_table_destroy(options);

	if (io == NULL)
		return -EIO;

	syntax = g_at_syntax_new_gsm_permissive();
	data->chat = g_at_chat_new(io, syntax);
	g_io_channel_unref(io);
	g_at_syntax_unref(syntax);

	if (data->chat == NULL)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, palmpre_debug, "");

	/* Ensure terminal is in a known state */
	g_at_chat_send(data->chat, "ATZ E0 +CMEE=1", NULL, NULL, NULL, NULL);

	/* Power modem up */
	g_at_chat_send(data->chat, "AT+CFUN=1", NULL,
			cfun_set_on_cb, modem, NULL);

	return 0;
}

static void cfun_set_off_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct palmpre_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int palmpre_disable(struct ofono_modem *modem)
{
	struct palmpre_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	/* Power modem down */
	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);
	g_at_chat_send(data->chat, "AT+CFUN=0", NULL,
			cfun_set_off_cb, modem, NULL);

	return -EINPROGRESS;
}

static void palmpre_pre_sim(struct ofono_modem *modem)
{
	struct palmpre_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	sim = ofono_sim_create(modem, OFONO_VENDOR_QUALCOMM_MSM, "atmodem",
				data->chat);
	ofono_voicecall_create(modem, 0, "atmodem", data->chat);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void palmpre_post_sim(struct ofono_modem *modem)
{
	struct palmpre_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_netreg_create(modem, 0, "atmodem", data->chat);
	ofono_sms_create(modem, OFONO_VENDOR_QUALCOMM_MSM, "atmodem",
				data->chat);
	ofono_phonebook_create(modem, 0, "atmodem", data->chat);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->chat);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->chat);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver palmpre_driver = {
	.name		= "palmpre",
	.probe		= palmpre_probe,
	.remove		= palmpre_remove,
	.enable		= palmpre_enable,
	.disable	= palmpre_disable,
	.pre_sim	= palmpre_pre_sim,
	.post_sim	= palmpre_post_sim
};

static int palmpre_init(void)
{
	return ofono_modem_driver_register(&palmpre_driver);
}

static void palmpre_exit(void)
{
	ofono_modem_driver_unregister(&palmpre_driver);
}

OFONO_PLUGIN_DEFINE(palmpre, "Palm Pre driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, palmpre_init, palmpre_exit)
