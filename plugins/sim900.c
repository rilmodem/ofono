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
#include <gatmux.h>

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
#include <ofono/call-volume.h>
#include <drivers/atmodem/vendor.h>

#define NUM_DLC 5

#define VOICE_DLC   0
#define NETREG_DLC  1
#define SMS_DLC     2
#define GPRS_DLC    3
#define SETUP_DLC   4

static char *dlc_prefixes[NUM_DLC] = { "Voice: ", "Net: ", "SMS: ",
					"GPRS: " , "Setup: "};

static const char *none_prefix[] = { NULL };

struct sim900_data {
	GIOChannel *device;
	GAtMux *mux;
	GAtChat * dlcs[NUM_DLC];
	guint frame_size;
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
	struct sim900_data *data = ofono_modem_get_data(modem);
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
	g_hash_table_insert(options, "Read", "on");

	channel = g_at_tty_open(device, options);
	g_hash_table_destroy(options);

	if (channel == NULL)
		return NULL;

	data->device = channel;
	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);

	if (chat == NULL) {
		g_io_channel_unref(data->device);
		data->device = NULL;

		return NULL;
	}

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, sim900_debug, debug);

	return chat;
}

static GAtChat *create_chat(GIOChannel *channel, struct ofono_modem *modem,
				char *debug)
{
	GAtSyntax *syntax;
	GAtChat *chat;

	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, sim900_debug, debug);

	return chat;
}

static void shutdown_device(struct sim900_data *data)
{
	int i;

	DBG("");

	for (i = 0; i < NUM_DLC; i++) {
		if (data->dlcs[i] == NULL)
			continue;

		g_at_chat_unref(data->dlcs[i]);
		data->dlcs[i] = NULL;
	}

	if (data->mux) {
		g_at_mux_shutdown(data->mux);
		g_at_mux_unref(data->mux);
		data->mux = NULL;
	}

	g_io_channel_unref(data->device);
	data->device = NULL;
}

static void setup_internal_mux(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("");

	data->frame_size = 128;

	data->mux = g_at_mux_new_gsm0710_basic(data->device,
						data->frame_size);
	if (data->mux == NULL)
		goto error;

	if (getenv("OFONO_MUX_DEBUG"))
		g_at_mux_set_debug(data->mux, sim900_debug, "MUX: ");

	if (!g_at_mux_start(data->mux)) {
		g_at_mux_shutdown(data->mux);
		g_at_mux_unref(data->mux);
		goto error;
	}

	for (i = 0; i < NUM_DLC; i++) {
		GIOChannel *channel = g_at_mux_create_channel(data->mux);

		data->dlcs[i] = create_chat(channel, modem, dlc_prefixes[i]);
		if (data->dlcs[i] == NULL) {
			ofono_error("Failed to create channel");
			goto error;
		}
	}

	ofono_modem_set_powered(modem, TRUE);

	return;

error:
	shutdown_device(data);
	ofono_modem_set_powered(modem, FALSE);
}

static void mux_setup_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->dlcs[SETUP_DLC]);
	data->dlcs[SETUP_DLC] = NULL;

	if (!ok)
		goto error;

	setup_internal_mux(modem);

	return;

error:
	shutdown_device(data);
	ofono_modem_set_powered(modem, FALSE);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		g_at_chat_unref(data->dlcs[SETUP_DLC]);
		data->dlcs[SETUP_DLC] = NULL;
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_at_chat_send(data->dlcs[SETUP_DLC],
			"AT+CMUX=0,0,5,128,10,3,30,10,2", NULL,
			mux_setup_cb, modem, NULL);
}

static int sim900_enable(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->dlcs[SETUP_DLC] = open_device(modem, "Device", "Setup: ");
	if (data->dlcs[SETUP_DLC] == NULL)
		return -EINVAL;

	g_at_chat_send(data->dlcs[SETUP_DLC], "ATE0", NULL, NULL, NULL, NULL);

	/* For obtain correct sms service number */
	g_at_chat_send(data->dlcs[SETUP_DLC], "AT+CSCS=\"GSM\"", NULL,
					NULL, NULL, NULL);

	g_at_chat_send(data->dlcs[SETUP_DLC], "AT+CFUN=1", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("");

	shutdown_device(data);

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int sim900_disable(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_send(data->dlcs[SETUP_DLC], "AT+CFUN=4", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void sim900_pre_sim(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	sim = ofono_sim_create(modem, OFONO_VENDOR_SIMCOM, "atmodem",
						data->dlcs[VOICE_DLC]);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void sim900_post_sim(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_sms_create(modem, OFONO_VENDOR_SIMCOM, "atmodem",
						data->dlcs[SMS_DLC]);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->dlcs[GPRS_DLC]);
	if (gprs == NULL)
		return;

	gc = ofono_gprs_context_create(modem, OFONO_VENDOR_SIMCOM,
					"atmodem", data->dlcs[GPRS_DLC]);
	if (gc)
		ofono_gprs_add_context(gprs, gc);
}

static void sim900_post_online(struct ofono_modem *modem)
{
	struct sim900_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_SIMCOM,
			"atmodem", data->dlcs[NETREG_DLC]);
	ofono_ussd_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_voicecall_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_call_volume_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
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
