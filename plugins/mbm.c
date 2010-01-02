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
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/cbs.h>
#include <ofono/ssn.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>
#include <ofono/phonebook.h>
#include <ofono/message-waiting.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/call-volume.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/log.h>

static const char *cfun_prefix[] = { "+CFUN:", NULL };
static const char *none_prefix[] = { NULL };

struct mbm_data {
	GAtChat *chat;
};

static void erinfo_notifier(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	int mode, gsm, umts;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*ERINFO:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &mode);
	g_at_result_iter_next_number(&iter, &gsm);
	g_at_result_iter_next_number(&iter, &umts);

	ofono_info("network capability: GSM %d UMTS %d", gsm, umts);
}

static int mbm_probe(struct ofono_modem *modem)
{
	struct mbm_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct mbm_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void mbm_remove(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->chat);
	g_free(data);
}

static void mbm_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok)
		ofono_modem_set_powered(modem, FALSE);

	ofono_modem_set_powered(modem, TRUE);

	g_at_chat_send(data->chat, "AT*ERINFO=1", none_prefix,
			NULL, NULL, NULL);
	g_at_chat_register(data->chat, "*ERINFO:", erinfo_notifier,
							FALSE, modem, NULL);
}

static void cfun_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int status;

	DBG("%d", ok);

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+CFUN:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &status);

	if (status == 4) {
		g_at_chat_send(data->chat, "AT+CFUN=1", none_prefix,
				cfun_enable, modem, NULL);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);
}

static void emrdy_notifier(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int status;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*EMRDY:") == FALSE)
		return;

	g_at_result_iter_next_number(&iter, &status);

	if (status != 1)
		return;

	g_at_chat_send(data->chat, "AT+CFUN?", cfun_prefix,
					cfun_query, modem, NULL);
}

static void emrdy_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%d", ok);

	if (ok)
		return;

	/* On some MBM hardware the EMRDY cannot be queried, so if this fails
	 * we try to run CFUN? to check the state.  CFUN? will fail unless
	 * EMRDY: 1 has been sent, in which case the emrdy_notifier should be
	 * triggered eventually and we send CFUN? again.
	 */
	g_at_chat_send(data->chat, "AT+CFUN?", cfun_prefix,
					cfun_query, modem, NULL);
};

static int mbm_enable(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);
	GIOChannel *channel;
	GAtSyntax *syntax;
	const char *device;

	DBG("%p", modem);

	device  = ofono_modem_get_string(modem, "ModemDevice");
	if (!device) {
		device = ofono_modem_get_string(modem, "Device");
		if (!device)
			return -EINVAL;
	}

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return -EIO;

	syntax = g_at_syntax_new_gsmv1();
	data->chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!data->chat)
		return -EIO;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, mbm_debug, NULL);

	g_at_chat_register(data->chat, "*EMRDY:", emrdy_notifier,
					FALSE, modem, NULL);

	g_at_chat_send(data->chat, "AT&F E0 V1 X4 &C1 +CMEE=1", NULL,
					NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT*EMRDY?", none_prefix,
				emrdy_query, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_shutdown(data->chat);
	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int mbm_disable(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->chat)
		return 0;

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);
	g_at_chat_send(data->chat, "AT+CFUN=4", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void mbm_pre_sim(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	ofono_sim_create(modem, 0, "atmodem", data->chat);
	ofono_voicecall_create(modem, 0, "atmodem", data->chat);
}

static void mbm_post_sim(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_call_settings_create(modem, 0, "atmodem", data->chat);
	ofono_call_meter_create(modem, 0, "atmodem", data->chat);
	ofono_call_volume_create(modem, 0, "atmodem", data->chat);

	ofono_ussd_create(modem, 0, "atmodem", data->chat);
	ofono_netreg_create(modem, 0, "atmodem", data->chat);
	ofono_phonebook_create(modem, 0, "atmodem", data->chat);
	ofono_ssn_create(modem, 0, "atmodem", data->chat);
	ofono_sms_create(modem, 0, "atmodem", data->chat);
	ofono_cbs_create(modem, 0, "atmodem", data->chat);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->chat);
	gc = ofono_gprs_context_create(modem, 0, "mbm", data->chat);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver mbm_driver = {
	.name		= "mbm",
	.probe		= mbm_probe,
	.remove		= mbm_remove,
	.enable		= mbm_enable,
	.disable	= mbm_disable,
	.pre_sim	= mbm_pre_sim,
	.post_sim	= mbm_post_sim,
};

static int mbm_init(void)
{
	return ofono_modem_driver_register(&mbm_driver);
}

static void mbm_exit(void)
{
	ofono_modem_driver_unregister(&mbm_driver);
}

OFONO_PLUGIN_DEFINE(mbm, "Ericsson MBM modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, mbm_init, mbm_exit)
