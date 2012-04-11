/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <glib.h>
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "common.h"

#include "hfpmodem.h"
#include "slc.h"

#define POLL_CLCC_INTERVAL 2000
#define POLL_CLCC_DELAY 50
#define CLIP_TIMEOUT 500

static const char *none_prefix[] = { NULL };
static const char *clcc_prefix[] = { "+CLCC:", NULL };

struct voicecall_data {
	GAtChat *chat;
	GSList *calls;
	unsigned int ag_features;
	unsigned int ag_mpty_features;
	unsigned char cind_pos[HFP_INDICATOR_LAST];
	int cind_val[HFP_INDICATOR_LAST];
	unsigned int local_release;
	unsigned int clcc_source;
	unsigned int clip_source;
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
	int affected_types;
};

static gboolean poll_clcc(gpointer user_data);

static GSList *find_dialing(GSList *calls)
{
	GSList *c;

	c = g_slist_find_custom(calls, GINT_TO_POINTER(CALL_STATUS_DIALING),
				at_util_call_compare_by_status);

	if (c == NULL)
		c = g_slist_find_custom(calls,
					GINT_TO_POINTER(CALL_STATUS_ALERTING),
					at_util_call_compare_by_status);

	return c;
}

static struct ofono_call *create_call(struct ofono_voicecall *vc, int type,
					int direction, int status,
					const char *num, int num_type, int clip)
{
	struct voicecall_data *d = ofono_voicecall_get_data(vc);
	struct ofono_call *call;

	/* Generate a call structure for the waiting call */
	call = g_try_new(struct ofono_call, 1);
	if (call == NULL)
		return NULL;

	ofono_call_init(call);

	call->id = ofono_voicecall_get_next_callid(vc);
	call->type = type;
	call->direction = direction;
	call->status = status;

	if (clip != 2) {
		strncpy(call->phone_number.number, num,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = num_type;
	}

	d->calls = g_slist_insert_sorted(d->calls, call, at_util_call_compare);

	call->clip_validity = clip;

	return call;
}

static void release_call(struct ofono_voicecall *vc, struct ofono_call *call)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	enum ofono_disconnect_reason reason;

	if (call == NULL)
		return;

	if (vd->local_release & (1 << call->id))
		reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
	else
		reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

	ofono_voicecall_disconnected(vc, call->id, reason, NULL);
	vd->local_release &= ~(1 << call->id);

	g_free(call);
}

static void release_all_calls(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *l;
	struct ofono_call *call;

	for (l = vd->calls; l; l = l->next) {
		call = l->data;

		release_call(vc, call);
	}

	g_slist_free(vd->calls);
	vd->calls = NULL;
}

static void release_with_status(struct ofono_voicecall *vc, int status)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *p = NULL;
	GSList *c = vd->calls;
	GSList *t;
	struct ofono_call *call;

	while (c) {
		call = c->data;

		if (call->status != status) {
			p = c;
			c = c->next;
			continue;
		}

		release_call(vc, call);

		if (p)
			p->next = c->next;
		else
			vd->calls = c->next;

		t = c;
		c = c->next;
		g_slist_free_1(t);
	}
}

static void clcc_poll_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *calls;
	GSList *n, *o;
	struct ofono_call *nc, *oc;
	unsigned int num_active = 0;
	unsigned int num_held = 0;

	if (!ok)
		return;

	calls = at_util_parse_clcc(result);

	n = calls;
	o = vd->calls;

	while (n || o) {
		nc = n ? n->data : NULL;
		oc = o ? o->data : NULL;

		if (nc && (nc->status == CALL_STATUS_ACTIVE))
			num_active++;

		if (nc && (nc->status == CALL_STATUS_HELD))
			num_held++;

		if (oc && (nc == NULL || (nc->id > oc->id))) {
			enum ofono_disconnect_reason reason;

			if (vd->local_release & (1 << oc->id))
				reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
			else
				reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

			if (!oc->type)
				ofono_voicecall_disconnected(vc, oc->id,
								reason, NULL);

			vd->local_release &= ~(1 << oc->id);

			o = o->next;
		} else if (nc && (oc == NULL || (nc->id < oc->id))) {
			/* new call, signal it */
			if (nc->type == 0)
				ofono_voicecall_notify(vc, nc);

			n = n->next;
		} else {
			/* Always use the clip_validity from old call
			 * the only place this is truly told to us is
			 * in the CLIP notify, the rest are fudged
			 * anyway.  Useful when RING, CLIP is used,
			 * and we're forced to use CLCC and clip_validity
			 * is 1
			 */
			nc->clip_validity = oc->clip_validity;

			if (memcmp(nc, oc, sizeof(struct ofono_call)) &&
					!nc->type)
				ofono_voicecall_notify(vc, nc);

			n = n->next;
			o = o->next;
		}
	}

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	vd->calls = calls;

	/* If either active/held call is more than 1, we are in mpty calls.
	 * we won't get indicator update if any of them is released by CHLD=1x.
	 * So we have to poll it.
	 */
	if (num_active > 1 || num_held > 1)
		vd->clcc_source = g_timeout_add(POLL_CLCC_INTERVAL, poll_clcc,
							vc);
}

static gboolean poll_clcc(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix,
				clcc_poll_cb, vc, NULL);

	vd->clcc_source = 0;

	return FALSE;
}

static void generic_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct change_state_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok && req->affected_types) {
		GSList *l;
		struct ofono_call *call;

		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (req->affected_types & (1 << call->status))
				vd->local_release |= (1 << call->id);
		}
	}

	req->cb(&error, req->data);
}

static void atd_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_voicecall *vc = cbd->user;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_cb_t cb = cbd->cb;
	int type = 128;
	int validity = 2;
	struct ofono_error error;
	struct ofono_call *call;
	GSList *l;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto out;

	/* On a success, make sure to put all active calls on hold */
	for (l = vd->calls; l; l = l->next) {
		call = l->data;

		if (call->status != CALL_STATUS_ACTIVE)
			continue;

		call->status = CALL_STATUS_HELD;
		ofono_voicecall_notify(vc, call);
	}

	call = create_call(vc, 0, 0, CALL_STATUS_DIALING, NULL, type, validity);
	if (call == NULL) {
		ofono_error("Unable to allocate call, "
				"call tracking will fail!");
		return;
	}

out:
	cb(&error, cbd->data);
}

static void hfp_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[256];

	cbd->user = vc;
	if (ph->type == 145)
		snprintf(buf, sizeof(buf), "ATD+%s", ph->number);
	else
		snprintf(buf, sizeof(buf), "ATD%s", ph->number);

	strcat(buf, ";");

	if (g_at_chat_send(vd->chat, buf, none_prefix,
				atd_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_template(const char *cmd, struct ofono_voicecall *vc,
			GAtResultFunc result_cb, unsigned int affected_types,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_try_new0(struct change_state_req, 1);

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	if (g_at_chat_send(vd->chat, cmd, none_prefix,
				result_cb, req, g_free) > 0)
		return;

error:
	g_free(req);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	hfp_template("ATA", vc, generic_cb, 0, cb, data);
}

static void hfp_hangup(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* Hangup current active call */
	hfp_template("AT+CHUP", vc, generic_cb, 0x1, cb, data);
}

static void hfp_hold_all_active(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->ag_mpty_features & AG_CHLD_2) {
		hfp_template("AT+CHLD=2", vc, generic_cb, 0, cb, data);
		return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_release_all_held(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	unsigned int held_status = 1 << CALL_STATUS_HELD;

	if (vd->ag_mpty_features & AG_CHLD_0) {
		hfp_template("AT+CHLD=0", vc, generic_cb, held_status,
				cb, data);
		return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_set_udub(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	unsigned int incoming_or_waiting =
		(1 << CALL_STATUS_INCOMING) | (1 << CALL_STATUS_WAITING);

	if (vd->ag_mpty_features & AG_CHLD_0) {
		hfp_template("AT+CHLD=0", vc, generic_cb, incoming_or_waiting,
				cb, data);
		return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->ag_mpty_features & AG_CHLD_1) {
		hfp_template("AT+CHLD=1", vc, generic_cb, 0x1, cb, data);
		return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void release_id_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct release_id_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok)
		vd->local_release |= (1 << req->id);

	req->cb(&error, req->data);
}

static void hfp_release_specific(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct release_id_req *req = NULL;
	char buf[32];

	if (!(vd->ag_mpty_features & AG_CHLD_1x))
		goto error;

	req = g_try_new0(struct release_id_req, 1);

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->id = id;

	snprintf(buf, sizeof(buf), "AT+CHLD=1%d", id);

	if (g_at_chat_send(vd->chat, buf, none_prefix,
				release_id_cb, req, g_free) > 0)
		return;

error:
	g_free(req);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_private_chat(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	char buf[32];

	if (vd->ag_mpty_features & AG_CHLD_2x) {
		snprintf(buf, sizeof(buf), "AT+CHLD=2%d", id);

		hfp_template(buf, vc, generic_cb, 0, cb, data);

		return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_create_multiparty(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->ag_mpty_features & AG_CHLD_3) {
		hfp_template("AT+CHLD=3", vc, generic_cb, 0, cb, data);

		return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	/* Transfer can puts held & active calls together and disconnects
	 * from both.  However, some networks support transferring of
	 * dialing/ringing calls as well.
	 */
	unsigned int transfer = 0x1 | 0x2 | 0x4 | 0x8;

	if (vd->ag_mpty_features & AG_CHLD_4) {
		hfp_template("AT+CHLD=4", vc, generic_cb, transfer, cb, data);

		return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_try_new0(struct change_state_req, 1);
	char *buf;
	int s;

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = 0;

	/* strlen("AT+VTS=) = 7 + NULL */
	buf = g_try_new(char, strlen(dtmf) + 8);
	if (buf == NULL)
		goto error;

	sprintf(buf, "AT+VTS=%s", dtmf);

	s = g_at_chat_send(vd->chat, buf, none_prefix,
				generic_cb, req, g_free);

	g_free(buf);

	if (s > 0)
		return;

error:
	g_free(req);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void no_carrier_notify(GAtResult *result, gpointer user_data)
{
	DBG("");
}

static void ccwa_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int num_type, validity;
	struct ofono_call *call;

	/* CCWA can repeat, ignore if we already have an waiting call */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_WAITING),
				at_util_call_compare_by_status))
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CCWA:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &num_type))
		return;

	if (strlen(num) > 0)
		validity = 0;
	else
		validity = 2;

	DBG("ccwa_notify: %s %d %d", num, num_type, validity);

	call = create_call(vc, 0, 1, CALL_STATUS_WAITING, num, num_type,
				validity);

	if (call == NULL) {
		ofono_error("malloc call struct failed.  "
				"Call management is fubar");
		return;
	}

	ofono_voicecall_notify(vc, call);
}

static gboolean clip_timeout(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *l;
	struct ofono_call *call;

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status);

	if (l == NULL)
		return FALSE;

	call = l->data;

	ofono_voicecall_notify(vc, call);

	vd->clip_source = 0;

	return FALSE;
}

static void ring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_call *call;
	GSList *waiting;

	/* RING can repeat, ignore if we already have an incoming call */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status))
		return;

	waiting = g_slist_find_custom(vd->calls,
					GINT_TO_POINTER(CALL_STATUS_WAITING),
					at_util_call_compare_by_status);

	/* If we started receiving RINGS but have a waiting call, most
	 * likely all other calls were dropped and we just didn't get
	 * notified yet, drop all other calls and update the status to
	 * incoming
	 */
	if (waiting) {
		DBG("Triggering waiting -> incoming cleanup code");

		vd->calls = g_slist_remove_link(vd->calls, waiting);
		release_all_calls(vc);
		vd->calls = waiting;

		call = waiting->data;
		call->status = CALL_STATUS_INCOMING;
		ofono_voicecall_notify(vc, call);

		return;
	}

	/* Generate an incoming call of voice type */
	call = create_call(vc, 0, 1, CALL_STATUS_INCOMING, NULL, 128, 2);

	if (call == NULL)
		ofono_error("Couldn't create call, call management is fubar!");

	/* We don't know the number must wait for CLIP to arrive before
	 * announcing the call. If timeout, we notify the call as it is.
	 */
	vd->clip_source = g_timeout_add(CLIP_TIMEOUT, clip_timeout, vc);
}

static void clip_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int type, validity;
	GSList *l;
	struct ofono_call *call;

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status);

	if (l == NULL) {
		ofono_error("CLIP for unknown call");
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLIP:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &type))
		return;

	if (strlen(num) > 0)
		validity = 0;
	else
		validity = 2;

	/* Skip subaddr, satype, alpha and validity */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	DBG("clip_notify: %s %d %d", num, type, validity);

	call = l->data;

	strncpy(call->phone_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = type;
	call->clip_validity = validity;

	ofono_voicecall_notify(vc, call);

	if (vd->clip_source) {
		g_source_remove(vd->clip_source);
		vd->clip_source = 0;
	}
}

static void ciev_call_notify(struct ofono_voicecall *vc,
				unsigned int value)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_call *call;

	switch (value) {
	case 0:
		/* If call goes to 0, then we have no held or active calls
		 * in the system.  The waiting calls are promoted to incoming
		 * calls, dialing calls are kept.  This also handles the
		 * situation when dialing and waiting calls exist
		 */
		release_with_status(vc, CALL_STATUS_HELD);
		release_with_status(vc, CALL_STATUS_ACTIVE);

		/* Promote waiting to incoming if it is the last call */
		if (vd->calls && vd->calls->next == NULL) {
			call = vd->calls->data;

			if (call->status == CALL_STATUS_WAITING) {
				call->status = CALL_STATUS_INCOMING;
				ofono_voicecall_notify(vc, call);
			}
		}

		break;

	case 1:
	{
		GSList *l;

		/* In this case either dialing/alerting or the incoming call
		 * is promoted to active
		 */
		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (call->status == CALL_STATUS_DIALING ||
					call->status == CALL_STATUS_ALERTING ||
					call->status == CALL_STATUS_INCOMING) {
				call->status = CALL_STATUS_ACTIVE;
				ofono_voicecall_notify(vc, call);
			}
		}

		break;
	}

	default:
		break;
	}

	vd->cind_val[HFP_INDICATOR_CALL] = value;
}

static void ciev_callsetup_notify(struct ofono_voicecall *vc,
					unsigned int value)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	unsigned int ciev_call = vd->cind_val[HFP_INDICATOR_CALL];
	unsigned int ciev_callheld = vd->cind_val[HFP_INDICATOR_CALLHELD];
	GSList *dialing;
	GSList *waiting;

	dialing = find_dialing(vd->calls);

	waiting = g_slist_find_custom(vd->calls,
					GINT_TO_POINTER(CALL_STATUS_WAITING),
					at_util_call_compare_by_status);

	/* This is a truly bizarre case not covered at all by the specification
	 * (yes, they are complete idiots).  Here we assume the other side is
	 * semi sane and will send callsetup updates in case the dialing call
	 * connects or the call waiting drops.  In which case we must poll
	 */
	if (waiting && dialing) {
		g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix,
				clcc_poll_cb, vc, NULL);
		goto out;
	}

	switch (value) {
	case 0:
		/* call=0 and callsetup=1: reject an incoming call
		 * call=0 and callsetup=2,3: interrupt an outgoing call
		 */
		if (ciev_call == 0) {
			release_all_calls(vc);
			goto out;
		}

		/* If call=1 and no call is waiting or dialing, the call is
		 * active and we moved it to active state when call=1 arrived
		 */
		if (waiting == NULL && dialing == NULL)
			goto out;

		/*
		 * If call=1, in the waiting case we have to poll, since we
		 * have no idea whether a waiting call gave up or we accepted
		 * using release+accept or hold+accept
		 *
		 * If call=1, in the dialing + held case we have to poll as
		 * well, we have no idea whether the call connected, or released
		 */
		if (waiting == NULL && ciev_callheld == 0) {
			struct ofono_call *call = dialing->data;

			/* We assume that the implementation follows closely
			 * the sequence of events in Figure 4.21.  That is
			 * call=1 arrives first, then callsetup=0
			 */

			call->status = CALL_STATUS_ACTIVE;
			ofono_voicecall_notify(vc, call);
		} else {
			g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix,
					clcc_poll_cb, vc, NULL);
		}

		break;

	case 1:
		/* Handled in RING/CCWA */
		break;

	case 2:
		/* two cases of outgoing call: dial from HF or AG.
		 * from HF: query and sync the phone number.
		 * from AG: query and create call.
		 */
		g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix,
				clcc_poll_cb, vc, NULL);
		break;

	case 3:
	{
		GSList *o = g_slist_find_custom(vd->calls,
					GINT_TO_POINTER(CALL_STATUS_DIALING),
					at_util_call_compare_by_status);

		if (o) {
			struct ofono_call *call = o->data;

			call->status = CALL_STATUS_ALERTING;
			ofono_voicecall_notify(vc, call);
		}

		break;
	}

	default:
		break;
	}

out:
	vd->cind_val[HFP_INDICATOR_CALLSETUP] = value;
}

static void ciev_callheld_notify(struct ofono_voicecall *vc,
					unsigned int value)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *l;
	struct ofono_call *call;
	unsigned int callheld = vd->cind_val[HFP_INDICATOR_CALLHELD];

	switch (value) {
	case 0:
		/* We have to poll here, we have no idea whether the call was
		 * dropped using CHLD=0 or simply retrieved, or the two calls
		 * were merged
		 */
		g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix,
				clcc_poll_cb, vc, NULL);
		break;

	case 1:
		if (vd->clcc_source) {
			g_source_remove(vd->clcc_source);
			vd->clcc_source = 0;
		}

		/* We have to poll here, we have no idea whether the call was
		 * accepted by CHLD=1 or swapped by CHLD=2 or one call was
		 * chosed for private chat by CHLD=2x
		 */
		g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix,
				clcc_poll_cb, vc, NULL);
		break;
	case 2:
		if (callheld == 0) {
			for (l = vd->calls; l; l = l->next) {
				call = l->data;

				if (call->status != CALL_STATUS_ACTIVE)
					continue;

				call->status = CALL_STATUS_HELD;
				ofono_voicecall_notify(vc, call);
			}
		} else if (callheld == 1) {
			if (vd->clcc_source)
				g_source_remove(vd->clcc_source);

			/* We have to schedule a poll here, we have no idea
			 * whether active call was dropped by remote or if this
			 * is an intermediate state during call swap
			 */
			vd->clcc_source = g_timeout_add(POLL_CLCC_DELAY,
							poll_clcc, vc);
		}
	}

	vd->cind_val[HFP_INDICATOR_CALLHELD] = value;
}

static void ciev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int index;
	int value;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIEV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	if (index == vd->cind_pos[HFP_INDICATOR_CALL])
		ciev_call_notify(vc, value);
	else if (index == vd->cind_pos[HFP_INDICATOR_CALLSETUP])
		ciev_callsetup_notify(vc, value);
	else if (index == vd->cind_pos[HFP_INDICATOR_CALLHELD])
		ciev_callheld_notify(vc, value);
}

static void hfp_clcc_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *l;

	if (!ok)
		return;

	vd->calls = at_util_parse_clcc(result);

	for (l = vd->calls; l; l = l->next)
		ofono_voicecall_notify(vc, l->data);
}

static void hfp_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	DBG("hfp_voicecall_init: registering to notifications");

	g_at_chat_register(vd->chat, "RING", ring_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CLIP:", clip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CIEV:", ciev_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CCWA:", ccwa_notify, FALSE, vc, NULL);

	g_at_chat_register(vd->chat, "NO CARRIER",
				no_carrier_notify, FALSE, vc, NULL);

	ofono_voicecall_register(vc);

	/* Populate the call list */
	g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix, hfp_clcc_cb, vc, NULL);
}

static int hfp_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				gpointer user_data)
{
	struct hfp_slc_info *info = user_data;
	struct voicecall_data *vd;

	vd = g_new0(struct voicecall_data, 1);

	vd->chat = g_at_chat_clone(info->chat);
	vd->ag_features = info->ag_features;
	vd->ag_mpty_features = info->ag_mpty_features;

	memcpy(vd->cind_pos, info->cind_pos, HFP_INDICATOR_LAST);
	memcpy(vd->cind_val, info->cind_val, HFP_INDICATOR_LAST);

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "AT+CLIP=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CCWA=1", NULL,
				hfp_voicecall_initialized, vc, NULL);
	return 0;
}

static void hfp_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->clcc_source)
		g_source_remove(vd->clcc_source);

	if (vd->clip_source)
		g_source_remove(vd->clip_source);

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "hfpmodem",
	.probe			= hfp_voicecall_probe,
	.remove			= hfp_voicecall_remove,
	.dial			= hfp_dial,
	.answer			= hfp_answer,
	.hangup_active		= hfp_hangup,
	.hold_all_active	= hfp_hold_all_active,
	.release_all_held	= hfp_release_all_held,
	.set_udub		= hfp_set_udub,
	.release_all_active	= hfp_release_all_active,
	.release_specific	= hfp_release_specific,
	.private_chat		= hfp_private_chat,
	.create_multiparty	= hfp_create_multiparty,
	.transfer		= hfp_transfer,
	.deflect		= NULL,
	.swap_without_accept	= NULL,
	.send_tones		= hfp_send_dtmf
};

void hfp_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void hfp_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
