/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Nokia Corporation and/or its subsidiary(-ies).
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

#include <string.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/nettime.h>
#include <ofono/types.h>

#include "common.h"

static int example_nettime_probe(struct ofono_nettime_context *context)
{
	ofono_debug("Example Network Time Probe for modem: %p",
			context->modem);
	return 0;
}

static void example_nettime_remove(struct ofono_nettime_context *context)
{
	ofono_debug("Example Network Time Remove for modem: %p",
			context->modem);
}

static void example_nettime_info_received(struct ofono_nettime_context *context,
						struct ofono_network_time *info)
{
	if (info == NULL)
		return;

	ofono_debug("Received a network time notification on modem: %p",
			context->modem);
	ofono_debug("Time: %04d-%02d-%02d %02d:%02d:%02d%c%02d:%02d (DST=%d)",
			info->year, info->mon, info->mday, info->hour,
			info->min, info->sec, info->utcoff > 0 ? '+' : '-',
			info->utcoff / 3600, (info->utcoff % 3600) / 60,
			info->dst);
}

static struct ofono_nettime_driver example_driver = {
	.name		= "Example Network Time",
	.probe		= example_nettime_probe,
	.remove		= example_nettime_remove,
	.info_received	= example_nettime_info_received,
};

static int example_nettime_init(void)
{
	return ofono_nettime_driver_register(&example_driver);
}

static void example_nettime_exit(void)
{
	ofono_nettime_driver_unregister(&example_driver);
}

OFONO_PLUGIN_DEFINE(example_nettime, "Example Network Time Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			example_nettime_init, example_nettime_exit)
