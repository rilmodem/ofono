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

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

static int ifx_probe(struct ofono_modem *modem)
{
	DBG("");

	return 0;
}

static void ifx_remove(struct ofono_modem *modem)
{
	DBG("");
}

static int ifx_enable(struct ofono_modem *modem)
{
	const char *device;

	DBG("");

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		return -EINVAL;

	return 0;
}

static int ifx_disable(struct ofono_modem *modem)
{
	DBG("");

	return 0;
}

static struct ofono_modem_driver ifx_driver = {
	.name		= "ifx",
	.probe		= ifx_probe,
	.remove		= ifx_remove,
	.enable		= ifx_enable,
	.disable	= ifx_disable,
};

static int ifx_init(void)
{
	return ofono_modem_driver_register(&ifx_driver);
}

static void ifx_exit(void)
{
	ofono_modem_driver_unregister(&ifx_driver);
}

OFONO_PLUGIN_DEFINE(ifx, "Infineon modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ifx_init, ifx_exit)
