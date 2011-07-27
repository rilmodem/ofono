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
#include <ofono/cdma-connman.h>
#include <ofono/log.h>

struct huaweicdma_data {
	GAtChat *modem;
	GAtChat *pcui;
};

static void huaweicdma_debug(const char *str, void *data)
{
	const char *prefix = data;

	ofono_info("%s%s", prefix, str);
}

static int huaweicdma_probe(struct ofono_modem *modem)
{
	struct huaweicdma_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct huaweicdma_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void huaweicdma_remove(struct ofono_modem *modem)
{
	struct huaweicdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_free(data);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huaweicdma_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;

		g_at_chat_unref(data->pcui);
		data->pcui = NULL;
	}

	ofono_modem_set_powered(modem, ok);
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
		g_at_chat_set_debug(chat, huaweicdma_debug, debug);

	return chat;
}

static int huaweicdma_enable(struct ofono_modem *modem)
{
	struct huaweicdma_data *data = ofono_modem_get_data(modem);

	DBG("");

	data->modem = open_device(modem, "Modem", "Modem: ");
	if (data->modem == NULL)
		return -EINVAL;

	data->pcui = open_device(modem, "Pcui", "PCUI: ");
	if (data->pcui == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_send(data->modem, "ATE0 &C0 +CMEE=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->pcui, "ATE0 &C0 +CMEE=1", NULL, NULL, NULL, NULL);

	g_at_chat_send(data->pcui, "AT+CFUN=1", NULL,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huaweicdma_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->pcui);
	data->pcui = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int huaweicdma_disable(struct ofono_modem *modem)
{
	struct huaweicdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	g_at_chat_cancel_all(data->pcui);
	g_at_chat_unregister_all(data->pcui);

	g_at_chat_send(data->pcui, "AT+CFUN=0", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void huaweicdma_pre_sim(struct ofono_modem *modem)
{
	struct huaweicdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "cdmamodem", data->pcui);
}

static void huaweicdma_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void huaweicdma_post_online(struct ofono_modem *modem)
{
	struct huaweicdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_cdma_connman_create(modem, 0, "cdmamodem", data->modem);
}

static struct ofono_modem_driver huaweicdma_driver = {
	.name		= "huaweicdma",
	.probe		= huaweicdma_probe,
	.remove		= huaweicdma_remove,
	.enable		= huaweicdma_enable,
	.disable	= huaweicdma_disable,
	.pre_sim	= huaweicdma_pre_sim,
	.post_sim	= huaweicdma_post_sim,
	.post_online	= huaweicdma_post_online,
};

static int huaweicdma_init(void)
{
	return ofono_modem_driver_register(&huaweicdma_driver);
}

static void huaweicdma_exit(void)
{
	ofono_modem_driver_unregister(&huaweicdma_driver);
}

OFONO_PLUGIN_DEFINE(huaweicdma, "Huawei CDMA modem driver", VERSION,
				OFONO_PLUGIN_PRIORITY_DEFAULT,
				huaweicdma_init, huaweicdma_exit)
