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
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *sysinfo_prefix[] = { "^SYSINFO:", NULL };

struct huawei_data {
	GAtChat *modem;
	GAtChat *pcui;
	struct ofono_sim *sim;
	gint sim_state;
};

static int huawei_probe(struct ofono_modem *modem)
{
	struct huawei_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct huawei_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void huawei_remove(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->modem);
	g_at_chat_unref(data->pcui);
	g_free(data);
}

static void huawei_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;
	ofono_info("%s%s", prefix, str);
}

static void notify_sim_state(struct ofono_modem *modem, gint sim_state)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	if (data->sim_state == 0 && sim_state == 1) {
		ofono_sim_inserted_notify(data->sim, TRUE);
		data->sim_state = sim_state;
	} else if (data->sim_state == 1 && sim_state == 0) {
		ofono_sim_inserted_notify(data->sim, FALSE);
		data->sim_state = sim_state;
	}
}

static void sysinfo_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	gint sim_state;
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SYSINFO:"))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_next_number(&iter, &sim_state))
		return;

	notify_sim_state(modem, sim_state);
}

static void simst_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GAtResultIter iter;
	int state;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SIMST:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	notify_sim_state(modem, state);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	ofono_modem_set_powered(modem, ok);

	if (!ok)
		return;

	/* follow sim state */
	g_at_chat_register(data->pcui, "^SIMST:", simst_notify,
							FALSE, modem, NULL);

	/* query current sim state */
	g_at_chat_send(data->pcui, "AT^SYSINFO", sysinfo_prefix,
					sysinfo_cb, modem, NULL);
}

static GAtChat *create_port(const char *device)
{
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!chat)
		return NULL;

	return chat;
}

static int huawei_enable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);
	const char *modem_device, *pcui_device;

	DBG("%p", modem);

	modem_device = ofono_modem_get_string(modem, "Modem");
	pcui_device = ofono_modem_get_string(modem, "Pcui");

	if (modem_device == NULL || pcui_device == NULL)
		return -EINVAL;

	data->modem = create_port(modem_device);

	if (data->modem == NULL)
		return -EIO;

	g_at_chat_add_terminator(data->modem, "COMMAND NOT SUPPORT", -1, FALSE);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->modem, huawei_debug, "Modem:");

	data->pcui = create_port(pcui_device);

	if (data->pcui == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_add_terminator(data->pcui, "COMMAND NOT SUPPORT", -1, FALSE);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->pcui, huawei_debug, "Pcui:");

	data->sim_state = 0;

	g_at_chat_send(data->pcui, "ATE0", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(data->pcui, "AT+CFUN=1", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->pcui);
	data->pcui = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int huawei_disable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->modem) {
		g_at_chat_cancel_all(data->modem);
		g_at_chat_unregister_all(data->modem);
		g_at_chat_unref(data->modem);
		data->modem = NULL;
	}

	if (!data->pcui)
		return 0;

	g_at_chat_cancel_all(data->pcui);
	g_at_chat_unregister_all(data->pcui);
	g_at_chat_send(data->pcui, "AT+CFUN=0", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void huawei_pre_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->pcui);
	data->sim = ofono_sim_create(modem, 0, "atmodem", data->pcui);
}

static void huawei_post_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs_context *gc;
	struct ofono_netreg *netreg;
	struct ofono_gprs *gprs;

	DBG("%p", modem);

	netreg = ofono_netreg_create(modem, OFONO_VENDOR_HUAWEI, "atmodem",
								data->pcui);

	ofono_sms_create(modem, OFONO_VENDOR_HUAWEI, "atmodem", data->pcui);
	ofono_cbs_create(modem, OFONO_VENDOR_QUALCOMM_MSM, "atmodem",
								data->pcui);
	ofono_ussd_create(modem, 0, "atmodem", data->pcui);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->pcui);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->modem);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static struct ofono_modem_driver huawei_driver = {
	.name		= "huawei",
	.probe		= huawei_probe,
	.remove		= huawei_remove,
	.enable		= huawei_enable,
	.disable	= huawei_disable,
	.pre_sim	= huawei_pre_sim,
	.post_sim	= huawei_post_sim,
};

static int huawei_init(void)
{
	return ofono_modem_driver_register(&huawei_driver);
}

static void huawei_exit(void)
{
	ofono_modem_driver_unregister(&huawei_driver);
}

OFONO_PLUGIN_DEFINE(huawei, "HUAWEI Mobile modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, huawei_init, huawei_exit)
