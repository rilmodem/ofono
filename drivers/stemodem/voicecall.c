/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010 ST-Ericsson AB.
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

#include "gatchat.h"
#include "gatresult.h"
#include "common.h"

#include "stemodem.h"

enum call_status_ste {
	STE_CALL_STATUS_IDLE =		0,
	STE_CALL_STATUS_CALLING =	1,
	STE_CALL_STATUS_CONNECTING =	2,
	STE_CALL_STATUS_ACTIVE =	3,
	STE_CALL_STATUS_HOLD =		4,
	STE_CALL_STATUS_WAITING =	5,
	STE_CALL_STATUS_ALERTING =	6,
	STE_CALL_STATUS_BUSY =		7,
	STE_CALL_STATUS_RELEASED =	8,
};

static const char *none_prefix[] = { NULL };

struct voicecall_data {
	GSList *calls;
	unsigned int local_release;
	GAtChat *chat;
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

/* Translate from the ECAV-based STE-status to CLCC based status */
static int call_status_ste_to_ofono(enum call_status_ste status)
{
	switch (status) {
	case STE_CALL_STATUS_IDLE:
	case STE_CALL_STATUS_RELEASED:
		return CALL_STATUS_DISCONNECTED;
	case STE_CALL_STATUS_CALLING:
		return CALL_STATUS_DIALING;
	case STE_CALL_STATUS_CONNECTING:
		return CALL_STATUS_ALERTING;
	case STE_CALL_STATUS_ACTIVE:
		return CALL_STATUS_ACTIVE;
	case STE_CALL_STATUS_HOLD:
		return CALL_STATUS_HELD;
	case STE_CALL_STATUS_WAITING:
		return CALL_STATUS_WAITING;
	case STE_CALL_STATUS_ALERTING:
		return CALL_STATUS_INCOMING;
	case STE_CALL_STATUS_BUSY:
		return CALL_STATUS_DISCONNECTED;
	}

	return CALL_STATUS_DISCONNECTED;
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

	call->type = type;
	call->direction = direction;
	call->status = status;

	if (clip != CLIP_VALIDITY_NOT_AVAILABLE) {
		strncpy(call->phone_number.number, num,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = num_type;
	}

	call->clip_validity = clip;

	d->calls = g_slist_insert_sorted(d->calls, call, at_util_call_compare);

	return call;
}

static void ste_generic_cb(gboolean ok, GAtResult *result, gpointer user_data)
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

static void release_id_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct release_id_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok)
		vd->local_release = 1 << req->id;

	req->cb(&error, req->data);
}

static void atd_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_error error;
	ofono_voicecall_cb_t cb = cbd->cb;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void ste_dial(struct ofono_voicecall *vc,
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

	switch (clir) {
	case OFONO_CLIR_OPTION_DEFAULT:
		break;
	case OFONO_CLIR_OPTION_INVOCATION:
		strcat(buf, "I");
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		strcat(buf, "i");
		break;
	}

	strcat(buf, ";");

	if (g_at_chat_send(vd->chat, buf, none_prefix,
				atd_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ste_template(const char *cmd, struct ofono_voicecall *vc,
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

static void ste_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ste_template("ATA", vc, ste_generic_cb, 0, cb, data);
}

static void ste_hangup(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	unsigned int active_dial_alert_or_incoming =
			(1 << CALL_STATUS_ACTIVE) |
			(1 << CALL_STATUS_DIALING) |
			(1 << CALL_STATUS_ALERTING) |
			(1 << CALL_STATUS_INCOMING);

	ste_template("AT+CHUP", vc, ste_generic_cb,
			active_dial_alert_or_incoming, cb, data);
}

static void ste_hold_all_active(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ste_template("AT+CHLD=2", vc, ste_generic_cb, 0, cb, data);
}

static void ste_release_all_held(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	unsigned int held = 1 << CALL_STATUS_HELD;

	ste_template("AT+CHLD=0", vc, ste_generic_cb, held, cb, data);
}

static void ste_set_udub(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	unsigned int incoming_or_waiting =
			(1 << CALL_STATUS_INCOMING) | (1 << CALL_STATUS_WAITING);

	ste_template("AT+CHLD=0", vc, ste_generic_cb, incoming_or_waiting,
			cb, data);
}

static void ste_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	unsigned int active = 1 << CALL_STATUS_ACTIVE;

	ste_template("AT+CHLD=1", vc, ste_generic_cb, active, cb, data);
}

static void ste_release_specific(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct release_id_req *req = g_try_new0(struct release_id_req, 1);
	char buf[32];

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

static void ste_private_chat(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "AT+CHLD=2%d", id);
	ste_template(buf, vc, ste_generic_cb, 0, cb, data);
}

static void ste_create_multiparty(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	ste_template("AT+CHLD=3", vc, ste_generic_cb, 0, cb, data);
}

static void ste_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* Held & Active */
	unsigned int transfer = 0x1 | 0x2;

	/* Transfer can puts held & active calls together and disconnects
	 * from both.  However, some networks support transferring of
	 * dialing/ringing calls as well.
	 */
	transfer |= 0x4 | 0x8;

	ste_template("AT+CHLD=4", vc, ste_generic_cb, transfer, cb, data);
}

static void ste_deflect(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			ofono_voicecall_cb_t cb, void *data)
{
	char buf[128];
	unsigned int incoming_or_waiting =
		(1 << CALL_STATUS_INCOMING) | (1 << CALL_STATUS_WAITING);

	snprintf(buf, sizeof(buf), "AT+CTFR=\"%s\",%d", ph->number, ph->type);
	ste_template(buf, vc, ste_generic_cb, incoming_or_waiting, cb, data);
}

static void vts_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void ste_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	int s;
	char *buf;

	/* strlen("AT+VTS=) = 7 + NULL */
	buf = g_try_new(char, strlen(dtmf) + 8);
	if (buf == NULL)
		goto error;

	sprintf(buf, "AT+VTS=%s", dtmf);

	s = g_at_chat_send(vd->chat, buf, none_prefix,
				vts_cb, cbd, g_free);

	g_free(buf);

	if (s > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ecav_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int id;
	int status;
	int call_type;
	int num_type;
	struct ofono_call *new_call;
	struct ofono_call *existing_call = NULL;
	GSList *l;

	/* Parse ECAV */
	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "*ECAV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &id))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_type))
		return;

	if (call_type != BEARER_CLASS_VOICE)
		return;

	/* Skip process id and exit cause */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	status = call_status_ste_to_ofono(status);

	if (status == CALL_STATUS_DIALING ||
			status == CALL_STATUS_WAITING ||
			status == CALL_STATUS_INCOMING) {
		/*
		 * If caller uses hidden id, the number and
		 * number type might not be present. Don't
		 * look for type if number is not present.
		 */
		if (!g_at_result_iter_next_string(&iter, &num)) {
			num = "";
			num_type = 128;
		} else if (!g_at_result_iter_next_number(&iter, &num_type))
			return;
	}

	/*
	 * Handle the call according to the status.
	 * If it doesn't exists we make a new one
	 */
	l = g_slist_find_custom(vd->calls, GUINT_TO_POINTER(id),
				at_util_call_compare_by_id);

	if (l)
		existing_call = l->data;

	if (l == NULL && status != CALL_STATUS_DIALING &&
				status != CALL_STATUS_WAITING &&
				status != CALL_STATUS_INCOMING) {
		ofono_error("ECAV notification for unknown call."
				" id: %d, status: %d", id, status);
		return;
	}

	switch (status) {
	case CALL_STATUS_DISCONNECTED: {
		enum ofono_disconnect_reason reason;

		existing_call->status = status;

		if (vd->local_release & (1 << existing_call->id))
			reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
		else
			reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

		ofono_voicecall_disconnected(vc, existing_call->id,
						reason, NULL);

		vd->local_release &= ~(1 << existing_call->id);
		vd->calls = g_slist_remove(vd->calls, l->data);
		g_free(existing_call);
		break;
	}

	case CALL_STATUS_DIALING:
	case CALL_STATUS_WAITING:
	case CALL_STATUS_INCOMING: {
		int clip_validity;
		int direction;

		if (status == CALL_STATUS_DIALING)
			direction = CALL_DIRECTION_MOBILE_ORIGINATED;
		else
			direction = CALL_DIRECTION_MOBILE_TERMINATED;

		if (strlen(num) > 0)
			clip_validity = CLIP_VALIDITY_VALID;
		else
			clip_validity = CLIP_VALIDITY_NOT_AVAILABLE;

		new_call = create_call(vc, call_type, direction, status,
					num, num_type, clip_validity);
		if (new_call == NULL) {
			ofono_error("Unable to malloc. "
					"Call management is fubar");
			return;
		}

		new_call->id = id;
		ofono_voicecall_notify(vc, new_call);
		break;
	}

	case CALL_STATUS_ALERTING:
	case CALL_STATUS_ACTIVE:
	case CALL_STATUS_HELD:
		existing_call->status = status;
		ofono_voicecall_notify(vc, existing_call);
		break;
	}
}

static void ste_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (!ok) {
		ofono_error("*ECAV not enabled. "
				"Do not have proper call handling");
		ofono_voicecall_remove(vc);
		return;
	}

	g_at_chat_register(vd->chat, "*ECAV:", ecav_notify, FALSE, vc, NULL);
	ofono_voicecall_register(vc);
}

static int ste_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->chat = g_at_chat_clone(chat);

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "AT*ECAM=2", none_prefix,
			ste_voicecall_initialized, vc, NULL);

	return 0;
}

static void ste_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "stemodem",
	.probe			= ste_voicecall_probe,
	.remove			= ste_voicecall_remove,
	.dial			= ste_dial,
	.answer			= ste_answer,
	.hangup_active		= ste_hangup,
	.hold_all_active	= ste_hold_all_active,
	.release_all_held	= ste_release_all_held,
	.set_udub		= ste_set_udub,
	.release_all_active	= ste_release_all_active,
	.release_specific	= ste_release_specific,
	.private_chat		= ste_private_chat,
	.create_multiparty	= ste_create_multiparty,
	.transfer		= ste_transfer,
	.deflect		= ste_deflect,
	.swap_without_accept	= NULL,
	.send_tones		= ste_send_dtmf
};

void ste_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void ste_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
