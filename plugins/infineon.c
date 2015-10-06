/*
 *
 *  oFono - Open Source Telephony - RIL-based devices: infineon modems
 *
 *  Copyright (C) 2014  Canonical Ltd.
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

#include "ofono.h"

#include "drivers/rilmodem/vendor.h"
#include "ril.h"

static int inf_probe(struct ofono_modem *modem)
{
	return ril_create(modem, OFONO_RIL_VENDOR_INFINEON);
}

static struct ofono_modem_driver infineon_driver = {
	.name = "infineon",
	.probe = inf_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

/*
 * This plugin is a device plugin for infineon modems that use RIL interface.
 * The plugin 'rildev' is used to determine which RIL plugin should be loaded
 * based upon an environment variable.
 */
static int inf_init(void)
{
	int retval = 0;

	retval = ofono_modem_driver_register(&infineon_driver);
	if (retval != 0)
		DBG("ofono_modem_driver_register returned: %d", retval);

	return retval;
}

static void inf_exit(void)
{
	DBG("");
	ofono_modem_driver_unregister(&infineon_driver);
}

OFONO_PLUGIN_DEFINE(infineon, "Infineon modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, inf_init, inf_exit)
