/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010-2011  Nokia Corporation and/or its subsidiary(-ies).
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

#include <stdlib.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

#include <drivers/atmodem/atutil.h>
#include <ofono/cdma-voicecall.h>
#include <ofono/devinfo.h>
#include <ofono/cdma-connman.h>

#include "common.h"

struct nokiacdma_data {
	GAtChat *chat;
};

static void nokiacdma_debug(const char *str, void *data)
{
	const char *prefix = data;

	ofono_info("%s%s", prefix, str);
}

static int nokiacdma_probe(struct ofono_modem *modem)
{
	struct nokiacdma_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct nokiacdma_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void nokiacdma_remove(struct ofono_modem *modem)
{
	struct nokiacdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->chat);

	g_free(data);
}

static int nokiacdma_enable(struct ofono_modem *modem)
{
	struct nokiacdma_data *data = ofono_modem_get_data(modem);
	GAtSyntax *syntax;
	GIOChannel *channel;
	const char *device;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		return -EINVAL;

	channel = g_at_tty_open(device, NULL);
	if (channel == NULL)
		return -EIO;

	/*
	 * TODO: Will need a CDMA AT syntax parser later.
	 * Using GSM V1 for now.
	 */
	syntax = g_at_syntax_new_gsmv1();

	data->chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (data->chat == NULL)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, nokiacdma_debug,
					"CDMA Device: ");

	return 0;
}

static int nokiacdma_disable(struct ofono_modem *modem)
{
	struct nokiacdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	return 0;
}

static void nokiacdma_pre_sim(struct ofono_modem *modem)
{
	struct nokiacdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_cdma_voicecall_create(modem, 0, "cdmamodem", data->chat);
	ofono_devinfo_create(modem, 0, "cdmamodem", data->chat);
}

static void nokiacdma_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void nokiacdma_post_online(struct ofono_modem *modem)
{
	struct nokiacdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_cdma_connman_create(modem, 0, "cdmamodem", data->chat);
}

static struct ofono_modem_driver nokiacdma_driver = {
	.name		= "nokiacdma",
	.probe		= nokiacdma_probe,
	.remove		= nokiacdma_remove,
	.enable		= nokiacdma_enable,
	.disable	= nokiacdma_disable,
	.pre_sim	= nokiacdma_pre_sim,
	.post_sim	= nokiacdma_post_sim,
	.post_online	= nokiacdma_post_online,
};

static int nokiacdma_init(void)
{
	return ofono_modem_driver_register(&nokiacdma_driver);
}

static void nokiacdma_exit(void)
{
	ofono_modem_driver_unregister(&nokiacdma_driver);
}

OFONO_PLUGIN_DEFINE(nokiacdma, "Nokia CDMA AT Modem", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			nokiacdma_init, nokiacdma_exit)
