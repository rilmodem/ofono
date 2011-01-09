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

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

static int gobi_probe(struct ofono_modem *modem)
{
	DBG("");

	return 0;
}

static void gobi_remove(struct ofono_modem *modem)
{
	DBG("");
}

static struct ofono_modem_driver gobi_driver = {
	.name		= "gobi",
	.probe		= gobi_probe,
	.remove		= gobi_remove,
};

static int gobi_init(void)
{
	return ofono_modem_driver_register(&gobi_driver);
}

static void gobi_exit(void)
{
	ofono_modem_driver_unregister(&gobi_driver);
}

OFONO_PLUGIN_DEFINE(gobi, "Qualcomm Gobi modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, gobi_init, gobi_exit)
