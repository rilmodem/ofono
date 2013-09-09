/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2013 Canonical Ltd.
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

#include "gril.h"
#include "grilutil.h"
#include "common.h"
#include "rilmodem.h"

/*
 * This module is the ofono_gprs_driver implementation for rilmodem.
 *
 * Notes:
 *
 * 1. ofono_gprs_suspend/resume() are not used by this module, as
 *    the concept of suspended GPRS is not exposed by RILD.
 *
 * 2. ofono_gprs_bearer_notify() is never called as RILD does not
 *    expose an unsolicited event equivalent to +CPSB ( see 27.007
 *    7.29 ), and the tech values returned by REQUEST_DATA/VOICE
 *    _REGISTRATION requests do not match the values defined for
 *    <AcT> in the +CPSB definition.  Note, the values returned by
 *    the *REGISTRATION commands are aligned with those defined by
 *    +CREG ( see 27.003 7.2 ).
 */

struct gprs_data {
	GRil *ril;
	gboolean ofono_attached;
	int max_cids;
	int rild_status;
};

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data);

static void ril_gprs_state_change(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	g_assert(message->req == RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED);

	/*
	 * We are just want to track network data status change if ofono
	 * itself is attached, so we avoid unnecessary data state requests.
	 */
	if (gd->ofono_attached == TRUE)
		ril_gprs_registration_status(gprs, NULL, NULL);
}

static void ril_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ofono_error error;

	DBG("attached: %d", attached);

	decode_ril_error(&error, "OK");

	/*
	 * As RIL offers no actual control over the GPRS 'attached'
	 * state, we save the desired state, and use it to override
	 * the actual modem's state in the 'attached_status' function.
	 * This is similar to the way the core ofono gprs code handles
	 * data roaming ( see src/gprs.c gprs_netreg_update().
	 *
	 * The core gprs code calls driver->set_attached() when a netreg
	 * notificaiton is received and any configured roaming conditions
	 * are met.
	 */
	gd->ofono_attached = attached;

	cb(&error, cbd->data);
	g_free(cbd);
}

static void ril_data_reg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->user;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ofono_error error;
	gboolean attached;
	int status, lac, ci, tech;
	int max_cids = 1;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("ril_data_reg_cb: reply failure: %s",
				ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		error.error = message->error;
		status = -1;
		goto error;
	}

	if (ril_util_parse_reg(gd->ril, message, &status,
				&lac, &ci, &tech, &max_cids) == FALSE) {
		ofono_error("Failure parsing data registration response.");
		decode_ril_error(&error, "FAIL");
		status = -1;
		goto error;
	}

	if (gd->rild_status == -1) {
		ofono_gprs_register(gprs);

		/* RILD tracks data network state together with voice */
		g_ril_register(gd->ril, RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
				ril_gprs_state_change, gprs);
	}

	if (max_cids > gd->max_cids) {
		DBG("Setting max cids to %d", max_cids);
		gd->max_cids = max_cids;
		ofono_gprs_set_cid_range(gprs, 1, max_cids);
	}

	/* Just need to notify ofono if it's already attached */
	if (gd->ofono_attached && (gd->rild_status != status)) {
		ofono_gprs_status_notify(gprs, status);
	}

	gd->rild_status = status;

	/*
	 * Override the actual status based upon the desired
	 * attached status set by the core GPRS code ( controlled
	 * by the ConnnectionManager's 'Powered' property ).
	 */
	attached = (status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
			status == NETWORK_REGISTRATION_STATUS_ROAMING);

	if (attached && gd->ofono_attached == FALSE) {
		DBG("attached=true; ofono_attached=false; return !REGISTERED");
		status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
	}

error:
	if (cb)
		cb(&error, status, cbd->data);
}

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int request = RIL_REQUEST_DATA_REGISTRATION_STATE;
	guint ret;

	cbd->user = gprs;

	ret = g_ril_send(gd->ril, request,
				NULL, 0, ril_data_reg_cb, cbd, g_free);

	g_ril_print_request_no_args(gd->ril, ret, request);

	if (ret <= 0) {
		ofono_error("Send RIL_REQUEST_DATA_RESTISTRATION_STATE failed.");
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static int ril_gprs_probe(struct ofono_gprs *gprs,
				unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct gprs_data *gd;

	gd = g_try_new0(struct gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	gd->ril = g_ril_clone(ril);
	gd->ofono_attached = FALSE;
	gd->max_cids = 0;
	gd->rild_status = -1;

	ofono_gprs_set_data(gprs, gd);

	ril_gprs_registration_status(gprs, NULL, NULL);

	return 0;
}

static void ril_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("");

	ofono_gprs_set_data(gprs, NULL);

	g_ril_unref(gd->ril);
	g_free(gd);
}

static struct ofono_gprs_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_gprs_probe,
	.remove			= ril_gprs_remove,
	.set_attached		= ril_gprs_set_attached,
	.attached_status	= ril_gprs_registration_status,
};

void ril_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void ril_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
