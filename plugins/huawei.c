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
#include <ofono/gprs.h>
#include <ofono/voicecall.h>
#include <ofono/log.h>

#include <drivers/atmodem/vendor.h>

struct huawei_data {
	GAtChat *chat;
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

	g_at_chat_unref(data->chat);
	g_free(data);
}

static void huawei_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, TRUE);
}

static int huawei_enable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtSyntax *syntax;
	GIOChannel *channel;
	const char *device;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
			return -EINVAL;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return -EIO;

	syntax = g_at_syntax_new_gsmv1();
	data->chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!data->chat)
		return -EIO;

	g_at_chat_add_terminator(data->chat, "COMMAND NOT SUPPORT", -1, FALSE);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, huawei_debug, NULL);

	g_at_chat_send(data->chat, "ATE0", NULL, NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=1", NULL,
					cfun_enable, modem, NULL);

	return 0;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_shutdown(data->chat);
	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int huawei_disable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->chat)
		return 0;

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);
	g_at_chat_send(data->chat, "AT+CFUN=0", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void huawei_pre_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	ofono_sim_create(modem, 0, "atmodem", data->chat);
	ofono_voicecall_create(modem, 0, "atmodem", data->chat);
}

static void huawei_post_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_HUAWEI, "atmodem", data->chat);
	ofono_sms_create(modem, OFONO_VENDOR_QUALCOMM_MSM, "atmodem", data->chat);
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
