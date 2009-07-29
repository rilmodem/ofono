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
#include "driver.h"

#include "gatchat.h"
#include "gatresult.h"

#include "at.h"

/* Amount of ms we wait between CLCC calls */
#define POLL_CLCC_INTERVAL 500

 /* Amount of time we give for CLIP to arrive before we commence CLCC poll */
#define CLIP_INTERVAL 200

static const char *clcc_prefix[] = { "+CLCC:", NULL };
static const char *none_prefix[] = { NULL };

/* According to 27.007 COLP is an intermediate status for ATD */
static const char *atd_prefix[] = { "+COLP:", NULL };

struct voicecall_data {
	gboolean poll_clcc;
	GSList *calls;
	unsigned int id_list;
	unsigned int local_release;
	unsigned int clcc_source;
};

static gboolean poll_clcc(gpointer user_data);

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

static unsigned int alloc_next_id(struct voicecall_data *d)
{
	unsigned int i;

	for (i = 1; i < sizeof(d->id_list) * 8; i++) {
		if (d->id_list & (0x1 << i))
			continue;

		d->id_list |= (0x1 << i);
		return i;
	}

	return 0;
}

#if 0
static gboolean alloc_specific_id(struct voicecall_data *d, unsigned int id)
{
	if (id < 1 || id > sizeof(d->id_list))
		return FALSE;

	if (d->id_list & (0x1 << id))
		return FALSE;

	d->id_list |= (0x1 << id);

	return TRUE;
}
#endif

static void release_id(struct voicecall_data *d, unsigned int id)
{
	d->id_list &= ~(0x1 << id);
}

#if 0
static gint call_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	if (id < call->id)
		return -1;

	if (id > call->id)
		return 1;

	return 0;
}
#endif

static gint call_compare_by_status(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	int status = GPOINTER_TO_INT(b);

	if (status != call->status)
		return 1;

	return 0;
}

static gint call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

static struct ofono_call *create_call(struct voicecall_data *d, int type,
					int direction, int status,
					const char *num, int num_type, int clip)
{
	struct ofono_call *call;

	/* Generate a call structure for the waiting call */
	call = g_try_new0(struct ofono_call, 1);

	if (!call)
		return NULL;

	call->id = alloc_next_id(d);
	call->type = type;
	call->direction = direction;
	call->status = status;

	if (clip != 2) {
		strncpy(call->phone_number.number, num,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = num_type;
	}

	call->clip_validity = clip;

	d->calls = g_slist_insert_sorted(d->calls, call, call_compare);

	return call;
}

static GSList *parse_clcc(GAtResult *result)
{
	GAtResultIter iter;
	GSList *l = NULL;
	int id, dir, status, type;
	struct ofono_call *call;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CLCC:")) {
		const char *str = "";
		int number_type = 129;

		if (!g_at_result_iter_next_number(&iter, &id))
			continue;

		if (!g_at_result_iter_next_number(&iter, &dir))
			continue;

		if (!g_at_result_iter_next_number(&iter, &status))
			continue;

		if (!g_at_result_iter_next_number(&iter, &type))
			continue;

		if (!g_at_result_iter_skip_next(&iter))
			continue;

		if (g_at_result_iter_next_string(&iter, &str))
			g_at_result_iter_next_number(&iter, &number_type);

		call = g_try_new0(struct ofono_call, 1);

		if (!call)
			break;

		call->id = id;
		call->direction = dir;
		call->status = status;
		call->type = type;
		strncpy(call->phone_number.number, str,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = number_type;

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		l = g_slist_insert_sorted(l, call, call_compare);
	}

	return l;
}

static void clcc_poll_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);
	GSList *calls;
	GSList *n, *o;
	struct ofono_call *nc, *oc;
	gboolean poll_again = FALSE;

	dump_response("clcc_poll_cb", ok, result);

	if (!ok) {
		ofono_error("We are polling CLCC and CLCC resulted in an error");
		ofono_error("All bets are off for call management");
		return;
	}

	calls = parse_clcc(result);

	n = calls;
	o = at->voicecall->calls;

	while (n || o) {
		nc = n ? n->data : NULL;
		oc = o ? o->data : NULL;

		if (nc && nc->status >= 2 && nc->status <= 5)
			poll_again = TRUE;

		if (oc && (!nc || (nc->id > oc->id))) {
			enum ofono_disconnect_reason reason;

			if (at->voicecall->local_release & (0x1 << oc->id))
				reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
			else
				reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

			if (!oc->type)
				ofono_voicecall_disconnected(modem, oc->id,
								reason, NULL);

			release_id(at->voicecall, oc->id);

			o = o->next;
		} else if (nc && (!oc || (nc->id < oc->id))) {
			/* new call, signal it */
			if (nc->type == 0)
				ofono_voicecall_notify(modem, nc);

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

			if (memcmp(nc, oc, sizeof(struct ofono_call)) && !nc->type)
				ofono_voicecall_notify(modem, nc);

			n = n->next;
			o = o->next;
		}
	}

	g_slist_foreach(at->voicecall->calls, (GFunc) g_free, NULL);
	g_slist_free(at->voicecall->calls);

	at->voicecall->calls = calls;

	at->voicecall->local_release = 0;

	if (poll_again && at->voicecall->poll_clcc &&
		!at->voicecall->clcc_source)
		at->voicecall->clcc_source = g_timeout_add(POLL_CLCC_INTERVAL,
								poll_clcc,
								modem);
}

static gboolean poll_clcc(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);

	g_at_chat_send(at->parser, "AT+CLCC", clcc_prefix,
				clcc_poll_cb, modem, NULL);

	at->voicecall->clcc_source = 0;

	return FALSE;
}

static void generic_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct at_data *at = ofono_modem_get_userdata(cbd->modem);
	ofono_generic_cb_t cb = cbd->cb;
	unsigned int released_status = GPOINTER_TO_UINT(cbd->user);
	struct ofono_error error;

	dump_response("generic_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (ok && released_status) {
		GSList *l;
		struct ofono_call *call;

		for (l = at->voicecall->calls; l; l = l->next) {
			call = l->data;

			if (released_status & (0x1 << call->status))
				at->voicecall->local_release |=
					(0x1 << call->id);
		}
	}

	if (at->voicecall->poll_clcc)
		g_at_chat_send(at->parser, "AT+CLCC", clcc_prefix,
					clcc_poll_cb, cbd->modem, NULL);

	/* We have to callback after we schedule a poll if required */
	cb(&error, cbd->data);
}

static void release_id_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct at_data *at = ofono_modem_get_userdata(cbd->modem);
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("release_id_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (ok)
		at->voicecall->local_release = GPOINTER_TO_UINT(cbd->user);

	if (at->voicecall->poll_clcc)
		g_at_chat_send(at->parser, "AT+CLCC", clcc_prefix,
					clcc_poll_cb, cbd->modem, NULL);

	/* We have to callback after we schedule a poll if required */
	cb(&error, cbd->data);
}
static void atd_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct at_data *at = ofono_modem_get_userdata(cbd->modem);
	ofono_generic_cb_t cb = cbd->cb;
	GAtResultIter iter;
	const char *num;
	int type = 128;
	int validity = 2;
	struct ofono_error error;
	struct ofono_call *call;

	dump_response("atd_cb", ok, result);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto out;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+COLP:")) {
		g_at_result_iter_next_string(&iter, &num);
		g_at_result_iter_next_number(&iter, &type);

		if (strlen(num) > 0)
			validity = 0;
		else
			validity = 2;

		ofono_debug("colp_notify: %s %d %d", num, type, validity);
	}

	/* Generate a voice call that was just dialed, we guess the ID */
	call = create_call(at->voicecall, 0, 0, 2, num, type, validity);

	if (!call) {
		ofono_error("Unable to allocate call, call tracking will fail!");
		return;
	}

	/* Telephonyd will generate a call with the dialed number
	 * inside its dial callback.  Unless we got COLP information
	 * we do not need to communicate that a call is being
	 * dialed
	 */
	if (validity != 2)
		ofono_voicecall_notify(cbd->modem, call);

	if (at->voicecall->poll_clcc && !at->voicecall->clcc_source)
		at->voicecall->clcc_source = g_timeout_add(POLL_CLCC_INTERVAL,
								poll_clcc,
								cbd->modem);

out:
	cb(&error, cbd->data);
}

static void at_dial(struct ofono_modem *modem,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, enum ofono_cug_option cug,
			ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[256];

	if (!cbd)
		goto error;

	if (ph->type == 145)
		sprintf(buf, "ATD+%s", ph->number);
	else
		sprintf(buf, "ATD%s", ph->number);

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

	switch (cug) {
	case OFONO_CUG_OPTION_INVOCATION:
		strcat(buf, "G");
		break;
	default:
		break;
	}

	strcat(buf, ";");

	if (g_at_chat_send(at->parser, buf, atd_prefix,
				atd_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_template(const char *cmd, struct ofono_modem *modem,
			GAtResultFunc result_cb, unsigned int released_status,
			ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	cbd->user = GUINT_TO_POINTER(released_status);

	if (g_at_chat_send(at->parser, cmd, none_prefix,
				result_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_answer(struct ofono_modem *modem, ofono_generic_cb_t cb, void *data)
{
	at_template("ATA", modem, generic_cb, 0, cb, data);
}

static void at_hangup(struct ofono_modem *modem, ofono_generic_cb_t cb, void *data)
{
	/* Hangup all calls */
	at_template("AT+CHUP", modem, generic_cb, 0x3f, cb, data);
}

static void clcc_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_list_cb_t cb = cbd->cb;
	struct ofono_error error;
	GSList *calls = NULL;
	GSList *l;
	struct ofono_call *list;
	int num;

	dump_response("clcc_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, 0, NULL, cbd->data);
		goto out;
	}

	calls = parse_clcc(result);

	if (calls == NULL) {
		DECLARE_FAILURE(e);
		cb(&e, 0, NULL, cbd->data);
		goto out;
	}

	list = g_try_new0(struct ofono_call, g_slist_length(calls));

	if (!list) {
		DECLARE_FAILURE(e);
		cb(&e, 0, NULL, cbd->data);
		goto out;
	}

	for (num = 0, l = calls; l; l = l->next, num++)
		memcpy(&list[num], l->data, sizeof(struct ofono_call));

	cb(&error, num, list, cbd->data);

	g_free(list);

out:
	g_slist_foreach(calls, (GFunc) g_free, NULL);
	g_slist_free(calls);
}

static void at_list_calls(struct ofono_modem *modem, ofono_call_list_cb_t cb,
				void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CLCC", clcc_prefix,
				clcc_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, 0, NULL, data);
	}

}

static void at_hold_all_active(struct ofono_modem *modem, ofono_generic_cb_t cb,
				void *data)
{
	at_template("AT+CHLD=2", modem, generic_cb, 0, cb, data);
}

static void at_release_all_held(struct ofono_modem *modem, ofono_generic_cb_t cb,
				void *data)
{
	unsigned int held_status = 0x1 << 1;
	at_template("AT+CHLD=0", modem, generic_cb, held_status, cb, data);
}

static void at_set_udub(struct ofono_modem *modem, ofono_generic_cb_t cb, void *data)
{
	unsigned int incoming_or_waiting = (0x1 << 4) | (0x1 << 5);
	at_template("AT+CHLD=0", modem, generic_cb, incoming_or_waiting,
			cb, data);
}

static void at_release_all_active(struct ofono_modem *modem, ofono_generic_cb_t cb,
					void *data)
{
	at_template("AT+CHLD=1", modem, generic_cb, 0x1, cb, data);
}

static void at_release_specific(struct ofono_modem *modem, int id,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CHLD=1%d", id);
	cbd->user = GINT_TO_POINTER(id);

	if (g_at_chat_send(at->parser, buf, none_prefix,
				release_id_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_private_chat(struct ofono_modem *modem, int id,
				ofono_generic_cb_t cb, void *data)
{
	char buf[32];

	sprintf(buf, "AT+CHLD=2%d", id);
	at_template(buf, modem, generic_cb, 0, cb, data);
}

static void at_create_multiparty(struct ofono_modem *modem, ofono_generic_cb_t cb,
				void *data)
{
	at_template("AT+CHLD=3", modem, generic_cb, 0, cb, data);
}

static void at_transfer(struct ofono_modem *modem, ofono_generic_cb_t cb,
				void *data)
{
	/* Held & Active */
	unsigned int transfer = 0x1 | 0x2;

	/* Transfer can puts held & active calls together and disconnects
	 * from both.  However, some networks support transfering of
	 * dialing/ringing calls as well.
	 */
	transfer |= 0x4 | 0x8;

	at_template("AT+CHLD=4", modem, generic_cb, transfer, cb, data);
}

static void at_deflect(struct ofono_modem *modem,
			const struct ofono_phone_number *ph,
			ofono_generic_cb_t cb, void *data)
{
	char buf[128];
	unsigned int incoming_or_waiting = (0x1 << 4) | (0x1 << 5);

	sprintf(buf, "AT+CTFR=%s,%d", ph->number, ph->type);
	at_template(buf, modem, generic_cb, incoming_or_waiting, cb, data);
}

static void vts_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("vts_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void at_send_dtmf(struct ofono_modem *modem, const char *dtmf,
			ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	int len = strlen(dtmf);
	int s;
	int i;
	char *buf;

	if (!cbd)
		goto error;

	/* strlen("+VTS=\"T\";") = 9 + initial AT + null */
	buf = g_try_new(char, len * 9 + 3);

	if (!buf)
		goto error;

	s = sprintf(buf, "AT+VTS=\"%c\"", dtmf[0]);

	for (i = 1; i < len; i++)
		s += sprintf(buf + s, ";+VTS=\"%c\"", dtmf[i]);

	s = g_at_chat_send(at->parser, buf, none_prefix,
				vts_cb, cbd, g_free);

	g_free(buf);

	if (s > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void ring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct ofono_call *call;

	dump_response("ring_notify", TRUE, result);

	/* RING can repeat, ignore if we already have an incoming call */
	if (g_slist_find_custom(at->voicecall->calls, GINT_TO_POINTER(4),
				call_compare_by_status))
		return;

	/* Generate an incoming call of unknown type */
	call = create_call(at->voicecall, 9, 1, 4, NULL, 128, 2);

	if (!call) {
		ofono_error("Couldn't create call, call management is fubar!");
		return;
	}

	/* We don't know the call type, we must run clcc */
	at->voicecall->clcc_source = g_timeout_add(CLIP_INTERVAL,
							poll_clcc, modem);
}

static void cring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);
	GAtResultIter iter;
	const char *line;
	int type;
	struct ofono_call *call;

	dump_response("cring_notify", TRUE, result);

	/* CRING can repeat, ignore if we already have an incoming call */
	if (g_slist_find_custom(at->voicecall->calls, GINT_TO_POINTER(4),
				call_compare_by_status))
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

	/* Generate an incoming call */
	call = create_call(at->voicecall, type, 1, 4, NULL, 128, 2);

	/* We have a call, and call type but don't know the number and
	 * must wait for the CLIP to arrive before announcing the call.
	 * So we wait, and schedule the clcc call.  If the CLIP arrives
	 * earlier, we announce the call there
	 */
	at->voicecall->clcc_source =
		g_timeout_add(CLIP_INTERVAL, poll_clcc, modem);

	ofono_debug("cring_notify");
}

static void clip_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);
	GAtResultIter iter;
	const char *num;
	int type, validity;
	GSList *l;
	struct ofono_call *call;

	dump_response("clip_notify", TRUE, result);

	l = g_slist_find_custom(at->voicecall->calls, GINT_TO_POINTER(4),
				call_compare_by_status);

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

	ofono_debug("clip_notify: %s %d %d", num, type, validity);

	call = l->data;

	strncpy(call->phone_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = type;
	call->clip_validity = validity;

	if (call->type == 0)
		ofono_voicecall_notify(modem, call);

	/* We started a CLCC, but the CLIP arrived and the call type
	 * is known.  If we don't need to poll, cancel the GSource
	 */
	if (call->type != 9 && !at->voicecall->poll_clcc &&
		at->voicecall->clcc_source &&
			g_source_remove(at->voicecall->clcc_source))
		at->voicecall->clcc_source = 0;
}

static void ccwa_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);
	GAtResultIter iter;
	const char *num;
	int num_type, validity, cls;
	struct ofono_call *call;

	dump_response("ccwa_notify", TRUE, result);

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

	ofono_debug("ccwa_notify: %s %d %d %d", num, num_type, cls, validity);

	call = create_call(at->voicecall, class_to_call_type(cls), 1, 5,
				num, num_type, validity);

	if (!call) {
		ofono_error("malloc call structfailed. Call management is fubar");
		return;
	}

	if (call->type == 0) /* Only notify voice calls */
		ofono_voicecall_notify(modem, call);

	if (at->voicecall->poll_clcc && !at->voicecall->clcc_source)
		at->voicecall->clcc_source = g_timeout_add(POLL_CLCC_INTERVAL,
								poll_clcc,
								modem);
}

static void no_carrier_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);

	if (at->voicecall->poll_clcc)
		g_at_chat_send(at->parser, "AT+CLCC", clcc_prefix,
					clcc_poll_cb, modem, NULL);
}

static void no_answer_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);

	if (at->voicecall->poll_clcc)
		g_at_chat_send(at->parser, "AT+CLCC", clcc_prefix,
					clcc_poll_cb, modem, NULL);
}

static void busy_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);

	/* Call was rejected, most likely due to network congestion
	 * or UDUB on the other side
	 * TODO: Handle UDUB or other conditions somehow
	 */
	if (at->voicecall->poll_clcc)
		g_at_chat_send(at->parser, "AT+CLCC", clcc_prefix,
					clcc_poll_cb, modem, NULL);
}

static void cssi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GAtResultIter iter;
	int code1, index;

	dump_response("cssi_notify", TRUE, result);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code1))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		index = 0;

	ofono_cssi_notify(modem, code1, index);
}

static void cssu_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GAtResultIter iter;
	int code2;
	int index = -1;
	const char *num;
	struct ofono_phone_number ph;

	ph.number[0] = '\0';
	ph.type = 129;

	dump_response("cssu_notify", TRUE, result);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSU:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code2))
		return;

	/* This field is optional, if we can't read it, try to skip it */
	if (!g_at_result_iter_next_number(&iter, &index) &&
			!g_at_result_iter_skip_next(&iter))
		goto out;

	if (!g_at_result_iter_next_string(&iter, &num))
		goto out;

	strncpy(ph.number, num, OFONO_MAX_PHONE_NUMBER_LENGTH);

	if (!g_at_result_iter_next_number(&iter, &ph.type))
		return;

out:
	ofono_cssu_notify(modem, code2, index, &ph);
}

static struct ofono_voicecall_ops ops = {
	.dial			= at_dial,
	.answer			= at_answer,
	.hangup			= at_hangup,
	.list_calls		= at_list_calls,
	.hold_all_active	= at_hold_all_active,
	.release_all_held	= at_release_all_held,
	.set_udub		= at_set_udub,
	.release_all_active	= at_release_all_active,
	.release_specific	= at_release_specific,
	.private_chat		= at_private_chat,
	.create_multiparty	= at_create_multiparty,
	.transfer		= at_transfer,
	.deflect		= at_deflect,
	.swap_without_accept	= NULL,
	.send_tones		= at_send_dtmf
};

static void at_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);

	ofono_debug("voicecall_init: registering to notifications");

	g_at_chat_register(at->parser, "RING",
				ring_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "+CRING:",
				cring_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "+CLIP:",
				clip_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "+CCWA:",
				ccwa_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "+CSSI:",
				cssi_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "+CSSU:",
				cssu_notify, FALSE, modem, NULL);

	/* Modems with 'better' call progress indicators should
	 * probably not even bother registering to these
	 */
	g_at_chat_register(at->parser, "NO CARRIER",
				no_carrier_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "NO ANSWER",
				no_answer_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "BUSY",
				busy_notify, FALSE, modem, NULL);

	ofono_voicecall_register(modem, &ops);
}

void at_voicecall_init(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_get_userdata(modem);

	at->voicecall = g_try_new0(struct voicecall_data, 1);

	if (!at->voicecall)
		return;

	at->voicecall->poll_clcc = TRUE;

	ofono_debug("Sending voice initialization commands");

	g_at_chat_send(at->parser, "AT+CRC=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(at->parser, "AT+CLIP=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(at->parser, "AT+COLP=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(at->parser, "AT+CSSN=1,1", NULL, NULL, NULL, NULL);
	g_at_chat_send(at->parser, "AT+CCWA=1", NULL,
				at_voicecall_initialized, modem, NULL);
}

void at_voicecall_exit(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_get_userdata(modem);

	if (!at->voicecall)
		return;

	g_slist_foreach(at->voicecall->calls, (GFunc) g_free, NULL);
	g_slist_free(at->voicecall->calls);

	g_free(at->voicecall);
	at->voicecall = NULL;

	ofono_voicecall_unregister(modem);
}
