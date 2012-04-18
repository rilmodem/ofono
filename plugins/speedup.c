/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
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
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/phonebook.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *creg_prefix[] = { "+CREG:", NULL };
static const char *none_prefix[] = { NULL };

struct speedup_data {
	GAtChat *modem;
	GAtChat *aux;
	gboolean have_sim;
	guint online_poll_source;
	guint online_poll_count;
	struct cb_data *online_cbd;
	struct at_util_sim_state_query *sim_state_query;
};

static int speedup_probe(struct ofono_modem *modem)
{
	struct speedup_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct speedup_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void speedup_remove(struct ofono_modem *modem)
{
	struct speedup_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup potential SIM state polling */
	at_util_sim_state_query_free(data->sim_state_query);

	/* Cleanup potential online enable polling */
	if (data->online_poll_source > 0) {
		g_source_remove(data->online_poll_source);

		g_free(data->online_cbd);
	}

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->aux);

	g_free(data);
}

static void speedup_debug(const char *str, void *user_data)
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
		g_at_chat_set_debug(chat, speedup_debug, debug);

	return chat;
}

static void sim_state_cb(gboolean present, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct speedup_data *data = ofono_modem_get_data(modem);

	at_util_sim_state_query_free(data->sim_state_query);
	data->sim_state_query = NULL;

	data->have_sim = present;

	ofono_modem_set_powered(modem, TRUE);

	/* AT&C0 needs to be send separate and on both channel */
	g_at_chat_send(data->modem, "AT&C0", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->aux, "AT&C0", NULL, NULL, NULL, NULL);

	/*
	 * Ensure that the modem is using GSM character set and not IRA,
	 * otherwise weirdness with umlauts and other non-ASCII characters
	 * can result
	 */
	g_at_chat_send(data->modem, "AT+CSCS=\"GSM\"", none_prefix,
							NULL, NULL, NULL);
	g_at_chat_send(data->aux, "AT+CSCS=\"GSM\"", none_prefix,
							NULL, NULL, NULL);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct speedup_data *data = ofono_modem_get_data(modem);

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

static int speedup_enable(struct ofono_modem *modem)
{
	struct speedup_data *data = ofono_modem_get_data(modem);

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
	struct speedup_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->aux);
	data->aux = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int speedup_disable(struct ofono_modem *modem)
{
	struct speedup_data *data = ofono_modem_get_data(modem);

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

static gboolean creg_online_check(gpointer user_data);

static void creg_online_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct speedup_data *data = user_data;
	ofono_modem_online_cb_t cb = data->online_cbd->cb;

	if (ok) {
		CALLBACK_WITH_SUCCESS(cb, data->online_cbd->data);
		goto done;
	}

	data->online_poll_count++;

	if (data->online_poll_count > 15)
		goto failure;

	data->online_poll_source = g_timeout_add_seconds(2,
						creg_online_check, data);
	return;

failure:
	CALLBACK_WITH_FAILURE(cb, data->online_cbd->data);

done:
	g_free(data->online_cbd);
	data->online_cbd = NULL;
}

static gboolean creg_online_check(gpointer user_data)
{
	struct speedup_data *data = user_data;

	data->online_poll_source = 0;

	g_at_chat_send(data->aux, "AT+CREG=?", creg_prefix,
					creg_online_cb, data, NULL);

	return FALSE;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct speedup_data *data = ofono_modem_get_data(modem);

	if (!ok) {
		ofono_modem_online_cb_t cb = data->online_cbd->cb;

		CALLBACK_WITH_FAILURE(cb, data->online_cbd->data);

		g_free(data->online_cbd);
		data->online_cbd = NULL;
		return;
	}

	data->online_poll_count = 0;

	creg_online_check(data);
}

static void set_offline_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void speedup_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct speedup_data *data = ofono_modem_get_data(modem);

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (online == TRUE) {
		data->online_cbd = cb_data_new(cb, user_data);

		if (g_at_chat_send(data->aux, "AT+CFUN=1", none_prefix,
					set_online_cb, modem, NULL) > 0)
			return;

		g_free(data->online_cbd);
		data->online_cbd = NULL;
	} else {
		struct cb_data *cbd = cb_data_new(cb, user_data);

		if (g_at_chat_send(data->aux, "AT+CFUN=4",
				none_prefix, set_offline_cb, cbd, g_free) > 0)
			return;

		g_free(cbd);
	}

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void speedup_pre_sim(struct ofono_modem *modem)
{
	struct speedup_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->aux);
	sim = ofono_sim_create(modem, OFONO_VENDOR_SPEEDUP,
						"atmodem", data->aux);

	if (sim && data->have_sim == TRUE)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void speedup_post_sim(struct ofono_modem *modem)
{
	struct speedup_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->aux);

	ofono_sms_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
						"atmodem", data->aux);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_SPEEDUP,
						"atmodem", data->aux);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->modem);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static void speedup_post_online(struct ofono_modem *modem)
{
	struct speedup_data *data = ofono_modem_get_data(modem);

	ofono_netreg_create(modem, OFONO_VENDOR_SPEEDUP, "atmodem", data->aux);

	ofono_cbs_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
						"atmodem", data->aux);
	ofono_ussd_create(modem, 0, "speedupmodem", data->aux);
}

static struct ofono_modem_driver speedup_driver = {
	.name		= "speedup",
	.probe		= speedup_probe,
	.remove		= speedup_remove,
	.enable		= speedup_enable,
	.disable	= speedup_disable,
	.set_online	= speedup_set_online,
	.pre_sim	= speedup_pre_sim,
	.post_sim	= speedup_post_sim,
	.post_online	= speedup_post_online,
};

static int speedup_init(void)
{
	return ofono_modem_driver_register(&speedup_driver);
}

static void speedup_exit(void)
{
	ofono_modem_driver_unregister(&speedup_driver);
}

OFONO_PLUGIN_DEFINE(speedup, "Speed Up modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, speedup_init, speedup_exit)
