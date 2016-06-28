/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Canonical Ltd.
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

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"
#include "drivers/mtk2modem/mtk2modem.h"
#include "drivers/mtk2modem/mtk2util.h"
#include "gril.h"
#include "ril.h"

static const char *mtk2_get_driver_type(enum ofono_atom_type atom)
{
	switch (atom) {
	case OFONO_ATOM_TYPE_VOICECALL:
	case OFONO_ATOM_TYPE_GPRS:
		return MTK2MODEM;
	default:
		return RILMODEM;
	}
}

static int mtk2_probe(struct ofono_modem *modem)
{
	return ril_create(modem, OFONO_RIL_VENDOR_MTK2,
				mtk2_request_id_to_string,
				mtk2_unsol_request_to_string,
				mtk2_get_driver_type);
}

static struct ofono_modem_driver mtk2_driver = {
	.name = "mtk2",
	.probe = mtk2_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

/*
 * This plugin is a device plugin for MTK modems. It can handle newer modems
 * than those that use the old mtk.c plugin, therefore the name mtk2.
 */
static int mtk2_init(void)
{
	int retval = ofono_modem_driver_register(&mtk2_driver);

	if (retval)
		DBG("ofono_modem_driver_register returned: %d", retval);

	return retval;
}

static void mtk2_exit(void)
{
	DBG("");
	ofono_modem_driver_unregister(&mtk2_driver);
}

OFONO_PLUGIN_DEFINE(mtk2, "MTK v2 modem plugin",
	VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT, mtk2_init, mtk2_exit)
