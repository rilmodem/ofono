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
#include "gprs.h"

/* Time between get data status retries */
#define GET_STATUS_TIMER_MS 5000

/*
 * This module is the ofono_gprs_driver implementation for rilmodem.
 *
 * Notes:
 *
 * 1. ofono_gprs_suspend/resume() are not used by this module, as
 *    the concept of suspended GPRS is not exposed by RILD.
 */

static int ril_tech_to_bearer_tech(int ril_tech)
{
	/*
	 * This code handles the mapping between the RIL_RadioTechnology
	 * and packet bearer values ( see <curr_bearer> values - 27.007
	 * Section 7.29 ).
	 */

	switch (ril_tech) {
	case RADIO_TECH_GSM:
	case RADIO_TECH_UNKNOWN:
		return PACKET_BEARER_NONE;
	case RADIO_TECH_GPRS:
		return PACKET_BEARER_GPRS;
	case RADIO_TECH_EDGE:
		return PACKET_BEARER_EGPRS;
	case RADIO_TECH_UMTS:
		return PACKET_BEARER_UMTS;
	case RADIO_TECH_HSDPA:
		return PACKET_BEARER_HSDPA;
	case RADIO_TECH_HSUPA:
		return PACKET_BEARER_HSUPA;
	case RADIO_TECH_HSPAP:
	case RADIO_TECH_HSPA:
		/*
		 * HSPAP is HSPA+; which ofono doesn't define;
		 * so, if differentiating HSPA and HSPA+ is
		 * important, then ofono needs to be patched,
		 * and we probably also need to introduce a
		 * new indicator icon.
		 */
		return PACKET_BEARER_HSUPA_HSDPA;
	case RADIO_TECH_LTE:
		return PACKET_BEARER_EPS;
	default:
		return PACKET_BEARER_NONE;
	}
}

static void ril_gprs_state_change(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	g_ril_print_unsol_no_args(gd->ril, message);

	/*
	 * We just want to track network data status if ofono
	 * itself is attached, so we avoid unnecessary data state requests.
	 */
	if (gd->ofono_attached == TRUE)
		ril_gprs_registration_status(gprs, NULL, NULL);
}

gboolean ril_gprs_set_attached_cb(gpointer user_data)
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
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

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

static gboolean ril_get_status_retry(gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;

	ril_gprs_registration_status(gprs, NULL, NULL);

	return FALSE;
}

static void ril_data_reg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->user;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct reply_data_reg_state *reply;
	gboolean attached = FALSE;
	gboolean notify_status = FALSE;
	int old_status;

	old_status = gd->rild_status;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: DATA_REGISTRATION_STATE reply failure: %s",
				__func__,
				ril_error_to_string(message->error));
		goto error;
	}

	if ((reply = g_ril_reply_parse_data_reg_state(gd->ril, message))
			== NULL)
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

	if (gd->rild_status != reply->reg_state.status) {
		gd->rild_status = reply->reg_state.status;

		if (cb == NULL)
			notify_status = TRUE;
	}

	/*
	 * Override the actual status based upon the desired
	 * attached status set by the core GPRS code ( controlled
	 * by the ConnnectionManager's 'Powered' property ).
	 */
	attached = (reply->reg_state.status ==
				NETWORK_REGISTRATION_STATUS_REGISTERED ||
			reply->reg_state.status ==
				NETWORK_REGISTRATION_STATUS_ROAMING);

	if (attached && gd->ofono_attached == FALSE) {
		DBG("attached=true; ofono_attached=false; return !REGISTERED");
		reply->reg_state.status =
			NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;

		/*
		 * Further optimization so that if ril_status ==
		 * NOT_REGISTERED, ofono_attached == false, and status ==
		 * ROAMING | REGISTERED, then notify gets cleared...
		 *
		 * As is, this results in unecessary status notify calls
		 * when nothing has changed.
		 */
		if (notify_status && reply->reg_state.status == old_status)
			notify_status = FALSE;
	}

	if (old_status == -1) {
		ofono_gprs_register(gprs);

		/* Different rild implementations use different events here */
		g_ril_register(gd->ril,
				gd->state_changed_unsol,
				ril_gprs_state_change, gprs);

		if (reply->max_cids == 0)
			gd->max_cids = RIL_MAX_NUM_ACTIVE_DATA_CALLS;
		else if (reply->max_cids < RIL_MAX_NUM_ACTIVE_DATA_CALLS)
			gd->max_cids = reply->max_cids;
		else
			gd->max_cids = RIL_MAX_NUM_ACTIVE_DATA_CALLS;

		DBG("Setting max cids to %d", gd->max_cids);
		ofono_gprs_set_cid_range(gprs, 1, gd->max_cids);

		/*
		 * This callback is a result of the inital call
		 * to probe(), so should return after registration.
		 */
		g_free(reply);

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
				old_status ==
					NETWORK_REGISTRATION_STATUS_ROAMING)) {
			DBG("calling ofono_gprs_detached_notify()");
			ofono_gprs_detached_notify(gprs);
			reply->reg_state.tech = RADIO_TECH_UNKNOWN;
		} else {
			DBG("calling ofono_gprs_status_notify()");
			ofono_gprs_status_notify(gprs, reply->reg_state.status);
		}
	}

	if (gd->tech != reply->reg_state.tech) {
		gd->tech = reply->reg_state.tech;

		ofono_gprs_bearer_notify(gprs,
				ril_tech_to_bearer_tech(reply->reg_state.tech));
	}

	if (cb)
		CALLBACK_WITH_SUCCESS(cb, reply->reg_state.status, cbd->data);

	g_free(reply);

	return;
error:

	/*
	 * For some modems DATA_REGISTRATION_STATE will return an error until we
	 * are registered in the voice network.
	 */
	if (old_status == -1 && message->error == RIL_E_GENERIC_FAILURE)
		g_timeout_add(GET_STATUS_TIMER_MS, ril_get_status_retry, gprs);

	if (cb)
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb, void *data)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data, gprs);

	DBG("");

	if (g_ril_send(gd->ril, RIL_REQUEST_DATA_REGISTRATION_STATE, NULL,
			ril_data_reg_cb, cbd, g_free) == 0) {
		ofono_error("%s: send "
				"RIL_REQUEST_DATA_REGISTRATION_STATE failed",
				__func__);
		g_free(cbd);

		if (cb != NULL)
			CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void drop_data_call_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	if (message->error == RIL_E_SUCCESS)
		g_ril_print_response_no_args(gd->ril, message);
	else
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));

	if (--(gd->pending_deact_req) == 0)
		ril_gprs_registration_status(gprs, NULL, NULL);
}

static int drop_data_call(struct ofono_gprs *gprs, int cid)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct req_deactivate_data_call request;
	struct parcel rilp;
	struct ofono_error error;

	request.cid = cid;
	request.reason = RIL_DEACTIVATE_DATA_CALL_NO_REASON;

	g_ril_request_deactivate_data_call(gd->ril, &request, &rilp, &error);

	if (g_ril_send(gd->ril, RIL_REQUEST_DEACTIVATE_DATA_CALL,
			&rilp, drop_data_call_cb, gprs, NULL) == 0) {
		ofono_error("%s: send failed", __func__);
		return -1;
	}

	return 0;
}

static void get_active_data_calls_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ril_data_call_list *call_list = NULL;
	GSList *iterator;
	struct ril_data_call *call;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		goto end;
	}

	/* reply can be NULL when there are no existing data calls */
	call_list = g_ril_unsol_parse_data_call_list(gd->ril, message);
	if (call_list == NULL)
		goto end;

	/*
	 * We disconnect from previous calls here, which might be needed
	 * because of a previous ofono abort, as some rild implementations do
	 * not disconnect the calls even after the ril socket is closed.
	 */
	for (iterator = call_list->calls; iterator; iterator = iterator->next) {
		call = iterator->data;
		DBG("Standing data call with cid %d", call->cid);
		if (drop_data_call(gprs, call->cid) == 0)
			++(gd->pending_deact_req);
	}

	g_ril_unsol_free_data_call_list(call_list);

end:
	if (gd->pending_deact_req == 0)
		ril_gprs_registration_status(gprs, NULL, NULL);
}

static void get_active_data_calls(struct ofono_gprs *gprs)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	if (g_ril_send(gd->ril, RIL_REQUEST_DATA_CALL_LIST, NULL,
			get_active_data_calls_cb, gprs, NULL) == 0)
		ofono_error("%s: send failed", __func__);
}

void ril_gprs_start(GRil *ril, struct ofono_gprs *gprs,
			struct ril_gprs_data *gd)
{
	gd->ril = g_ril_clone(ril);
	gd->ofono_attached = FALSE;
	gd->max_cids = 0;
	gd->rild_status = -1;
	gd->tech = RADIO_TECH_UNKNOWN;
	/* AOSP RILD tracks data network state together with voice */
	gd->state_changed_unsol =
		RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED;

	ofono_gprs_set_data(gprs, gd);

	get_active_data_calls(gprs);
}

int ril_gprs_probe(struct ofono_gprs *gprs, unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct ril_gprs_data *gd;

	gd = g_try_new0(struct ril_gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	ril_gprs_start(ril, gprs, gd);

	return 0;
}

void ril_gprs_remove(struct ofono_gprs *gprs)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

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
