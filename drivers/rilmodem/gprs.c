/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2013 Canonical Ltd.
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

#include "gril.h"
#include "grilutil.h"
#include "common.h"
#include "rilmodem.h"

#include "grilreply.h"
#include "grilrequest.h"
#include "grilunsol.h"

/*
 * This module is the ofono_gprs_driver implementation for rilmodem.
 *
 * Notes:
 *
 * 1. ofono_gprs_suspend/resume() are not used by this module, as
 *    the concept of suspended GPRS is not exposed by RILD.
 */

struct gprs_data {
	GRil *ril;
	gboolean ofono_attached;
	unsigned int max_cids;
	int rild_status;
	int tech;
};

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data);

static void ril_gprs_state_change(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	g_ril_print_unsol_no_args(gd->ril, message);

	/*
	 * We just want to track network data status if ofono
	 * itself is attached, so we avoid unnecessary data state requests.
	 */
	if (gd->ofono_attached == TRUE)
		ril_gprs_registration_status(gprs, NULL, NULL);
}

static gboolean ril_gprs_set_attached_cb(gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;

	DBG("");

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	g_free(cbd);

	/* Run once per g_idle_add() call */
	return FALSE;
}

static void ril_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, NULL);
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("attached: %d", attached);

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

	/*
	 * Call from idle loop, so core can set driver_attached before
	 * the callback is invoked.
	 */
	g_idle_add(ril_gprs_set_attached_cb, cbd);
}

static void ril_data_reg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->user;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct reply_reg_state *reply;
	gboolean attached = FALSE;
	gboolean notify_status = FALSE;
	int old_status;

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: DATA_REGISTRATION_STATE reply failure: %s",
				__func__,
				ril_error_to_string(message->error));
		goto error;
	}

	old_status = gd->rild_status;

	if ((reply = g_ril_reply_parse_reg_state(gd->ril, message)) == NULL)
		goto error;

	/*
	 * There are three cases that can result in this callback
	 * running:
	 *
	 * 1) The driver's probe() method was called, and thus an
	 *    internal call to ril_gprs_registration_status() is
	 *    generated.  No ofono cb exists.
	 *
	 * 2) ril_gprs_state_change() is called due to an unsolicited
	 *    event from RILD.  No ofono cb exists.
	 *
	 * 3) The ofono code code calls the driver's attached_status()
	 *    function.  A valid ofono cb exists.
	 */

	if (gd->rild_status != reply->status) {
		gd->rild_status = reply->status;

		if (cb == NULL)
			notify_status = TRUE;
	}

	/*
	 * Override the actual status based upon the desired
	 * attached status set by the core GPRS code ( controlled
	 * by the ConnnectionManager's 'Powered' property ).
	 */
	attached = (reply->status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
			reply->status == NETWORK_REGISTRATION_STATUS_ROAMING);

	if (attached && gd->ofono_attached == FALSE) {
		DBG("attached=true; ofono_attached=false; return !REGISTERED");
		reply->status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;

		/*
		 * Further optimization so that if ril_status ==
		 * NOT_REGISTERED, ofono_attached == false, and status ==
		 * ROAMING | REGISTERED, then notify gets cleared...
		 *
		 * As is, this results in unecessary status notify calls
		 * when nothing has changed.
		 */
		if (notify_status && reply->status == old_status)
			notify_status = FALSE;
	}

	if (old_status == -1) {
		ofono_gprs_register(gprs);

		/* RILD tracks data network state together with voice */
		g_ril_register(gd->ril,
				RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
				ril_gprs_state_change, gprs);

		if (reply->max_cids)
			gd->max_cids = reply->max_cids;
		else
			gd->max_cids = 1;

		DBG("Setting max cids to %d", gd->max_cids);
		ofono_gprs_set_cid_range(gprs, 1, gd->max_cids);

		/*
		 * This callback is a result of the inital call
		 * to probe(), so should return after registration.
		 */
		return;
	}

	/* Just need to notify ofono if it's already attached */
	if (notify_status) {

		/*
		 * If network disconnect has occurred, call detached_notify()
		 * instead of status_notify().
		 */
		if (!attached &&
			(old_status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
				old_status == NETWORK_REGISTRATION_STATUS_ROAMING)) {
			DBG("calling ofono_gprs_detached_notify()");
			ofono_gprs_detached_notify(gprs);
			reply->tech = PACKET_BEARER_NONE;
		} else {
			DBG("calling ofono_gprs_status_notify()");
			ofono_gprs_status_notify(gprs, reply->status);
		}
	}

	if (gd->tech != reply->tech) {
		gd->tech = reply->tech;
		ofono_gprs_bearer_notify(gprs, reply->tech);
	}

	if (cb)
		CALLBACK_WITH_SUCCESS(cb, reply->status, cbd->data);

	g_free(reply);

	return;
error:

	if (cb)
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data, gprs);
	int request = RIL_REQUEST_DATA_REGISTRATION_STATE;
	gint ret;

	DBG("");

	ret = g_ril_send(gd->ril, request,
				NULL, 0, ril_data_reg_cb, cbd, g_free);

	g_ril_print_request_no_args(gd->ril, ret, request);

	if (!ret) {
		ofono_error("%s: send "
				"RIL_REQUEST_DATA_REGISTRATION_STATE failed",
				__func__);
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
	gd->tech = -1;

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
