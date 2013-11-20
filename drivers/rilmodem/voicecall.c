/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013 Canonical Ltd.
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
#include <ofono/voicecall.h>

#include "gril.h"
#include "grilutil.h"
#include "grilrequest.h"
#include "grilreply.h"
#include "grilunsol.h"

#include "common.h"
#include "rilmodem.h"

/* Amount of ms we wait between CLCC calls */
#define POLL_CLCC_INTERVAL 300

#define FLAG_NEED_CLIP 1

#define MAX_DTMF_BUFFER 32

/* To use with change_state_req::affected_types */
#define AFFECTED_STATES_ALL 0x3F

struct voicecall_data {
	GSList *calls;
	/* Call local hangup indicator, one bit per call (1 << call_id) */
	unsigned int local_release;
	unsigned int clcc_source;
	GRil *ril;
	unsigned int vendor;
	unsigned char flags;
	ofono_voicecall_cb_t cb;
	void *data;
	gchar *tone_queue;
	gboolean tone_pending;
};

struct release_id_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	int id;
};

struct change_state_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	/* Call states affected by a local release (1 << enum call_status) */
	int affected_types;
};

struct lastcause_req {
	struct ofono_voicecall *vc;
	int id;
};

static void send_one_dtmf(struct voicecall_data *vd);
static void clear_dtmf_queue(struct voicecall_data *vd);

static void lastcause_cb(struct ril_msg *message, gpointer user_data)
{
	struct lastcause_req *reqdata = user_data;
	struct ofono_voicecall *vc = reqdata->vc;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	enum ofono_disconnect_reason reason;

	reason = g_ril_reply_parse_call_fail_cause(vd->ril, message);

	DBG("Call %d ended with reason %d", reqdata->id, reason);

	ofono_voicecall_disconnected(vc, reqdata->id, reason, NULL);
}

static void clcc_poll_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *calls;
	GSList *n, *o;
	struct ofono_call *nc, *oc;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("We are polling CLCC and received an error");
		ofono_error("All bets are off for call management");
		return;
	}

	calls = g_ril_reply_parse_get_calls(vd->ril, message);

	n = calls;
	o = vd->calls;

	while (n || o) {
		nc = n ? n->data : NULL;
		oc = o ? o->data : NULL;

		/* TODO: Add comments explaining call id handling */
		if (oc && (nc == NULL || (nc->id > oc->id))) {
			if (vd->local_release & (1 << oc->id)) {
				ofono_voicecall_disconnected(vc, oc->id,
					OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
					NULL);
			} else {
				/* Get disconnect cause before calling core */
				struct lastcause_req *reqdata =
					g_try_new0(struct lastcause_req, 1);
				if (reqdata) {
					int req;
					req = RIL_REQUEST_LAST_CALL_FAIL_CAUSE;
					reqdata->vc = user_data;
					reqdata->id = oc->id;
					g_ril_send(vd->ril, req,
							NULL, 0, lastcause_cb,
							reqdata, g_free);
				}
			}

			clear_dtmf_queue(vd);

			o = o->next;
		} else if (nc && (oc == NULL || (nc->id < oc->id))) {
			/* new call, signal it */
			if (nc->type) {
				ofono_voicecall_notify(vc, nc);
				if (vd->cb) {
					struct ofono_error error;
					ofono_voicecall_cb_t cb = vd->cb;
					decode_ril_error(&error, "OK");
					cb(&error, vd->data);
					vd->cb = NULL;
					vd->data = NULL;
				}

			}

			n = n->next;
		} else {
			/*
			 * Always use the clip_validity from old call
			 * the only place this is truly told to us is
			 * in the CLIP notify, the rest are fudged
			 * anyway.  Useful when RING, CLIP is used,
			 * and we're forced to use CLCC and clip_validity
			 * is 1
			 */
			if (oc->clip_validity == 1)
				nc->clip_validity = oc->clip_validity;

			nc->cnap_validity = oc->cnap_validity;

			/*
			 * CDIP doesn't arrive as part of CLCC, always
			 * re-use from the old call
			 */
			memcpy(&nc->called_number, &oc->called_number,
					sizeof(oc->called_number));

			/*
			 * If the CLIP is not provided and the CLIP never
			 * arrives, or RING is used, then signal the call
			 * here
			 */
			if (nc->status == CALL_STATUS_INCOMING &&
					(vd->flags & FLAG_NEED_CLIP)) {
				if (nc->type)
					ofono_voicecall_notify(vc, nc);

				vd->flags &= ~FLAG_NEED_CLIP;
			} else if (memcmp(nc, oc, sizeof(*nc)) && nc->type)
				ofono_voicecall_notify(vc, nc);

			n = n->next;
			o = o->next;
		}
	}

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	vd->calls = calls;
	vd->local_release = 0;
}

static gboolean poll_clcc(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_GET_CURRENT_CALLS;
	int ret;

	ret = g_ril_send(vd->ril, request, NULL,
			0, clcc_poll_cb, vc, NULL);

	g_ril_print_request_no_args(vd->ril, ret, request);

	vd->clcc_source = 0;

	return FALSE;
}

static void generic_cb(struct ril_msg *message, gpointer user_data)
{
	struct change_state_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;
	int request = RIL_REQUEST_GET_CURRENT_CALLS;
	int ret;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	g_ril_print_response_no_args(vd->ril, message);

	if (req->affected_types) {
		GSList *l;
		struct ofono_call *call;

		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (req->affected_types & (1 << call->status))
				vd->local_release |= (1 << call->id);
		}
	}

out:
	ret = g_ril_send(vd->ril, request, NULL,
				0, clcc_poll_cb, req->vc, NULL);

	g_ril_print_request_no_args(vd->ril, ret, request);

	/* We have to callback after we schedule a poll if required */
	if (req->cb)
		req->cb(&error, req->data);
}

static int ril_template(const guint rreq, struct ofono_voicecall *vc,
			GRilResponseFunc func, unsigned int affected_types,
			gpointer pdata, const gsize psize,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_try_new0(struct change_state_req, 1);
	int ret;

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	ret = g_ril_send(vd->ril, rreq, pdata, psize, func, req, g_free);
	if (ret > 0)
		return ret;
error:
	g_free(req);

	if (cb)
		CALLBACK_WITH_FAILURE(cb, data);

	return 0;
}

static void rild_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_voicecall *vc = cbd->user;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;
	struct ofono_call *call;
	GSList *l;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	g_ril_print_response_no_args(vd->ril, message);

	/* On a success, make sure to put all active calls on hold */
	for (l = vd->calls; l; l = l->next) {
		call = l->data;

		if (call->status != CALL_STATUS_ACTIVE)
			continue;

		call->status = CALL_STATUS_HELD;
		ofono_voicecall_notify(vc, call);
	}

	/* CLCC will update the oFono call list with proper ids  */
	if (!vd->clcc_source)
		vd->clcc_source = g_timeout_add(POLL_CLCC_INTERVAL,
						poll_clcc, vc);

	/* we cannot answer just yet since we don't know the call id */
	vd->cb = cb;
	vd->data = cbd->data;

	return;

out:
	cb(&error, cbd->data);
}

static void ril_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data, vc);
	struct parcel rilp;
	int request = RIL_REQUEST_DIAL;
	int ret;

	g_ril_request_dial(vd->ril, ph, clir, &rilp);

	/* Send request to RIL */
	ret = g_ril_send(vd->ril, request, rilp.data,
				rilp.size, rild_cb, cbd, g_free);

	g_ril_print_request(vd->ril, ret, request);

	parcel_free(&rilp);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_hangup_all(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;
	struct ofono_error error;
	struct ofono_call *call;
	GSList *l;
	int request = RIL_REQUEST_HANGUP;
	int ret;

	for (l = vd->calls; l; l = l->next) {
		call = l->data;
		/* TODO: Hangup just the active ones once we have call
		 * state tracking (otherwise it can't handle ringing) */
		g_ril_request_hangup(vd->ril, call->id, &rilp);

		/* Send request to RIL */
		ret = ril_template(request, vc, generic_cb, AFFECTED_STATES_ALL,
					rilp.data, rilp.size, NULL, NULL);

		g_ril_print_request(vd->ril, ret, request);

		parcel_free(&rilp);
	}

	/* TODO: Deal in case of an error at hungup */
	decode_ril_error(&error, "OK");
	cb(&error, data);
}

static void ril_hangup_specific(struct ofono_voicecall *vc,
		int id, ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;
	int request = RIL_REQUEST_HANGUP;
	int ret;

	DBG("Hanging up call with id %d", id);

	g_ril_request_hangup(vd->ril, id, &rilp);

	/* Send request to RIL */
	ret = ril_template(request, vc, generic_cb, AFFECTED_STATES_ALL,
				rilp.data, rilp.size, cb, data);

	g_ril_print_request(vd->ril, ret, request);

	parcel_free(&rilp);
}

static void ril_call_state_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;

	if (message->req != RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED)
		goto error;

	/* Just need to request the call list again */
	poll_clcc(vc);

	return;

error:
	ofono_error("Unable to notify about call state changes");
}

static void ril_ss_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct unsol_supp_svc_notif *unsol;

	unsol = g_ril_unsol_parse_supp_svc_notif(vd->ril,  message);
	if (unsol == NULL) {
		ofono_error("%s: Parsing error", __func__);
		return;
	}

	DBG("RIL data: MT/MO: %i, code: %i, index: %i",
		unsol->notif_type, unsol->code, unsol->index);

	/* 0 stands for MO intermediate, 1 for MT unsolicited */
	/* TODO How do we know the affected call? Refresh call list? */
	if (unsol->notif_type == 1)
		ofono_voicecall_ssn_mt_notify(
			vc, 0, unsol->code, unsol->index, &unsol->number);
	else
		ofono_voicecall_ssn_mo_notify(vc, 0, unsol->code, unsol->index);

	g_ril_unsol_free_supp_svc_notif(unsol);
}

static void ril_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_ANSWER;
	int ret;

	DBG("Answering current call");

	/* Send request to RIL */
	ret = ril_template(request, vc, generic_cb, 0,
				NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static void ril_send_dtmf_cb(struct ril_msg *message, gpointer user_data)
{
	struct voicecall_data *vd = user_data;

	if (message->error == RIL_E_SUCCESS) {
		/* Remove sent DTMF character from queue */
		gchar *tmp_tone_queue = g_strdup(vd->tone_queue + 1);
		int remaining = strlen(tmp_tone_queue);

		memcpy(vd->tone_queue, tmp_tone_queue, remaining);
		vd->tone_queue[remaining] = '\0';
		g_free(tmp_tone_queue);

		vd->tone_pending = FALSE;

		if (remaining > 0)
			send_one_dtmf(vd);
	} else {
		DBG("error=%d", message->error);
		clear_dtmf_queue(vd);
	}
}

static void send_one_dtmf(struct voicecall_data *vd)
{
	struct parcel rilp;
	int request = RIL_REQUEST_DTMF;
	int ret;

	if (vd->tone_pending == TRUE)
		return; /* RIL request pending */

	if (strlen(vd->tone_queue) == 0)
		return; /* nothing to send */

	g_ril_request_dtmf(vd->ril, vd->tone_queue[0], &rilp);

	ret = g_ril_send(vd->ril, request, rilp.data,
				rilp.size, ril_send_dtmf_cb, vd, NULL);

	g_ril_print_request(vd->ril, ret, request);
	parcel_free(&rilp);

	vd->tone_pending = TRUE;
}

static void ril_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
		ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_error error;

	DBG("Queue '%s'", dtmf);

	/*
	 * Queue any incoming DTMF (up to MAX_DTMF_BUFFER characters),
	 * send them to RIL one-by-one, immediately call back
	 * core with no error
	 */
	g_strlcat(vd->tone_queue, dtmf, MAX_DTMF_BUFFER);
	send_one_dtmf(vd);

	/* We don't really care about errors here */
	decode_ril_error(&error, "OK");
	cb(&error, data);
}

static void clear_dtmf_queue(struct voicecall_data *vd)
{
	g_free(vd->tone_queue);
	vd->tone_queue = g_strnfill(MAX_DTMF_BUFFER + 1, '\0');
	vd->tone_pending = FALSE;
}

static void ril_create_multiparty(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_CONFERENCE;
	int ret;

	ret = ril_template(request, vc, generic_cb, 0, NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static void ril_private_chat(struct ofono_voicecall *vc, int id,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;
	int request = RIL_REQUEST_SEPARATE_CONNECTION;
	int ret;

	g_ril_request_separate_conn(vd->ril, id, &rilp);

	/* Send request to RIL */
	ret = ril_template(request, vc, generic_cb, 0, rilp.data, rilp.size,
				cb, data);

	g_ril_print_request(vd->ril, ret, request);

	parcel_free(&rilp);
}

static void ril_swap_without_accept(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE;
	int ret;

	ret = ril_template(request, vc, generic_cb, 0, NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static void ril_hold_all_active(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE;
	int ret;

	ret = ril_template(request, vc, generic_cb, 0, NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static void ril_release_all_held(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND;
	int ret;

	ret = ril_template(request, vc, generic_cb, 0, NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static void ril_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND;
	int ret;

	ret = ril_template(request, vc, generic_cb, 0, NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static void ril_set_udub(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND;
	int ret;

	ret = ril_template(request, vc, generic_cb, 0, NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static gboolean enable_supp_svc(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION;
	int ret;
	struct parcel rilp;

	g_ril_request_set_supp_svc_notif(vd->ril, &rilp);

	ret = g_ril_send(vd->ril, request, rilp.data,
				rilp.size, NULL, vc, NULL);

	g_ril_print_request(vd->ril, ret, request);

	parcel_free(&rilp);

	/* Makes this a single shot */
	return FALSE;
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_register(vc);

	/* Initialize call list */
	poll_clcc(vc);

	/* Unsol when call state changes */
	g_ril_register(vd->ril, RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
			ril_call_state_notify, vc);

	/* Unsol when call set on hold */
	g_ril_register(vd->ril, RIL_UNSOL_SUPP_SVC_NOTIFICATION,
			ril_ss_notify, vc);

	/* request supplementary service notifications*/
	enable_supp_svc(vc);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->ril = g_ril_clone(ril);
	vd->vendor = vendor;
	vd->cb = NULL;
	vd->data = NULL;

	clear_dtmf_queue(vd);

	ofono_voicecall_set_data(vc, vd);

	/*
	 * ofono_voicecall_register() needs to be called after
	 * the driver has been set in ofono_voicecall_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle event instead.
	 */
	g_idle_add(ril_delayed_register, vc);

	return 0;
}

static void ril_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->clcc_source)
		g_source_remove(vd->clcc_source);

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	ofono_voicecall_set_data(vc, NULL);

	g_ril_unref(vd->ril);
	g_free(vd->tone_queue);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_voicecall_probe,
	.remove			= ril_voicecall_remove,
	.dial			= ril_dial,
	.answer			= ril_answer,
	.hangup_all		= ril_hangup_all,
	.release_specific	= ril_hangup_specific,
	.send_tones		= ril_send_dtmf,
	.create_multiparty	= ril_create_multiparty,
	.private_chat		= ril_private_chat,
	.swap_without_accept	= ril_swap_without_accept,
	.hold_all_active	= ril_hold_all_active,
	.release_all_held	= ril_release_all_held,
	.set_udub		= ril_set_udub,
	.release_all_active	= ril_release_all_active,
};

void ril_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void ril_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
