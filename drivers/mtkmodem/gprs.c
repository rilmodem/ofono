/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2013-2014  Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>
#include <ofono/types.h>

#include "common.h"

#include "mtkmodem.h"
#include "mtk_constants.h"
#include "mtkrequest.h"
#include "drivers/rilmodem/rilutil.h"
#include "drivers/rilmodem/gprs.h"

/* Time between get data status retries */
#define GET_STATUS_TIMER_MS 5000

/*
 * This module is the ofono_gprs_driver implementation for mtkmodem. Most of the
 * implementation can be found in the rilmodem gprs atom. The main reason for
 * creating a new atom is the need to handle specific MTK requests that are
 * needed to set-up the data call.
 *
 * Notes:
 *
 * 1. ofono_gprs_suspend/resume() are not used by this module, as
 *    the concept of suspended GPRS is not exposed by RILD.
 */

struct gprs_attach_data {
	struct ril_gprs_data* gd;
	gboolean set_attached;
};

static void mtk_gprs_set_connect_type_cb(struct ril_msg *message,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;
	struct gprs_attach_data *attach_data = cbd->user;
	struct ril_gprs_data *gd = attach_data->gd;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(gd->ril, message);

		gd->ofono_attached = attach_data->set_attached;

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}

	g_free(attach_data);
}

static void mtk_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd;
	struct parcel rilp;
	struct gprs_attach_data *attach_data =
		g_try_new0(struct gprs_attach_data, 1);

	if (attach_data == NULL) {
		ofono_error("%s: out of memory", __func__);
		return;
	}

	DBG("attached: %d", attached);

	attach_data->gd = gd;
	attach_data->set_attached = attached;

	cbd = cb_data_new(cb, data, attach_data);

	/* MTK controls attachment with this request, as opposed to rilmodem */

	g_mtk_request_set_gprs_connect_type(gd->ril, attached, &rilp);

	if (g_ril_send(gd->ril, RIL_REQUEST_SET_GPRS_CONNECT_TYPE, &rilp,
			mtk_gprs_set_connect_type_cb, cbd, g_free) == 0) {
		ofono_error("%s: send failed", __func__);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static int mtk_gprs_probe(struct ofono_gprs *gprs,
				unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct ril_gprs_data *gd;

	gd = g_try_new0(struct ril_gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	ril_gprs_start(ril, gprs, gd);

	/*
	 * In MTK the event emitted when the gprs state changes is different
	 * from the one in AOSP ril. Overwrite the one set in parent.
	 */
	gd->state_changed_unsol = RIL_UNSOL_RESPONSE_PS_NETWORK_STATE_CHANGED;

	return 0;
}

static struct ofono_gprs_driver driver = {
	.name			= MTKMODEM,
	.probe			= mtk_gprs_probe,
	.remove			= ril_gprs_remove,
	.set_attached		= mtk_gprs_set_attached,
	.attached_status	= ril_gprs_registration_status,
};

void mtk_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void mtk_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
