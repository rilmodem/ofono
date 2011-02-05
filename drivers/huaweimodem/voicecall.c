/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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
#include "huaweimodem.h"

static const char *none_prefix[] = { NULL };

struct voicecall_data {
	GAtChat *chat;
	GSList *calls;
};

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

	g_at_chat_send(d->chat, "AT^DDSETEX=2", none_prefix,
						NULL, NULL, NULL);

	return call;
}

static void huawei_generic_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void huawei_template(struct ofono_voicecall *vc, const char *cmd,
					ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(vd->chat, cmd, none_prefix,
				huawei_generic_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void huawei_dial(struct ofono_voicecall *vc,
				const struct ofono_phone_number *ph,
				enum ofono_clir_option clir,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[256];

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

	huawei_template(vc, buf, cb, data);
}

static void huawei_answer(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	huawei_template(vc, "ATA", cb, data);
}

static void huawei_hangup(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	/* Hangup active call */
	huawei_template(vc, "AT+CHUP", cb, data);
}

static void huawei_release_specific(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "AT+CHLD=1%d", id);
	huawei_template(vc, buf, cb, data);
}

static void cring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *line;
	int type;
	int id;

	/* CRING can repeat, ignore if we already have an incoming call */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status))
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRING:"))
		return;

	line = g_at_result_iter_raw_line(&iter);
	if (line == NULL)
		return;

	/* Ignore everything that is not voice for now */
	if (!strcasecmp(line, "VOICE"))
		type = 0;
	else
		type = 9;

	id = ofono_voicecall_get_next_callid(vc);

	/* Generate an incoming call */
	create_call(vc, type, 1, CALL_STATUS_INCOMING, NULL, 128, 2, id);

	/* Assume the CLIP always arrives, and we signal the call there */
	DBG("%d", type);
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

	DBG("%s %d %d", num, type, validity);

	call = l->data;

	strncpy(call->phone_number.number, num,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = type;
	call->clip_validity = validity;

	if (call->type == 0)
		ofono_voicecall_notify(vc, call);
}

static void ccwa_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	const char *num;
	int num_type, validity, cls;

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

	DBG("%s %d %d %d", num, num_type, cls, validity);
}

static void orig_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	GAtResultIter iter;
	gint call_id, call_type;
	struct ofono_call *call;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^ORIG:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_type))
		return;

	ofono_info("Call origin: id %d type %d", call_id, call_type);

	call = create_call(vc, call_type, 0, CALL_STATUS_DIALING, NULL, 128, 2,
			    call_id);
	if (call == NULL) {
		ofono_error("Unable to malloc, call tracking will fail!");
		return;
	}

	if (call->type == 0)
		ofono_voicecall_notify(vc, call);
}

static void conf_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	gint call_id;
	struct ofono_call *call;
	GSList *l;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CONF:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	ofono_info("Call setup: id %d", call_id);

	l = g_slist_find_custom(vd->calls, GINT_TO_POINTER(call_id),
				at_util_call_compare_by_id);
	if (l == NULL) {
		ofono_error("Received CONF for untracked call");
		return;
	}

	/* Set call to alerting */
	call = l->data;
	call->status = CALL_STATUS_ALERTING;

	if (call->type == 0)
		ofono_voicecall_notify(vc, call);
}

static void conn_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	gint call_id, call_type;
	struct ofono_call *call;
	GSList *l;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CONN:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_type))
		return;

	ofono_info("Call connect: id %d type %d", call_id, call_type);

	l = g_slist_find_custom(vd->calls, GINT_TO_POINTER(call_id),
				at_util_call_compare_by_id);
	if (l == NULL) {
		ofono_error("Received CONN for untracked call");
		return;
	}

	/* Set call to active */
	call = l->data;
	call->status = CALL_STATUS_ACTIVE;

	if (call->type == 0)
		ofono_voicecall_notify(vc, call);
}

static void cend_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	gint call_id, duration, end_status, cc_pause;
	struct ofono_call *call;
	GSList *l;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CEND:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &call_id))
		return;

	if (!g_at_result_iter_next_number(&iter, &duration))
		return;

	if (!g_at_result_iter_next_number(&iter, &end_status))
		return;

	/* parameter is not present on errors */
	g_at_result_iter_next_number(&iter, &cc_pause);

	ofono_info("Call end: id %d duration %ds status %d",
				call_id, duration, end_status);

	l = g_slist_find_custom(vd->calls, GINT_TO_POINTER(call_id),
				at_util_call_compare_by_id);
	if (l == NULL) {
		ofono_error("Received CEND for untracked call");
		return;
	}

	call = l->data;

	if (call->type == 0)
		ofono_voicecall_disconnected(vc, call->id,
				OFONO_DISCONNECT_REASON_UNKNOWN, NULL);

	vd->calls = g_slist_remove(vd->calls, call);
	g_free(call);
}

static void huawei_voicecall_initialized(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	DBG("registering to notifications");

	g_at_chat_register(vd->chat, "+CRING:", cring_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CLIP:", clip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CCWA:", ccwa_notify, FALSE, vc, NULL);

	g_at_chat_register(vd->chat, "^ORIG:", orig_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "^CONF:", conf_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "^CONN:", conn_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "^CEND:", cend_notify, FALSE, vc, NULL);

	ofono_voicecall_register(vc);
}

static int huawei_voicecall_probe(struct ofono_voicecall *vc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->chat = g_at_chat_clone(chat);

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "AT+CRC=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CLIP=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+COLP=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CCWA=1", none_prefix,
				huawei_voicecall_initialized, vc, NULL);

	return 0;
}

static void huawei_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "huaweimodem",
	.probe			= huawei_voicecall_probe,
	.remove			= huawei_voicecall_remove,
	.dial			= huawei_dial,
	.answer			= huawei_answer,
	.hangup_active		= huawei_hangup,
	.release_specific	= huawei_release_specific,
};

void huawei_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void huawei_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
