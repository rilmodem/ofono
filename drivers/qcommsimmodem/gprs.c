/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2013-2014  Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
 *  Copyright (C) 2016 Ratchanan Srirattanamet.
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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>

#include "gril.h"

#include "drivers/rilmodem/rilutil.h"
#include "drivers/rilmodem/gprs.h"
#include "qcom_msim_modem.h"
#include "qcom_msim_constants.h"

static void set_data_sub_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;
	struct ril_gprs_data *gd = cbd->user;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(gd->ril, message);

		gd->ofono_attached = 1;

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void qcom_msim_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data, gd);

	DBG("attached: %d", attached);

	/*
	 * This modem use RIL_REQUEST_SET_DATA_SUBSCRIPTION to select which SIM
	 * is used for data connection. We still doesn't have actual control
	 * over 'attached' state. So, we still use the same trick as rilmodem's
	 * gprs atom (save the desired state, and use it to override the actual
	 * modem's state in the 'attached_status' function).
	 *
	 * The core gprs code calls driver->set_attached() when a netreg
	 * notificaiton is received and any configured roaming conditions
	 * are met.
	 */

	if (attached) {
		/*
		 * We don't have a request to see if we're a selected SIM slot.
		 * So, we just send this request. There's no harm sending this
		 * repeatedly anyway.
		 */
		 g_ril_send(gd->ril, QCOM_MSIM_RIL_REQUEST_SET_DATA_SUBSCRIPTION
					, NULL, set_data_sub_cb, cbd, NULL);
	} else {
		/*
		 * We don't actually have a request to unselect the slot.
		 * So, just do nothing.
		 */
		gd->ofono_attached = attached;

		/*
		 * Call from idle loop, so core can set driver_attached before
		 * the callback is invoked.
		 */
		g_idle_add(ril_gprs_set_attached_cb, cbd);
	}
}

static struct ofono_gprs_driver driver = {
	.name			= QCOMMSIMMODEM,
	.probe			= ril_gprs_probe,
	.remove			= ril_gprs_remove,
	.set_attached		= qcom_msim_gprs_set_attached,
	.attached_status	= ril_gprs_registration_status,
};

void qcom_msim_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void qcom_msim_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
