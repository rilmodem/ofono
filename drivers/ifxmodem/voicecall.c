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
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"
#include "ifxmodem.h"

static const char *none_prefix[] = { NULL };
static const char *xlema_prefix[] = { "+XLEMA:", NULL };

struct voicecall_data {
	GSList *calls;
	unsigned int local_release;
	GAtChat *chat;
	char **en_list;
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

static int class_to_call_type(int cls)
{
	switch (cls) {
	case 1:
		return 0;
	case 4:
		return 2;
	case 8:
		return 9;
	default:
		return 1;
	}
}

static struct ofono_call *create_call(struct ofono_voicecall *vc, int type,
					int direction, int status,
					const char *num, int num_type,
					int clip, int id)
{
	struct voicecall_data *d = ofono_voicecall_get_data(vc);
	struct ofono_call *call;

	/* Generate a call structure for the waiting call */
	call = g_try_new(struct ofono_call, 1);
	if (call == NULL)
		return NULL;

	ofono_call_init(call);

	call->id = id;
	call->type = type;
	call->direction = direction;
	call->status = status;

	if (clip != 2) {
		strncpy(call->phone_number.number, num,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = num_type;
	}

	call->clip_validity = clip;

	d->calls = g_slist_insert_sorted(d->calls, call, at_util_call_compare);

	return call;
}

static void xcallstat_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	int id;
	int status;
	GSList *l;
	struct ofono_call *new_call;
	struct ofono_call *existing_call = NULL;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+XCALLSTAT:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &id) == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &status) == FALSE)
		return;

	l = g_slist_find_custom(vd->calls, GINT_TO_POINTER(id),
				at_util_call_compare_by_id);

	if (l == NULL && status != CALL_STATUS_DIALING &&
				status != CALL_STATUS_INCOMING &&
				status != CALL_STATUS_WAITING) {
		ofono_error("Received XCALLSTAT for an untracked"
				" call, this indicates a bug!");
		return;
	}

	if (l)
		existing_call = l->data;

	switch (status) {
	case CALL_STATUS_DISCONNECTED:
	{
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
	case CALL_STATUS_INCOMING:
	{
		int direction;

		/* Handle the following situation:
		 * Active Call + Waiting Call. Active Call is Released.
		 * The Waiting call becomes Incoming. In this case, no
		 * need to create a new call. Call status change will be
		 * triggered from clip_notify.
		 */
		if (existing_call) {
			existing_call->status = status;
			return;
		}

		if (status == CALL_STATUS_DIALING)
			direction = CALL_DIRECTION_MOBILE_ORIGINATED;
		else
			direction = CALL_DIRECTION_MOBILE_TERMINATED;

		new_call = create_call(vc, 0, direction, status,
					NULL, 128,
					CLIP_VALIDITY_NOT_AVAILABLE, id);
		if (new_call == NULL) {
			ofono_error("Unable to malloc. "
					"Call management is fubar");
			return;
		}

		new_call->id = id;
		break;
	}
	case CALL_STATUS_ALERTING:
	case CALL_STATUS_ACTIVE:
	case CALL_STATUS_HELD:
	default:
		/* For connected status, simply reset back to active */
		if (status == 7)
			status = CALL_STATUS_ACTIVE;

		existing_call->status = status;
		ofono_voicecall_notify(vc, existing_call);
		break;
	}
}

static void xem_notify(GAtResult *result, gpointer user_data)
{
	//struct ofono_voicecall *vc = user_data;
	//struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	int state;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+XEM:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &state) == FALSE)
		return;

	DBG("state %d", state);

	switch (state) {
	case 0:
		ofono_info("Emergency call is finished");
		break;
	case 1:
		ofono_info("Emergency call is entered");
		break;
	}
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

static void release_id_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct release_id_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok)
		vd->local_release |= 1 << req->id;

	req->cb(&error, req->data);
}

static void atd_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	/* Let oFono core will generate a call with the dialed number
	 * inside its dial callback.
	 */
	cb(&error, cbd->data);
}

static void ifx_dial(struct ofono_voicecall *vc,
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
	case OFONO_CLIR_OPTION_INVOCATION:
		strcat(buf, "I");
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		strcat(buf, "i");
		break;
	default:
		break;
	}

	strcat(buf, ";");

	if (g_at_chat_send(vd->chat, buf, none_prefix,
				atd_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ifx_template(const char *cmd, struct ofono_voicecall *vc,
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

static void ifx_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ifx_template("ATA", vc, generic_cb, 0, cb, data);
}

static void ifx_ath(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* Hangup active + held call, but not waiting */
	ifx_template("ATH", vc, generic_cb, 0x1f, cb, data);
}

static void ifx_chup(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* Hangup active + but not held or waiting */
	ifx_template("AT+CHUP", vc, generic_cb, 0x1d, cb, data);
}

static void ifx_hold_all_active(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ifx_template("AT+CHLD=2", vc, generic_cb, 0, cb, data);
}

static void ifx_release_all_held(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	unsigned int held_status = 1 << CALL_STATUS_HELD;
	ifx_template("AT+CHLD=0", vc, generic_cb, held_status, cb, data);
}

static void ifx_set_udub(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	unsigned int incoming_or_waiting =
		(1 << CALL_STATUS_INCOMING) | (1 << CALL_STATUS_WAITING);

	ifx_template("AT+CHLD=0", vc, generic_cb, incoming_or_waiting,
			cb, data);
}

static void ifx_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	ifx_template("AT+CHLD=1", vc, generic_cb, 0x1, cb, data);
}

static void ifx_release_specific(struct ofono_voicecall *vc, int id,
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

static void ifx_private_chat(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "AT+CHLD=2%d", id);
	ifx_template(buf, vc, generic_cb, 0, cb, data);
}

static void ifx_create_multiparty(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	ifx_template("AT+CHLD=3", vc, generic_cb, 0, cb, data);
}

static void ifx_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* Held & Active */
	unsigned int transfer = 0x1 | 0x2;

	/* Transfer can puts held & active calls together and disconnects
	 * from both.  However, some networks support transferring of
	 * dialing/ringing calls as well.
	 */
	transfer |= 0x4 | 0x8;

	ifx_template("AT+CHLD=4", vc, generic_cb, transfer, cb, data);
}

static void ifx_deflect(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			ofono_voicecall_cb_t cb, void *data)
{
	char buf[128];
	unsigned int incoming_or_waiting =
		(1 << CALL_STATUS_INCOMING) | (1 << CALL_STATUS_WAITING);

	snprintf(buf, sizeof(buf), "AT+CTFR=%s,%d", ph->number, ph->type);
	ifx_template(buf, vc, generic_cb, incoming_or_waiting, cb, data);
}

static void ifx_swap_without_accept(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ifx_template("AT+CHLD=6", vc, generic_cb, 0, cb, data);
}

static void vts_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void ifx_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	int len = strlen(dtmf);
	int s;
	int i;
	char *buf;

	/* strlen("+VTS=T\;") = 7 + initial AT + null */
	buf = g_try_new(char, len * 7 + 3);
	if (buf == NULL)
		goto error;

	s = sprintf(buf, "AT+VTS=%c", dtmf[0]);

	for (i = 1; i < len; i++)
		s += sprintf(buf + s, ";+VTS=%c", dtmf[i]);

	s = g_at_chat_send(vd->chat, buf, none_prefix,
				vts_cb, cbd, g_free);

	g_free(buf);

	if (s > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void cring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *line;
	GSList *l;
	struct ofono_call *call;

	/* Handle the following situation:
	 * Active Call + Waiting Call.  Active Call is Released.  The Waiting
	 * call becomes Incoming and CRING indications are signaled.
	 * Sometimes these arrive before the actual state change notification.
	 * If this happens, simply ignore the CRING when a waiting call
	 * exists (cannot have waiting + incoming in GSM)
	 */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_WAITING),
				at_util_call_compare_by_status))
		return;

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status);
	if (l == NULL) {
		ofono_error("CRING received before XCALLSTAT!!!");
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRING:"))
		return;

	line = g_at_result_iter_raw_line(&iter);
	if (line == NULL)
		return;

	call = l->data;

	/* Ignore everything that is not voice for now */
	if (!strcasecmp(line, "VOICE"))
		call->type = 0;
	else
		call->type = 9;

	/* Assume the CLIP always arrives, and we signal the call there */
	DBG("cring_notify");
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

	/* Skip subaddr, satype and alpha */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	/* If we have CLI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("clip_notify: %s %d %d", num, type, validity);

	call = l->data;

	strncpy(call->phone_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = type;
	call->clip_validity = validity;

	if (call->type == 0)
		ofono_voicecall_notify(vc, call);
}

static void cnap_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *name;
	int validity;
	GSList *l;
	struct ofono_call *call;

	/*
	 * Currently, its not clear which URC will contain the
	 * calling party name for the waiting call.
	 */
	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status);
	if (l == NULL) {
		ofono_error("CNAP for unknown call");
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CNAP:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &name))
		return;

	if (strlen(name) > 0)
		validity = CNAP_VALIDITY_VALID;
	else
		validity = CNAP_VALIDITY_NOT_AVAILABLE;

	/* If we have CNI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("%s %d", name, validity);

	call = l->data;

	strncpy(call->name, name, OFONO_MAX_CALLER_NAME_LENGTH);
	call->name[OFONO_MAX_CALLER_NAME_LENGTH] = '\0';
	call->cnap_validity = validity;

	if (call->type == 0)
		ofono_voicecall_notify(vc, call);
}

static void ccwa_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int num_type, validity, cls;
	GSList *l;
	struct ofono_call *call;

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_WAITING),
				at_util_call_compare_by_status);
	if (l == NULL) {
		ofono_error("CCWA received before XCALLSTAT!!!");
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CCWA:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &num_type))
		return;

	if (!g_at_result_iter_next_number(&iter, &cls))
		return;

	/* Skip alpha field */
	g_at_result_iter_skip_next(&iter);

	if (strlen(num) > 0)
		validity = 0;
	else
		validity = 2;

	/* If we have CLI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("ccwa_notify: %s %d %d %d", num, num_type, cls, validity);

	call = l->data;

	call->type = class_to_call_type(cls);
	strncpy(call->phone_number.number, num, OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = num_type;
	call->clip_validity = validity;

	if (call->type == 0) /* Only notify voice calls */
		ofono_voicecall_notify(vc, call);
}

static void xcolp_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int type, call_id;
	GSList *l;
	struct ofono_call *call;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XCOLP:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &type))
		return;

	if (strlen(num) == 0) {
		ofono_error("XCOLP received with invalid number!!!");
		return;
	}

	DBG("xcolp_notify: %d %s %d", call_id, num, type);

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(call_id),
				at_util_call_compare_by_id);
	if (l == NULL) {
		ofono_error("XCOLP for unknown call");
		return;
	}

	call = l->data;

	strncpy(call->phone_number.number, num,	OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = type;
	call->clip_validity = CLIP_VALIDITY_VALID;

	ofono_voicecall_notify(vc, call);
}

static void xlema_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	int index, total_cnt;
	const char *number;
	int len;
	int count = (vd->en_list == NULL) ? 0 : g_strv_length(vd->en_list);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XLEMA:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		return;

	if (!g_at_result_iter_next_number(&iter, &total_cnt))
		return;

	if (!g_at_result_iter_next_string(&iter, &number))
		return;

	/* Skip category */
	if (g_at_result_iter_skip_next(&iter) == FALSE)
		goto done;

	/* Skip presence */
	if (g_at_result_iter_skip_next(&iter) == FALSE)
		goto done;

	/* If we succeed here, then the number is from NVM or NITZ */
	if (g_at_result_iter_skip_next(&iter) == FALSE)
		goto done;

	if (vd->en_list == NULL)
		vd->en_list = g_new0(char *, total_cnt + 1);

	len = strspn(number, "0123456789");
	vd->en_list[count] = g_strndup(number, len);

	if (number[len] != '\0')
		ofono_warn("Malformed emergency number: %.*s", len, number);

done:
	if (index != total_cnt)
		return;

	if (vd->en_list) {
		ofono_voicecall_en_list_notify(vc, vd->en_list);

		g_strfreev(vd->en_list);
		vd->en_list = NULL;
	}
}

static void xlema_read(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	int num = 0;
	int index, total_cnt;
	const char *number;
	int len;

	if (!ok) {
		DBG("Emergency number list read failed");
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+XLEMA:"))
		num += 1;

	vd->en_list = g_new0(char *, num + 1);

	num = 0;
	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+XLEMA:")) {
		if (!g_at_result_iter_next_number(&iter, &index))
			continue;

		if (!g_at_result_iter_next_number(&iter, &total_cnt))
			continue;

		if (!g_at_result_iter_next_string(&iter, &number))
			continue;

		len = strspn(number, "0123456789");
		vd->en_list[num++] = g_strndup(number, len);

		if (number[len] != '\0')
			ofono_warn("Malformed emergency number: %.*s",
							len, number);
	}

	ofono_voicecall_en_list_notify(vc, vd->en_list);

	g_strfreev(vd->en_list);
	vd->en_list = NULL;
}

static void cssi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	GAtResultIter iter;
	int code, index;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		index = 0;

	ofono_voicecall_ssn_mo_notify(vc, 0, code, index);
}

static void cssu_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	GAtResultIter iter;
	int code;
	int index;
	const char *num;
	struct ofono_phone_number ph;

	ph.number[0] = '\0';
	ph.type = 129;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSU:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code))
		return;

	if (!g_at_result_iter_next_number_default(&iter, -1, &index))
		goto out;

	if (!g_at_result_iter_next_string(&iter, &num))
		goto out;

	strncpy(ph.number, num, OFONO_MAX_PHONE_NUMBER_LENGTH);

	if (!g_at_result_iter_next_number(&iter, &ph.type))
		return;

out:
	ofono_voicecall_ssn_mt_notify(vc, 0, code, index, &ph);
}

static void ifx_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	DBG("voicecall_init: registering to notifications");

	g_at_chat_register(vd->chat, "+CRING:", cring_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CLIP:", clip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CNAP:", cnap_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CCWA:", ccwa_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+XEM:", xem_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+XCALLSTAT:", xcallstat_notify,
							FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+XCOLP:", xcolp_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+XLEMA:", xlema_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CSSI:", cssi_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CSSU:", cssu_notify, FALSE, vc, NULL);
	/* Enable emergency number list notification */
	g_at_chat_send(vd->chat, "AT+XLEMA=1", xlema_prefix, xlema_read, vc,
									NULL);

	ofono_voicecall_register(vc);
}

static int ifx_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->chat = g_at_chat_clone(chat);

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "AT+XCALLSTAT=1", none_prefix, NULL, NULL,
			NULL);
	g_at_chat_send(vd->chat, "AT+XEMC=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+XCOLP=1", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(vd->chat, "AT+CRC=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CLIP=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CNAP=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CSSN=1,1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CCWA=1", none_prefix,
				ifx_voicecall_initialized, vc, NULL);

	return 0;
}

static void ifx_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	g_strfreev(vd->en_list);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "ifxmodem",
	.probe			= ifx_voicecall_probe,
	.remove			= ifx_voicecall_remove,
	.dial			= ifx_dial,
	.answer			= ifx_answer,
	.hangup_all		= ifx_ath,
	.hangup_active		= ifx_chup,
	.hold_all_active	= ifx_hold_all_active,
	.release_all_held	= ifx_release_all_held,
	.set_udub		= ifx_set_udub,
	.release_all_active	= ifx_release_all_active,
	.release_specific	= ifx_release_specific,
	.private_chat		= ifx_private_chat,
	.create_multiparty	= ifx_create_multiparty,
	.transfer		= ifx_transfer,
	.deflect		= ifx_deflect,
	.swap_without_accept	= ifx_swap_without_accept,
	.send_tones		= ifx_send_dtmf
};

void ifx_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void ifx_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
