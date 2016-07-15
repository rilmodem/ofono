/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  Canonical Ltd.
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

#define _GNU_SOURCE
#include <errno.h>

#include <glib.h>

#include <ofono/modem.h>
#include <ofono/gprs.h>

#include "common.h"

#include "mtk2util.h"
#include "mtk2modem.h"
#include "mtk2_constants.h"
#include "drivers/mtkmodem/mtkunsol.h"
#include "drivers/rilmodem/rilutil.h"
#include "drivers/rilmodem/gprs.h"

/*
 * This module is the ofono_gprs_driver implementation for mtk2modem. Most of
 * the implementation can be found in the rilmodem gprs atom. The main reason
 * for creating a new atom is the need to handle specific MTK requests.
 */

static int mtk2_gprs_probe(struct ofono_gprs *gprs,
				unsigned int vendor, void *data)
{
	struct ril_gprs_driver_data *driver_data = data;
	struct ril_gprs_data *gd;

	gd = g_try_new0(struct ril_gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	ril_gprs_start(driver_data, gprs, gd);

	/*
	 * In MTK the event emitted when the gprs state changes is different
	 * from the one in AOSP ril. Overwrite the one set in parent.
	 */
	gd->state_changed_unsol =
			MTK2_RIL_UNSOL_RESPONSE_PS_NETWORK_STATE_CHANGED;

	return 0;
}

static struct ofono_gprs_driver driver = {
	.name			= MTK2MODEM,
	.probe			= mtk2_gprs_probe,
	.remove			= ril_gprs_remove,
	.set_attached		= ril_gprs_set_attached,
	.attached_status	= ril_gprs_registration_status,
};

void mtk2_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void mtk2_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
