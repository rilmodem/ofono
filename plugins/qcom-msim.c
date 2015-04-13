/*
 *
 *  oFono - Open Source Telephony - RIL-based devices: Qualcomm multi-sim modems
 *
 *  Copyright (C) 2015 Ratchanan Srirattanamet.
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

static int qcom_msim_probe(struct ofono_modem *modem)
{
	return ril_create(modem, OFONO_RIL_VENDOR_QCOM_MSIM);
}

static struct ofono_modem_driver qcom_msim_driver = {
	.name = "qcom_msim",
	.probe = qcom_msim_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

/*
 * This plugin is a device plugin for Qualcomm's multi-sim device that use
 * RIL interface. The plugin 'rildev' is used to determine which RIL plugin
 * should be loaded based upon an environment variable.
 */
static int qcom_msim_init(void)
{
	int retval = ofono_modem_driver_register(&qcom_msim_driver);

	if (retval)
		DBG("ofono_modem_driver_register returned: %d", retval);

	return retval;
}

static void qcom_msim_exit(void)
{
	DBG("");
	ofono_modem_driver_unregister(&qcom_msim_driver);
}

OFONO_PLUGIN_DEFINE(qcom_msim, "Modem driver for Qualcomm's multi-sim device",
	VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT, qcom_msim_init, qcom_msim_exit)
