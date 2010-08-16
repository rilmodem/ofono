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
#include <ofono/modem.h>
#include <ofono/log.h>

static int zte_probe(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static void zte_remove(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static int zte_enable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static int zte_disable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static void zte_pre_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void zte_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static struct ofono_modem_driver zte_driver = {
	.name		= "zte",
	.probe		= zte_probe,
	.remove		= zte_remove,
	.enable		= zte_enable,
	.disable	= zte_disable,
	.pre_sim	= zte_pre_sim,
	.post_sim	= zte_post_sim,
};

static int zte_init(void)
{
	return ofono_modem_driver_register(&zte_driver);
}

static void zte_exit(void)
{
	ofono_modem_driver_unregister(&zte_driver);
}

OFONO_PLUGIN_DEFINE(zte, "ZTE modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, zte_init, zte_exit)
