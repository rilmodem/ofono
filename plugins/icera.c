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
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static int icera_probe(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static void icera_remove(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static int icera_enable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static int icera_disable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static void icera_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("%p %s", modem, online ? "online" : "offline");

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void icera_pre_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void icera_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void icera_post_online(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static struct ofono_modem_driver icera_driver = {
	.name		= "icera",
	.probe		= icera_probe,
	.remove		= icera_remove,
	.enable		= icera_enable,
	.disable	= icera_disable,
	.set_online	= icera_set_online,
	.pre_sim	= icera_pre_sim,
	.post_sim	= icera_post_sim,
	.post_online	= icera_post_online,
};

static int icera_init(void)
{
	return ofono_modem_driver_register(&icera_driver);
}

static void icera_exit(void)
{
	ofono_modem_driver_unregister(&icera_driver);
}

OFONO_PLUGIN_DEFINE(icera, "Icera modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, icera_init, icera_exit)
