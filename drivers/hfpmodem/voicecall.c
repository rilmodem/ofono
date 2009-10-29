/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>
#include <common.h>
#include "gatchat.h"
#include "gatresult.h"

#include "hfpmodem.h"

#define AG_CHLD_0 0x01
#define AG_CHLD_1 0x02
#define AG_CHLD_1x 0x04
#define AG_CHLD_2 0x08
#define AG_CHLD_2x 0x10
#define AG_CHLD_3 0x20
#define AG_CHLD_4 0x40

static const char *none_prefix[] = { NULL };
static const char *chld_prefix[] = { "+CHLD:", NULL };

struct voicecall_data {
	GAtChat *chat;
	GSList *calls;
	struct ofono_call *call;
	gboolean mpty_call;
	unsigned int ag_features;
	unsigned int ag_mpty_features;
	unsigned char cind_pos[HFP_INDICATOR_LAST];
	int cind_val[HFP_INDICATOR_LAST];
	unsigned int id_list;
	unsigned int local_release;
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

static struct ofono_call *create_call(struct voicecall_data *d, int type,
					int direction, int status,
					const char *num, int num_type, int clip)
{
	struct ofono_call *call;

	/* Generate a call structure for the waiting call */
	call = g_try_new0(struct ofono_call, 1);

	if (!call)
		return NULL;

	call->id = at_util_alloc_next_id(&d->id_list);
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

	if (d->call)
		d->mpty_call = TRUE;

	d->call = call;

	return call;
}

static void generic_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct change_state_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	dump_response("generic_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (ok && req->affected_types) {
		GSList *l;
		struct ofono_call *call;

		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (req->affected_types & (0x1 << call->status))
				vd->local_release |=
					(0x1 << call->id);
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
	GAtResultIter iter;
	int type = 128;
	int validity = 2;
	struct ofono_error error;
	struct ofono_call *call;

	dump_response("atd_cb", ok, result);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto out;

	g_at_result_iter_init(&iter, result);

	call = create_call(vd, 0, 0, CALL_STATUS_DIALING, NULL, type, validity);

	if (!call) {
		ofono_error("Unable to allocate call, "
				"call tracking will fail!");
		return;
	}

out:
	cb(&error, cbd->data);
}

static void hfp_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, enum ofono_cug_option cug,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[256];

	if (!cbd)
		goto error;

	cbd->user = vc;
	if (ph->type == 145)
		sprintf(buf, "ATD+%s", ph->number);
	else
		sprintf(buf, "ATD%s", ph->number);

	if ((clir != OFONO_CLIR_OPTION_DEFAULT) ||
			(cug != OFONO_CUG_OPTION_DEFAULT))
		goto error;

	strcat(buf, ";");

	if (g_at_chat_send(vd->chat, buf, none_prefix,
				atd_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_template(const char *cmd, struct ofono_voicecall *vc,
			GAtResultFunc result_cb, unsigned int affected_types,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_try_new0(struct change_state_req, 1);

	if (!req)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	if (g_at_chat_send(vd->chat, cmd, none_prefix,
				result_cb, req, g_free) > 0)
		return;

error:
	if (req)
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
	/* Hangup all calls */
	hfp_template("AT+CHUP", vc, generic_cb, 0x3f, cb, data);
}

static void ring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_call *call;

	dump_response("ring_notify", TRUE, result);

	/* RING can repeat, ignore if we already have an incoming call */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status))
		return;

	/* ignore if we already have a waiting call */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_WAITING),
				at_util_call_compare_by_status))
		return;

	/* Generate an incoming call of voice type */
	call = create_call(vd, 0, 1, CALL_STATUS_INCOMING, NULL, 128, 2);

	if (!call)
		ofono_error("Couldn't create call, call management is fubar!");
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

	dump_response("clip_notify", TRUE, result);

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

	ofono_debug("clip_notify: %s %d %d", num, type, validity);

	call = l->data;

	strncpy(call->phone_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = type;
	call->clip_validity = validity;

	ofono_voicecall_notify(vc, call);
}

static void release_call(struct ofono_voicecall *vc, struct ofono_call *call)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	enum ofono_disconnect_reason reason;

	if (call == NULL)
		return;

	if (vd->local_release & (0x1 << call->id))
		reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
	else
		reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

	ofono_voicecall_disconnected(vc, call->id, reason, NULL);
	at_util_release_id(&vd->id_list, call->id);

	if (vd->mpty_call == FALSE)
		vd->local_release = 0;

	vd->calls = g_slist_remove(vd->calls, call);

	if (call == vd->call)
		vd->call = NULL;

	g_free(call);
}

static void ciev_call_notify(struct ofono_voicecall *vc,
				struct ofono_call *call,
				unsigned int value)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	unsigned int call_pos = vd->cind_pos[HFP_INDICATOR_CALL];

	if (vd->mpty_call == FALSE) {
		switch (value) {
		case 0:
			release_call(vc, call);
			break;
		case 1:
			call->status = CALL_STATUS_ACTIVE;
			ofono_voicecall_notify(vc, call);
			break;
		default:
			break;
		}
	}

	vd->cind_val[call_pos] = value;
}

static void ciev_callsetup_notify(struct ofono_voicecall *vc,
					struct ofono_call *call,
					unsigned int value)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	unsigned int callsetup_pos = vd->cind_pos[HFP_INDICATOR_CALLSETUP];
	unsigned int call_pos = vd->cind_pos[HFP_INDICATOR_CALL];

	if (vd->mpty_call == FALSE) {
		switch (value) {
		case 0:
			/* call=0 and callsetup=1: reject an incoming call
			 * call=0 and callsetup=2,3: interrupt an outgoing call
			 */
			if ((vd->cind_val[call_pos] == 0) &&
					(vd->cind_val[callsetup_pos] > 0))
				release_call(vc, call);
			break;
		case 1:
		case 2:
			break;
		case 3:
			call->status = CALL_STATUS_ALERTING;
			ofono_voicecall_notify(vc, call);
		default:
			break;
		}
	}

	vd->cind_val[callsetup_pos] = value;
}

static void ciev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_call *call = vd->call;
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
		ciev_call_notify(vc, call, value);
	else if (index == vd->cind_pos[HFP_INDICATOR_CALLSETUP])
		ciev_callsetup_notify(vc, call, value);
}

static void chld_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct voicecall_data *vd = user_data;
	unsigned int ag_mpty_feature = 0;
	GAtResultIter iter;
	const char *str;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CHLD:"))
		return;

	if (!g_at_result_iter_open_list(&iter))
		return;

	while (g_at_result_iter_next_unquoted_string(&iter, &str)) {
		if (!strcmp(str, "0"))
			ag_mpty_feature |= AG_CHLD_0;
		else if (!strcmp(str, "1"))
			ag_mpty_feature |= AG_CHLD_1;
		else if (!strcmp(str, "1x"))
			ag_mpty_feature |= AG_CHLD_1x;
		else if (!strcmp(str, "2"))
			ag_mpty_feature |= AG_CHLD_2;
		else if (!strcmp(str, "2x"))
			ag_mpty_feature |= AG_CHLD_2x;
		else if (!strcmp(str, "3"))
			ag_mpty_feature |= AG_CHLD_3;
		else if (!strcmp(str, "4"))
			ag_mpty_feature |= AG_CHLD_4;
	}

	if (!g_at_result_iter_close_list(&iter))
		return;

	vd->ag_mpty_features = ag_mpty_feature;
}

static void hfp_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	ofono_debug("hfp_voicecall_init: registering to notifications");

	g_at_chat_register(vd->chat, "RING", ring_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CLIP:", clip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CIEV:", ciev_notify, FALSE, vc, NULL);

	ofono_voicecall_register(vc);
}

static int hfp_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				gpointer user_data)
{
	struct hfp_data *data = user_data;
	struct voicecall_data *vd;

	vd = g_new0(struct voicecall_data, 1);

	vd->chat = data->chat;
	vd->ag_features = data->ag_features;
	vd->call = NULL;
	vd->mpty_call = FALSE;

	memcpy(vd->cind_pos, data->cind_pos, HFP_INDICATOR_LAST);
	memcpy(vd->cind_val, data->cind_val, HFP_INDICATOR_LAST);

	if (vd->ag_features & AG_FEATURE_3WAY)
		g_at_chat_send(vd->chat, "AT+CHLD=?", chld_prefix,
			chld_cb, vd, NULL);

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "AT+CLIP=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CCWA=1", NULL,
				hfp_voicecall_initialized, vc, NULL);
	return 0;
}

static void hfp_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "hfpmodem",
	.probe			= hfp_voicecall_probe,
	.remove			= hfp_voicecall_remove,
	.dial			= hfp_dial,
	.answer			= hfp_answer,
	.hangup			= hfp_hangup,
	.list_calls		= NULL,
	.hold_all_active	= NULL,
	.release_all_held	= NULL,
	.set_udub		= NULL,
	.release_all_active	= NULL,
	.release_specific	= NULL,
	.private_chat		= NULL,
	.create_multiparty	= NULL,
	.transfer		= NULL,
	.deflect		= NULL,
	.swap_without_accept	= NULL,
	.send_tones		= NULL
};

void hfp_voicecall_init()
{
	ofono_voicecall_driver_register(&driver);
}

void hfp_voicecall_exit()
{
	ofono_voicecall_driver_unregister(&driver);
}
