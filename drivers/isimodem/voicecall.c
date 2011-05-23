/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>
#include <gisi/message.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "isimodem.h"
#include "isiutil.h"
#include "call.h"
#include "debug.h"

#define ISI_CALL_TIMEOUT	1000

struct isi_call {
	uint8_t id;
	uint8_t call_id;
	uint8_t status;
	uint8_t prev_status;
	uint8_t mode;
	uint8_t mode_info;
	uint8_t cause_type;
	uint8_t cause;
	uint8_t addr_type;
	uint8_t presentation;
	uint8_t name_presentation;
	uint8_t reason;
	char address[20];
	char name[20];
	char addr_pad[4];
};

struct call_addr_info {
	uint8_t call_id;
	uint8_t mode;
	uint8_t mode_info;
	uint8_t status;
	uint8_t filler[2];
	uint8_t addr_type;
	uint8_t presentation;
	uint8_t filler2;
	uint8_t addr_len;
};

struct call_info {
	uint8_t call_id;
	uint8_t mode;
	uint8_t mode_info;
	uint8_t status;
};

struct isi_voicecall {
	GIsiClient *client;
	GIsiClient *pn_call;
	GIsiClient *pn_modem_call;
	struct isi_call_req_ctx *queue;
	struct isi_call calls[8];
	void *control_req_irc;
};

typedef void isi_call_req_step(struct isi_call_req_ctx *ctx, int reason);

struct isi_call_req_ctx {
	struct isi_call_req_ctx *next;
	struct isi_call_req_ctx **prev;
	isi_call_req_step *step;
	struct ofono_voicecall *ovc;
	ofono_voicecall_cb_t cb;
	void *data;
};

static struct isi_call_req_ctx *isi_call_req(struct ofono_voicecall *ovc,
						const void *__restrict req,
						size_t len,
						GIsiNotifyFunc handler,
						ofono_voicecall_cb_t cb,
						void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct isi_call_req_ctx *irc;

	irc = g_try_new0(struct isi_call_req_ctx, 1);
	if (irc == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return NULL;
	}

	irc->ovc = ovc;
	irc->cb = cb;
	irc->data = data;

	if (g_isi_client_send(ivc->client, req, len, handler, irc, NULL))
		return irc;

	g_free(irc);
	return NULL;
}

static void isi_ctx_queue(struct isi_call_req_ctx *irc, isi_call_req_step *next)
{
	struct isi_voicecall *ivc;

	if (irc->prev != NULL) {
		irc->step = next;
		return;
	}

	ivc = ofono_voicecall_get_data(irc->ovc);
	if (ivc->queue) {
		irc->next = ivc->queue;
		irc->next->prev = &irc->next;
	}

	irc->prev = &ivc->queue;
	ivc->queue = irc;
}

static void isi_ctx_remove(struct isi_call_req_ctx *irc)
{
	if (irc->prev == NULL)
		return;

	*irc->prev = irc->next;

	if (irc->next) {
		irc->next->prev = irc->prev;
		irc->next = NULL;
	}
	irc->prev = NULL;
}

static void isi_ctx_free(struct isi_call_req_ctx *irc)
{
	if (irc == NULL)
		return;

	isi_ctx_remove(irc);
	g_free(irc);
}

static gboolean isi_ctx_return(struct isi_call_req_ctx *irc,
				enum ofono_error_type type, int error)
{
	if (irc == NULL)
		return TRUE;

	if (irc->cb) {
		struct ofono_error e = {
			.type = type,
			.error = error
		};
		irc->cb(&e, irc->data);
	}

	isi_ctx_free(irc);
	return TRUE;
}

static gboolean isi_ctx_return_failure(struct isi_call_req_ctx *irc)
{
	return isi_ctx_return(irc, OFONO_ERROR_TYPE_FAILURE, 0);
}

static gboolean isi_ctx_return_success(struct isi_call_req_ctx *irc)
{
	if (irc == NULL || irc->step == NULL)
		return isi_ctx_return(irc, OFONO_ERROR_TYPE_NO_ERROR, 0);

	irc->step(irc, 0);
	return TRUE;
}

/* Decoding subblocks */
static void isi_call_any_address_sb_proc(struct isi_voicecall *ivc,
						struct isi_call *call,
						GIsiSubBlockIter *sb)
{
	uint8_t type;
	uint8_t pres;
	uint8_t len;
	char *addr;

	if (!g_isi_sb_iter_get_byte(sb, &type, 2))
		return;

	if (!g_isi_sb_iter_get_byte(sb, &pres, 3))
		return;

	if (!g_isi_sb_iter_get_byte(sb, &len, 5))
		return;

	if (!g_isi_sb_iter_get_alpha_tag(sb, &addr, 2 * len, 6))
		return;

	call->addr_type = type | 0x80;
	call->presentation = pres;
	strncpy(call->address, addr, sizeof(call->address));

	g_free(addr);
}

static void isi_call_origin_address_sb_proc(struct isi_voicecall *ivc,
						struct isi_call *call,
						GIsiSubBlockIter *sb)
{
	if (call->address[0] == '\0')
		isi_call_any_address_sb_proc(ivc, call, sb);
}

static void isi_call_destination_address_sb_proc(struct isi_voicecall *ivc,
							struct isi_call *call,
							GIsiSubBlockIter *sb)
{
	if (call->address[0] == '\0')
		isi_call_any_address_sb_proc(ivc, call, sb);
}

static void isi_call_origin_info_sb_proc(struct isi_voicecall *ivc,
						struct isi_call *call,
						GIsiSubBlockIter *sb)
{
	uint8_t pres;
	uint8_t id;
	uint8_t len;
	char *name;

	if (!g_isi_sb_iter_get_byte(sb, &pres, 2))
		return;

	if (!g_isi_sb_iter_get_byte(sb, &id, 6))
		return;

	if (!g_isi_sb_iter_get_byte(sb, &len, 7))
		return;

	if (!g_isi_sb_iter_get_alpha_tag(sb, &name, 2 * len, 8))
		return;

	DBG("Got name %s", name);
	call->name_presentation = pres;
	strncpy(call->name, name, sizeof(call->name));

	g_free(name);
}

static void isi_call_mode_sb_proc(struct isi_voicecall *ivc,
					struct isi_call *call,
					GIsiSubBlockIter *sb)
{
	uint8_t mode;
	uint8_t info;

	if (!g_isi_sb_iter_get_byte(sb, &mode, 2) ||
			!g_isi_sb_iter_get_byte(sb, &info, 3))
		return;

	call->mode = mode;
	call->mode_info = info;
}

static void isi_call_cause_sb_proc(struct isi_voicecall *ivc,
					struct isi_call *call,
					GIsiSubBlockIter *sb)
{
	uint8_t type;
	uint8_t cause;

	if (!g_isi_sb_iter_get_byte(sb, &type, 2) ||
			!g_isi_sb_iter_get_byte(sb, &cause, 3))
		return;

	call->cause_type = type;
	call->cause = cause;
}

static void isi_call_status_sb_proc(struct isi_voicecall *ivc,
					struct isi_call *call,
					GIsiSubBlockIter *sb)
{
	uint8_t status;

	if (!g_isi_sb_iter_get_byte(sb, &status, 2))
		return;
	call->prev_status = call->status;
	call->status = status;
}

static struct isi_call *isi_call_status_info_sb_proc(struct isi_voicecall *ivc,
							GIsiSubBlockIter *sb)
{
	struct isi_call *call = NULL;
	int i;
	struct call_info *ci;
	size_t len = sizeof(struct call_info);

	if (!g_isi_sb_iter_get_struct(sb, (void *) &ci, len, 2))
		return NULL;

	i = ci->call_id & 7;

	if (1 <= i && i <= 7) {
		call = &ivc->calls[i];
		call->call_id = ci->call_id;
		call->status = ci->status;
		call->mode = ci->mode;
		call->mode_info = ci->mode_info;
	}

	return call;
}

static struct isi_call *isi_call_addr_and_status_info_sb_proc(
						struct isi_voicecall *ivc,
						GIsiSubBlockIter *sb)
{
	struct isi_call *call = NULL;
	int i;
	struct call_addr_info *ci;
	size_t len = sizeof(struct call_addr_info);
	char *addr;

	if (!g_isi_sb_iter_get_struct(sb, (void *) &ci, len, 2))
		return NULL;

	if (!g_isi_sb_iter_get_alpha_tag(sb, &addr, 2 * ci->addr_len, 12))
		return NULL;

	i = ci->call_id & 7;

	if (1 <= i && i <= 7) {
		call = &ivc->calls[i];
		call->call_id = ci->call_id;
		call->status = ci->status;
		call->mode = ci->mode;
		call->mode_info = ci->mode_info;
		call->addr_type = ci->addr_type | 0x80;
		call->presentation = ci->presentation;
		strncpy(call->address, addr, sizeof call->address);
	}

	g_free(addr);
	return call;
}

static int isi_call_status_to_clcc(const struct isi_call *call)
{
	switch (call->status) {
	case CALL_STATUS_CREATE:
		return 2;

	case CALL_STATUS_COMING:
		return 4;

	case CALL_STATUS_PROCEEDING:

		if ((call->mode_info & CALL_MODE_ORIGINATOR))
			return 4; /* MT */
		else
			return 2; /* MO */

	case CALL_STATUS_MO_ALERTING:
		return 3;

	case CALL_STATUS_MT_ALERTING:
		return 4;

	case CALL_STATUS_WAITING:
		return 5;

	case CALL_STATUS_MO_RELEASE:
		return 6;

	case CALL_STATUS_MT_RELEASE:
		if ((call->prev_status == CALL_STATUS_MT_ALERTING) ||
				(call->prev_status == CALL_STATUS_COMING) ||
				(call->prev_status == CALL_STATUS_WAITING))
			return 4;
		else
			return 6;

	case CALL_STATUS_ACTIVE:
	case CALL_STATUS_HOLD_INITIATED:
		return 0;

	case CALL_STATUS_HOLD:
	case CALL_STATUS_RETRIEVE_INITIATED:
		return 1;

	case CALL_STATUS_RECONNECT_PENDING:
	case CALL_STATUS_SWAP_INITIATED:
	default:
		return 0;
	}
}

static struct ofono_call isi_call_as_ofono_call(const struct isi_call *call)
{
	struct ofono_call ocall;
	struct ofono_phone_number *number = &ocall.phone_number;

	ofono_call_init(&ocall);
	ocall.id = call->id;
	ocall.type = 0;	/* Voice call */
	ocall.direction = call->mode_info & CALL_MODE_ORIGINATOR;
	ocall.status = isi_call_status_to_clcc(call);

	memcpy(number->number, call->address, sizeof(number->number));
	memcpy(ocall.name, call->name, sizeof(ocall.name));

	number->type = 0x80 | call->addr_type;
	ocall.clip_validity = call->presentation & 3;
	ocall.cnap_validity = call->name_presentation & 3;

	if (ocall.clip_validity == 0 && strlen(number->number) == 0)
		ocall.clip_validity = 2;

	if (ocall.cnap_validity == 0 && strlen(call->name) == 0)
		ocall.cnap_validity = 2;

	return ocall;
}

static gboolean check_response_status(const GIsiMessage *msg, uint8_t msgid)
{
	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			net_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}
	return TRUE;
}

static struct isi_call *isi_call_set_idle(struct isi_call *call)
{
	uint8_t id;

	if (call == NULL)
		return NULL;

	id = call->id;
	memset(call, 0, sizeof(struct isi_call));
	call->id = id;

	return call;
}

static void isi_call_disconnected(struct ofono_voicecall *ovc,
					struct isi_call *call)
{
	struct ofono_error error = {
		OFONO_ERROR_TYPE_NO_ERROR, 0
	};

	DBG("disconnected id=%u reason=%u", call->id, call->reason);

	ofono_voicecall_disconnected(ovc, call->id, call->reason, &error);

	isi_call_set_idle(call);
}

static void isi_call_set_disconnect_reason(struct isi_call *call)
{
	enum ofono_disconnect_reason reason;

	if (call->reason != OFONO_DISCONNECT_REASON_UNKNOWN)
		return;

	switch (call->status) {
	case CALL_STATUS_IDLE:
		reason = OFONO_DISCONNECT_REASON_UNKNOWN;
		break;

	case CALL_STATUS_MO_RELEASE:
		reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
		break;

	case CALL_STATUS_MT_RELEASE:
		reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
		break;

	case CALL_STATUS_TERMINATED:
	default:
		reason = OFONO_DISCONNECT_REASON_ERROR;
	}

	call->reason = reason;
}

static void isi_call_notify(struct ofono_voicecall *ovc, struct isi_call *call)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct isi_call_req_ctx *irc, **queue;
	struct ofono_call ocall;

	DBG("called with status=%s (0x%02X)",
		call_status_name(call->status), call->status);

	for (queue = &ivc->queue; (irc = *queue);) {
		irc->step(irc, call->status);

		if (*queue == irc)
			queue = &irc->next;
	}

	switch (call->status) {
	case CALL_STATUS_IDLE:
		isi_call_disconnected(ovc, call);
		return;

	case CALL_STATUS_COMING:
	case CALL_STATUS_PROCEEDING:
		if ((call->mode_info & CALL_MODE_ORIGINATOR))
			/* Do not notify early MT calls */
			return;
		break;

	case CALL_STATUS_MO_RELEASE:
	case CALL_STATUS_MT_RELEASE:
		/*
		* Core requires the call status to be either incoming
		* or waiting to identify the disconnected call as missed.
		* The MT RELEASE is not mapped to any state in +CLCC, but
		* we need the disconnect reason.
		*/
		isi_call_set_disconnect_reason(call);
		break;
	case CALL_STATUS_TERMINATED:
		DBG("State( CALL_STATUS_TERMINATED ) need not be reported to Core");
		/*
		* The call terminated is not reported to core as
		* these intermediate states are not processed in
		* the core. We report the call status when it becomes
		* idle and TERMINATED is not mapped to +CLCC. The disconnect
		* reason is set, so that the call termination cause
		* in case of error is available to the core.
		*/
		isi_call_set_disconnect_reason(call);
		return;
	case CALL_STATUS_ANSWERED:
		DBG("State need not be reported to Core");
		return;
	}

	ocall = isi_call_as_ofono_call(call);

	DBG("id=%u,%s,%u,\"%s\",\"%s\",%u,%u",
		ocall.id,
		ocall.direction ? "terminated" : "originated",
		ocall.status,
		ocall.phone_number.number,
		ocall.name,
		ocall.phone_number.type,
		ocall.clip_validity);

	ofono_voicecall_notify(ovc, &ocall);
}

static void isi_call_create_resp(const GIsiMessage *msg, void *data)
{
	struct isi_call_req_ctx *irc = data;
	uint8_t call_id;
	uint8_t subblocks;

	if (!check_response_status(msg, CALL_CREATE_RESP))
		goto failure;

	if (!g_isi_msg_data_get_byte(msg, 0, &call_id) ||
			call_id == CALL_ID_NONE)
		goto failure;

	if (!g_isi_msg_data_get_byte(msg, 1, &subblocks))
		goto failure;

	if (subblocks != 0) {
		GIsiSubBlockIter iter;
		struct isi_call call = { 0 };

		for (g_isi_sb_iter_init(&iter, msg, 2);
				g_isi_sb_iter_is_valid(&iter);
				g_isi_sb_iter_next(&iter)) {

			switch (g_isi_sb_iter_get_id(&iter)) {
			case CALL_CAUSE:
				isi_call_cause_sb_proc(NULL, &call, &iter);
				DBG("CALL_CREATE_RESP "
					"cause_type=0x%02x cause=0x%02x",
					call.cause_type, call.cause);
				goto failure;
			}
		}
	}

	isi_ctx_return_success(irc);
	return;

failure:
	isi_ctx_return_failure(irc);
}

static struct isi_call_req_ctx *isi_modem_call_create_req(
						struct ofono_voicecall *ovc,
						uint8_t presentation,
						uint8_t addr_type,
						char const address[21],
						ofono_voicecall_cb_t cb,
						void *data)
{
	size_t addr_len = strlen(address);
	size_t sub_len = ALIGN4(6 + 2 * addr_len);
	size_t offset = 3 + 4 + 4 + 6;
	uint8_t req[3 + 4 + 4 + 6 + 40] = {
		CALL_CREATE_REQ,
		0, /* No id */
		3, /* Mode, Clir, Number */
		CALL_MODE, 4, CALL_MODE_SPEECH, 0,
		CALL_LINE_ID, 4, presentation, 0,
		CALL_DESTINATION_ADDRESS, sub_len, addr_type & 0x7F, 0, 0,
		addr_len,
		/* uint16_t addr[20] */
	};
	size_t rlen = 3 + 4 + 4 + sub_len;
	size_t i;

	if (addr_len > 20) {
		CALLBACK_WITH_FAILURE(cb, data);
		return NULL;
	}

	for (i = 0; i < addr_len; i++)
		req[offset + 2 * i + 1] = address[i];

	return isi_call_req(ovc, req, rlen, isi_call_create_resp, cb, data);
}

static struct isi_call_req_ctx *isi_call_create_req(struct ofono_voicecall *ovc,
							uint8_t presentation,
							uint8_t addr_type,
							char const address[21],
							ofono_voicecall_cb_t cb,
							void *data)
{
	size_t addr_len = strlen(address);
	size_t sub_len = ALIGN4(6 + 2 * addr_len);
	size_t offset = 3 + 4 + 8 + 6;
	uint8_t req[3 + 4 + 8 + 6 + 40] = {
		CALL_CREATE_REQ,
		0,		/* No id */
		3,		/* Mode, Clir, Number */
		CALL_MODE, 4, CALL_MODE_SPEECH, CALL_MODE_INFO_NONE,
		CALL_ORIGIN_INFO, 8, presentation, 0, 0, 0, 0, 0,
		CALL_DESTINATION_ADDRESS, sub_len, addr_type & 0x7F, 0, 0,
		addr_len,
		/* uint16_t addr[20] */
	};
	size_t rlen = 3 + 4 + 8 + sub_len;
	size_t i;

	if (addr_len > 20) {
		CALLBACK_WITH_FAILURE(cb, data);
		return NULL;
	}

	for (i = 0; i < addr_len; i++)
		req[offset + 2 * i + 1] = address[i];

	return isi_call_req(ovc, req, rlen, isi_call_create_resp, cb, data);
}

static void isi_call_status_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_voicecall *ovc = data;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct isi_call *call;
	GIsiSubBlockIter iter;

	uint8_t call_id;
	uint8_t old_status;

	if (ivc == NULL || g_isi_msg_id(msg) != CALL_STATUS_IND ||
			!g_isi_msg_data_get_byte(msg, 0, &call_id) ||
			(call_id & 7) == 0)
		return;

	call = &ivc->calls[call_id & 7];
	old_status = call->status;
	call->call_id = call_id;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case CALL_STATUS:
			isi_call_status_sb_proc(ivc, call, &iter);
			break;

		case CALL_MODE:
			isi_call_mode_sb_proc(ivc, call, &iter);
			break;

		case CALL_CAUSE:
			isi_call_cause_sb_proc(ivc, call, &iter);
			break;

		case CALL_DESTINATION_ADDRESS:
			isi_call_destination_address_sb_proc(ivc, call, &iter);
			break;

		case CALL_ORIGIN_ADDRESS:
			isi_call_origin_address_sb_proc(ivc, call, &iter);
			break;

		case CALL_ORIGIN_INFO:
			isi_call_origin_info_sb_proc(ivc, call, &iter);
			break;

		case CALL_GSM_DETAILED_CAUSE:
		case CALL_DESTINATION_PRE_ADDRESS:
		case CALL_DESTINATION_POST_ADDRESS:
		case CALL_DESTINATION_SUBADDRESS:
		case CALL_GSM_EVENT_INFO:
		case CALL_NW_CAUSE:
			break;
		}
	}

	if (old_status == call->status)
		return;

	isi_call_notify(ovc, call);
}

static void isi_call_terminated_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_voicecall *ovc = data;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct isi_call *call;

	uint8_t call_id;
	uint8_t old_status;

	if (ivc == NULL || g_isi_msg_id(msg) != CALL_TERMINATED_IND ||
			!g_isi_msg_data_get_byte(msg, 0, &call_id) ||
			(call_id & 7) == 0)
		return;

	call = &ivc->calls[call_id & 7];
	old_status = call->status;

	if (old_status == CALL_STATUS_IDLE)
		return;

	call->status = CALL_STATUS_TERMINATED;
	isi_call_notify(ovc, call);
}

static gboolean decode_notify(GIsiSubBlockIter *iter)
{
	uint8_t byte;

	if (!g_isi_sb_iter_get_byte(iter, &byte, 2))
		return FALSE;

	switch (byte) {
	case CALL_NOTIFY_USER_SUSPENDED:
		DBG("CALL_NOTIFY_USER_SUSPENDED");
		break;

	case CALL_NOTIFY_USER_RESUMED:
		DBG("CALL_NOTIFY_USER_RESUMED");
		break;

	case CALL_NOTIFY_BEARER_CHANGE:
		DBG("CALL_NOTIFY_BEARER_CHANGE");
		break;

	default:
		DBG("Unknown notification: 0x%02X", byte);
	}

	return TRUE;
}

static gboolean decode_ss_code(GIsiSubBlockIter *iter, int *cssi, int *cssu)
{
	uint16_t word;

	if (!g_isi_sb_iter_get_word(iter, &word, 2))
		return FALSE;

	switch (word) {
	case CALL_SSC_ALL_FWDS:
		DBG("Call forwarding is active");
		break;

	case CALL_SSC_ALL_COND_FWD:
		*cssi = SS_MO_CONDITIONAL_FORWARDING;
		DBG("Some of conditional call forwardings active");
		break;

	case CALL_SSC_CFU:
		*cssi = SS_MO_UNCONDITIONAL_FORWARDING;
		DBG("Unconditional call forwarding is active");
		break;

	case CALL_SSC_OUTGOING_BARR_SERV:
		*cssi = SS_MO_OUTGOING_BARRING;
		DBG("Outgoing calls are barred");
		break;

	case CALL_SSC_INCOMING_BARR_SERV:
		*cssi = SS_MO_INCOMING_BARRING;
		DBG("Incoming calls are barred");
		break;

	case CALL_SSC_CALL_WAITING:
		DBG("Incoming calls are barred");
		break;

	case CALL_SSC_CLIR:
		DBG("CLIR connected unknown indication.");
		break;

	case CALL_SSC_MPTY:
		*cssu = SS_MT_MULTIPARTY_VOICECALL;
		DBG("Multiparty call entered.");
		break;

	case CALL_SSC_CALL_HOLD:
		*cssu = SS_MT_VOICECALL_HOLD_RELEASED;
		DBG("Call on hold has been released.");
		break;

	default:
		DBG("Unknown/unhandled notification: 0x%02X", word);
		break;
	}

	return TRUE;
}

static gboolean decode_ss_status(GIsiSubBlockIter *iter)
{
	uint8_t byte;

	if (!g_isi_sb_iter_get_byte(iter, &byte, 2))
		return FALSE;

	if (byte & CALL_SS_STATUS_ACTIVE)
		DBG("CALL_SS_STATUS_ACTIVE");

	if (byte & CALL_SS_STATUS_REGISTERED)
		DBG("CALL_SS_STATUS_REGISTERED");

	if (byte & CALL_SS_STATUS_PROVISIONED)
		DBG("CALL_SS_STATUS_PROVISIONED");

	if (byte & CALL_SS_STATUS_QUIESCENT)
		DBG("CALL_SS_STATUS_QUIESCENT");

	return TRUE;
}

static gboolean decode_ss_notify(GIsiSubBlockIter *iter, int *cssi, int *cssu)
{
	uint8_t byte;

	if (!g_isi_sb_iter_get_byte(iter, &byte, 2))
		return FALSE;

	if (byte & CALL_SSN_INCOMING_IS_FWD) {
		*cssu = SS_MT_CALL_FORWARDED;
		DBG("This is a forwarded call #1.");
	}

	if (byte & CALL_SSN_INCOMING_FWD)
		DBG("This is a forwarded call #2.");

	if (byte & CALL_SSN_OUTGOING_FWD) {
		*cssi = SS_MO_CALL_FORWARDED;
		DBG("Call has been forwarded.");
	}

	return TRUE;
}

static gboolean decode_ss_notify_indicator(GIsiSubBlockIter *iter, int *cssi)
{
	uint8_t byte;

	if (!g_isi_sb_iter_get_byte(iter, &byte, 2))
		return FALSE;

	if (byte & CALL_SSI_CALL_IS_WAITING) {
		*cssi = SS_MO_CALL_WAITING;
		DBG("Call is waiting.");
	}

	if (byte & CALL_SSI_MPTY)
		DBG("Multiparty call");

	if (byte & CALL_SSI_CLIR_SUPPR_REJ) {
		*cssi = SS_MO_CLIR_SUPPRESSION_REJECTED;
		DBG("CLIR suppression rejected");
	}

	return TRUE;
}

static gboolean decode_ss_hold_indicator(GIsiSubBlockIter *iter, int *cssu)
{
	uint8_t byte;

	if (!g_isi_sb_iter_get_byte(iter, &byte, 2))
		return FALSE;

	if (byte == CALL_HOLD_IND_RETRIEVED) {
		*cssu = SS_MT_VOICECALL_RETRIEVED;
		DBG("Call has been retrieved");
	} else if (byte & CALL_HOLD_IND_ON_HOLD) {
		*cssu = SS_MT_VOICECALL_ON_HOLD;
		DBG("Call has been put on hold");
	} else {
		return FALSE;
	}

	return TRUE;
}

static gboolean decode_ss_ect_indicator(GIsiSubBlockIter *iter, int *cssu)
{
	uint8_t byte;

	if (!g_isi_sb_iter_get_byte(iter, &byte, 2))
		return FALSE;

	if (byte & CALL_ECT_CALL_STATE_ALERT) {
		*cssu = SS_MT_VOICECALL_IN_TRANSFER;
		DBG("Call is being connected with the remote party in "
			"alerting state");
	}

	if (byte & CALL_ECT_CALL_STATE_ACTIVE) {
		*cssu = SS_MT_VOICECALL_TRANSFERRED;
		DBG("Call has been connected with the other remote "
			"party in explicit call transfer operation.");
	}

	return TRUE;
}

static gboolean decode_remote_address(GIsiSubBlockIter *iter,
					struct ofono_phone_number *number,
					int *index)
{
	uint8_t type, len;
	char *addr;

	if (!g_isi_sb_iter_get_byte(iter, &type, 2))
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &len, 5))
		return FALSE;

	if (len > OFONO_MAX_PHONE_NUMBER_LENGTH)
		return FALSE;

	if (!g_isi_sb_iter_get_alpha_tag(iter, &addr, 2 * len, 6))
		return FALSE;

	strncpy(number->number, addr, len);
	number->number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	number->type = type;

	g_free(addr);

	return TRUE;
}

static gboolean decode_cug_info(GIsiSubBlockIter *iter, int *index, int *cssu)
{
	uint8_t pref;
	uint8_t access;
	uint16_t word;

	if (!g_isi_sb_iter_get_byte(iter, &pref, 2))
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &access, 3))
		return FALSE;

	if (!g_isi_sb_iter_get_word(iter, &word, 4))
		return FALSE;

	DBG("Preferential CUG: 0x%02X", pref);
	DBG("CUG output access: 0x%02X", access);

	*index = word;
	*cssu = SS_MO_CUG_CALL;

	return TRUE;
}

static void notification_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_voicecall *ovc = data;
	GIsiSubBlockIter iter;

	struct ofono_phone_number number;
	int index = 0;
	int cssi = -1;
	int cssu = -1;
	uint8_t call_id;

	if (ovc == NULL || g_isi_msg_id(msg) != CALL_GSM_NOTIFICATION_IND ||
			!g_isi_msg_data_get_byte(msg, 0, &call_id) ||
			(call_id & 7) == 0)
		return;

	DBG("Received CallServer notification for call: 0x%02X", call_id);

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case CALL_GSM_NOTIFY:
			if (!decode_notify(&iter))
				return;

			break;

		case CALL_GSM_SS_CODE:
			if (!decode_ss_code(&iter, &cssi, &cssu))
				return;

			break;

		case CALL_GSM_SS_STATUS:
			if (!decode_ss_status(&iter))
				return;

			break;

		case CALL_GSM_SS_NOTIFY:
			if (!decode_ss_notify(&iter, &cssi, &cssu))
				return;

			break;

		case CALL_GSM_SS_NOTIFY_INDICATOR:
			if (!decode_ss_notify_indicator(&iter, &cssi))
				return;

			break;

		case CALL_GSM_SS_HOLD_INDICATOR:
			if (!decode_ss_hold_indicator(&iter, &cssu))
				return;

			break;

		case CALL_GSM_SS_ECT_INDICATOR:
			if (!decode_ss_ect_indicator(&iter, &cssu))
				return;

			break;

		case CALL_GSM_REMOTE_ADDRESS:
			if (!decode_remote_address(&iter, &number, &index))
				return;

			break;

		case CALL_GSM_REMOTE_SUBADDRESS:
			break;

		case CALL_GSM_CUG_INFO:
			if (!decode_cug_info(&iter, &index, &cssu))
				return;

			break;

		case CALL_ORIGIN_INFO:
			break;

		case CALL_GSM_ALERTING_PATTERN:
			break;

		case CALL_ALERTING_INFO:
			break;
		}
	}

	if (cssi != -1)
		ofono_voicecall_ssn_mo_notify(ovc, call_id & 7, cssi, index);

	if (cssu != -1)
		ofono_voicecall_ssn_mt_notify(ovc, call_id & 7, cssu, index,
						&number);
}

static void isi_call_answer_resp(const GIsiMessage *msg, void *data)
{
	struct isi_call_req_ctx *irc = data;
	uint8_t call_id;

	if (!check_response_status(msg, CALL_ANSWER_RESP) ||
			!g_isi_msg_data_get_byte(msg, 0, &call_id) ||
			call_id == CALL_ID_NONE) {
		isi_ctx_return_failure(irc);
		return;
	}

	isi_ctx_return_success(irc);
}

static struct isi_call_req_ctx *isi_call_answer_req(struct ofono_voicecall *ovc,
							uint8_t call_id,
							ofono_voicecall_cb_t cb,
							void *data)
{
	const uint8_t req[] = {
		CALL_ANSWER_REQ,
		call_id,
		0
	};

	return isi_call_req(ovc, req, sizeof(req), isi_call_answer_resp,
				cb, data);
}

static void isi_call_release_resp(const GIsiMessage *msg, void *data)
{
	struct isi_call_req_ctx *irc = data;
	GIsiSubBlockIter iter;
	uint8_t cause_type;
	uint8_t cause;

	if (!check_response_status(msg, CALL_RELEASE_RESP))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != CALL_CAUSE)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &cause_type, 2) ||
				!g_isi_sb_iter_get_byte(&iter, &cause, 3))
			goto error;
	}

	if ((cause_type == CALL_CAUSE_TYPE_SERVER ||
			cause_type == CALL_CAUSE_TYPE_CLIENT) &&
			(cause == CALL_CAUSE_RELEASE_BY_USER ||
			cause == CALL_CAUSE_BUSY_USER_REQUEST)) {
		isi_ctx_return_success(irc);
		return;
	}

error:
	isi_ctx_return_failure(irc);
}

static struct isi_call_req_ctx *isi_call_release_req(
						struct ofono_voicecall *ovc,
						uint8_t call_id,
						enum call_cause_type cause_type,
						uint8_t cause,
						ofono_voicecall_cb_t cb,
						void *data)
{
	const uint8_t req[] = {
		CALL_RELEASE_REQ,
		call_id,
		1,	/* Sub-block count */
		CALL_CAUSE,
		4,	/* Sub-block length */
		cause_type,
		cause,
	};

	return isi_call_req(ovc, req, sizeof(req), isi_call_release_resp,
				cb, data);
}

static void isi_call_status_resp(const GIsiMessage *msg, void *data)
{
	struct isi_call_req_ctx *irc = data;
	struct ofono_voicecall *ovc = irc->ovc;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	GIsiSubBlockIter iter;
	struct isi_call *call = NULL;

	if (!check_response_status(msg, CALL_STATUS_RESP)) {
		isi_ctx_return_failure(irc);
		return;
	}

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case CALL_STATUS_INFO:
			call = isi_call_status_info_sb_proc(ivc, &iter);
			break;

		case CALL_ADDR_AND_STATUS_INFO:
			call = isi_call_addr_and_status_info_sb_proc(ivc,
									&iter);
			if (call)
				isi_call_notify(ovc, call);
			break;

		case CALL_CAUSE:

			if (call)
				isi_call_cause_sb_proc(ivc, call, &iter);
			break;
		}
	}

	isi_ctx_return_success(irc);
}

static struct isi_call_req_ctx *isi_call_status_req(struct ofono_voicecall *ovc,
							uint8_t call_id,
							uint8_t mode,
							ofono_voicecall_cb_t cb,
							void *data)
{
	const uint8_t req[] = {
		CALL_STATUS_REQ,
		call_id,
		1,	/* Sub-block count */
		CALL_STATUS_MODE,
		4,	/* Sub-block length */
		mode, 0,
	};

	return isi_call_req(ovc, req, sizeof(req), isi_call_status_resp,
				cb, data);
}

static void isi_call_control_resp(const GIsiMessage *msg, void *data)
{
	struct isi_call_req_ctx *irc = data;
	GIsiSubBlockIter iter;
	uint8_t cause = CALL_CAUSE_NO_CAUSE;
	uint8_t cause_type = 0;

	if (!check_response_status(msg, CALL_CONTROL_RESP))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != CALL_CAUSE)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &cause_type, 2) ||
				!g_isi_sb_iter_get_byte(&iter, &cause, 3))
			goto error;
	}

	if (cause == CALL_CAUSE_NO_CAUSE) {
		isi_ctx_return_success(irc);
		return;
	}

error:
	isi_ctx_return_failure(irc);
}

static struct isi_call_req_ctx *isi_call_control_req(
						struct ofono_voicecall *ovc,
						uint8_t call_id,
						enum call_operation op,
						uint8_t info,
						ofono_voicecall_cb_t cb,
						void *data)
{
	const uint8_t req[] = {
		CALL_CONTROL_REQ,
		call_id,
		1,	/* Sub-block count */
		CALL_OPERATION,
		4,	/* Sub-block length */
		op, info,
	};

	return isi_call_req(ovc, req, sizeof(req), isi_call_control_resp,
				cb, data);
}

static struct isi_call_req_ctx *isi_call_deflect_req(
						struct ofono_voicecall *ovc,
						uint8_t call_id,
						uint8_t address_type,
						const char address[21],
						ofono_voicecall_cb_t cb,
						void *data)
{
	size_t addr_len = strlen(address);
	size_t sub_len = (6 + 2 * addr_len + 3) & ~3;
	size_t i, offset = 3 + 4 + 6;
	size_t rlen = 3 + 4 + sub_len;
	uint8_t req[3 + 4 + 6 + 40] = {
		CALL_CONTROL_REQ,
		call_id,
		2,		/* Sub-block count */
		CALL_OPERATION,
		4,		/* Sub-block length */
		CALL_GSM_OP_DEFLECT, 0,
		CALL_GSM_DEFLECTION_ADDRESS,
		sub_len,	/* Sub-block length */
		address_type & 0x7F,
		0x7,		/* Default presentation */
		0,		/* Filler */
		addr_len,
	};

	if (addr_len > 20) {
		CALLBACK_WITH_FAILURE(cb, data);
		return NULL;
	}

	for (i = 0; i < addr_len; i++)
		req[offset + 2 * i + 1] = address[i];

	return isi_call_req(ovc, req, rlen, isi_call_control_resp, cb, data);
}

static void isi_call_control_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_voicecall *ovc = data;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	GIsiSubBlockIter iter;
	uint8_t cause_type = 0, cause = 0;

	if (ivc == NULL || g_isi_msg_id(msg) != CALL_CONTROL_IND)
		return;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != CALL_CAUSE)
			continue;
		if (!g_isi_sb_iter_get_byte(&iter, &cause_type, 2) ||
				!g_isi_sb_iter_get_byte(&iter, &cause, 3))
			return;
	}

	if (ivc->control_req_irc) {
		if (!cause)
			isi_ctx_return_success(ivc->control_req_irc);
		else
			isi_ctx_return_failure(ivc->control_req_irc);

		ivc->control_req_irc = NULL;
	}
}

static void isi_call_dtmf_send_resp(const GIsiMessage *msg, void *data)
{
	struct isi_call_req_ctx *irc = data;
	GIsiSubBlockIter iter;
	uint8_t cause_type;
	uint8_t cause = CALL_CAUSE_NO_CAUSE;

	if (!check_response_status(msg, CALL_DTMF_SEND_RESP))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != CALL_CAUSE)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &cause_type, 2) ||
				!g_isi_sb_iter_get_byte(&iter, &cause, 3))
			goto error;
	}

	if (cause == CALL_CAUSE_NO_CAUSE) {
		isi_ctx_return_success(irc);
		return;
	}

error:
	isi_ctx_return_failure(irc);
}

static struct isi_call_req_ctx *isi_call_dtmf_send_req(
						struct ofono_voicecall *ovc,
						uint8_t call_id,
						const char *string,
						ofono_voicecall_cb_t cb,
						void *data)
{
	size_t str_len = strlen(string);
	size_t sub_len = 4 + ((2 * str_len + 3) & ~3);
	size_t i, offset = 3 + 4 + 8 + 4;
	size_t rlen = 3 + 4 + 8 + sub_len;
	uint8_t req[3 + 4 + 8 + (255 & ~3)] = {
		CALL_DTMF_SEND_REQ, call_id, 3,
		CALL_DTMF_INFO, 4, CALL_DTMF_ENABLE_TONE_IND_SEND, 0,
		CALL_DTMF_TIMERS, 8,
		0, 200, /* duration in ms */
		0, 100, /* gap in ms */
		0, 0,	/* filler */
		CALL_DTMF_STRING, sub_len,
		100,     /* pause length */
		str_len,
		/* string */
	};

	if (sub_len >= 256) {
		CALLBACK_WITH_FAILURE(cb, data);
		return FALSE;
	}

	for (i = 0; i < str_len; i++)
		req[offset + 2 * i + 1] = string[i];

	return isi_call_req(ovc, req, rlen, isi_call_dtmf_send_resp, cb, data);
}

static void isi_dial(struct ofono_voicecall *ovc,
			const struct ofono_phone_number *number,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	gboolean have_pn_call = g_isi_client_resource(ivc->client) == PN_CALL;
	unsigned char presentation;

	switch (clir) {
	case OFONO_CLIR_OPTION_INVOCATION:
		presentation = CALL_PRESENTATION_RESTRICTED;
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		presentation = CALL_PRESENTATION_ALLOWED;
		break;
	case OFONO_CLIR_OPTION_DEFAULT:
	default:
		presentation = have_pn_call ? CALL_GSM_PRESENTATION_DEFAULT :
				CALL_MODEM_PROP_PRESENT_DEFAULT;
	}

	if (have_pn_call)
		isi_call_create_req(ovc, presentation, number->type,
					number->number, cb, data);
	else
		isi_modem_call_create_req(ovc, presentation, number->type,
						number->number, cb, data);
}

static void isi_answer(struct ofono_voicecall *ovc, ofono_voicecall_cb_t cb,
			void *data)
{
	isi_call_answer_req(ovc, CALL_ID_ALL, cb, data);
}

static void isi_hangup_current(struct ofono_voicecall *ovc,
				ofono_voicecall_cb_t cb, void *data)
{
	/*
	 * Hangup call(s) that are not held or waiting:
	 * active calls or calls in progress.
	 */
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	int id = 0;
	uint8_t cause = CALL_CAUSE_RELEASE_BY_USER;

	for (id = 1; id <= 7; id++) {
		if (ivc->calls[id].call_id & CALL_ID_WAITING)
			continue;
		if (ivc->calls[id].call_id & CALL_ID_HOLD)
			continue;

		switch (ivc->calls[id].status) {
		case CALL_STATUS_CREATE:
		case CALL_STATUS_COMING:
		case CALL_STATUS_MO_ALERTING:
		case CALL_STATUS_ANSWERED:
		case CALL_STATUS_HOLD_INITIATED:
			goto release_by_id;
		case CALL_STATUS_MT_ALERTING:
			cause = CALL_CAUSE_BUSY_USER_REQUEST;
			goto release_by_id;
		case CALL_STATUS_PROCEEDING:
			if (ivc->calls[id].mode_info & CALL_MODE_ORIGINATOR)
				cause = CALL_CAUSE_BUSY_USER_REQUEST;
			goto release_by_id;
		}
	}

	id = CALL_ID_ACTIVE;

release_by_id:
	isi_call_release_req(ovc, id, CALL_CAUSE_TYPE_CLIENT, cause, cb, data);
}

static void isi_release_all_held(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	isi_call_release_req(ovc, CALL_ID_HOLD, CALL_CAUSE_TYPE_CLIENT,
				CALL_CAUSE_RELEASE_BY_USER, cb, data);
}

static void isi_set_udub(struct ofono_voicecall *ovc,
				ofono_voicecall_cb_t cb, void *data)
{
	/* Release waiting calls */
	isi_call_release_req(ovc, CALL_ID_WAITING,
				CALL_CAUSE_TYPE_CLIENT,
				CALL_CAUSE_BUSY_USER_REQUEST, cb, data);
}

static void isi_retrieve(struct ofono_voicecall *ovc,
				ofono_voicecall_cb_t cb, void *data)
{
	isi_call_control_req(ovc, CALL_ID_HOLD, CALL_OP_RETRIEVE, 0, cb, data);
}

static void isi_wait_and_answer(struct isi_call_req_ctx *irc, int event)
{
	DBG("irc=%p event=%u", (void *) irc, event);

	if (event != CALL_STATUS_TERMINATED)
		return;

	isi_answer(irc->ovc, irc->cb, irc->data);
	isi_ctx_free(irc);
}

static void isi_wait_and_retrieve(struct isi_call_req_ctx *irc, int event)
{
	DBG("irc=%p event=%u", (void *) irc, event);

	if (event != CALL_STATUS_TERMINATED)
		return;

	isi_retrieve(irc->ovc, irc->cb, irc->data);
	isi_ctx_free(irc);
}

static void isi_release_all_active(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct isi_call_req_ctx *irc;
	int id = 0;
	int waiting = 0;
	int active = 0;
	int hold = 0;

	for (id = 1; id <= 7; id++) {

		if (ivc->calls[id].call_id & CALL_ID_WAITING)
			waiting++;

		if (ivc->calls[id].call_id & CALL_ID_HOLD)
			hold++;

		if (ivc->calls[id].call_id & CALL_ID_ACTIVE)
			active++;
	}

	if (!active) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	irc = isi_call_release_req(ovc, CALL_ID_ACTIVE,
					CALL_CAUSE_TYPE_CLIENT,
					CALL_CAUSE_RELEASE_BY_USER,
					cb, data);
	if (irc == NULL)
		return;

	if (waiting)
		isi_ctx_queue(irc, isi_wait_and_answer);
	else if (hold)
		isi_ctx_queue(irc, isi_wait_and_retrieve);
}

static void isi_hold_all_active(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	int id = 0;
	int op = 0;
	int waiting = 0;
	int active = 0;
	int hold = 0;

	for (id = 1; id <= 7; id++) {

		if (ivc->calls[id].call_id & CALL_ID_WAITING)
			waiting++;

		if (ivc->calls[id].call_id & CALL_ID_HOLD)
			hold++;

		if (ivc->calls[id].call_id & CALL_ID_ACTIVE)
			active++;
	}

	if (!waiting && !hold && !active) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	if (waiting) {
		isi_call_answer_req(ovc, CALL_ID_WAITING, cb, data);

	} else if (hold) {

		if (active) {
			op = CALL_OP_SWAP;
			id = CALL_ID_ACTIVE;
		} else {
			op = CALL_OP_RETRIEVE;
			id = CALL_ID_HOLD;
		}
		isi_call_control_req(ovc, id, op, 0, cb, data);

	} else if (active) {
		id = CALL_ID_ACTIVE;
		op = CALL_OP_HOLD;

		isi_call_control_req(ovc, id, op, 0, cb, data);
	}
}

static void isi_release_specific(struct ofono_voicecall *ovc, int id,
					ofono_voicecall_cb_t cb, void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	const struct isi_call *status;
	uint8_t cause;

	if (id < 1 || id > 7) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	status = &ivc->calls[id];
	cause = CALL_CAUSE_RELEASE_BY_USER;

	switch (status->status) {
	case CALL_STATUS_MT_ALERTING:
	case CALL_STATUS_WAITING:
		cause = CALL_CAUSE_BUSY_USER_REQUEST;
		break;

	case CALL_STATUS_PROCEEDING:

		if ((status->mode_info & CALL_MODE_ORIGINATOR))
			cause = CALL_CAUSE_BUSY_USER_REQUEST;
			break;
	}

	isi_call_release_req(ovc, id, CALL_CAUSE_TYPE_CLIENT, cause, cb, data);
}

static void isi_private_chat(struct ofono_voicecall *ovc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	if (id < 1 || id > 7) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	isi_call_control_req(ovc, id, CALL_OP_CONFERENCE_SPLIT, 0, cb, data);
}

static void isi_create_multiparty(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	isi_call_control_req(ovc, CALL_ID_ALL, CALL_OP_CONFERENCE_BUILD, 0,
				cb, data);
}

static void isi_transfer(struct ofono_voicecall *ovc,
				ofono_voicecall_cb_t cb, void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	uint8_t id;

	for (id = 1; id <= 7; id++) {

		if (ivc->calls[id].status == CALL_STATUS_MO_ALERTING)
			break;
	}

	if (id > 7)
		id = CALL_ID_ACTIVE;

	isi_call_control_req(ovc, id, CALL_GSM_OP_TRANSFER, 0, cb, data);
}

static void isi_deflect(struct ofono_voicecall *ovc,
			const struct ofono_phone_number *ph,
			ofono_voicecall_cb_t cb, void *data)
{
	isi_call_deflect_req(ovc, CALL_ID_WAITING, ph->type, ph->number,
				cb, data);
}

static void isi_swap_without_accept(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	int id = 0;
	int op = 0;
	int active = 0;
	int hold = 0;

	for (id = 1; id <= 7; id++) {

		if (ivc->calls[id].call_id & CALL_ID_HOLD)
			hold++;

		if (ivc->calls[id].call_id & CALL_ID_ACTIVE)
			active++;
	}

	if (hold && active) {
		id = CALL_ID_ACTIVE;
		op = CALL_OP_SWAP;
	} else if (active) {
		id = CALL_ID_ACTIVE;
		op = CALL_OP_HOLD;
	} else if (hold) {
		id = CALL_ID_HOLD;
		op = CALL_OP_RETRIEVE;
	} else {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	isi_call_control_req(ovc, id, op, 0, cb, data);
}

static void isi_send_tones(struct ofono_voicecall *ovc, const char *tones,
				ofono_voicecall_cb_t cb, void *data)
{
	isi_call_dtmf_send_req(ovc, CALL_ID_ALL, tones, cb, data);;
}

static void subscribe_indications(GIsiClient *cl, void *data)
{
	g_isi_client_ind_subscribe(cl, CALL_STATUS_IND, isi_call_status_ind_cb,
					data);
	g_isi_client_ind_subscribe(cl, CALL_CONTROL_IND, isi_call_control_ind_cb,
					data);
	g_isi_client_ind_subscribe(cl, CALL_TERMINATED_IND,
					isi_call_terminated_ind_cb, data);
	g_isi_client_ind_subscribe(cl, CALL_GSM_NOTIFICATION_IND,
					notification_ind_cb, data);

}

static void pn_call_verify_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_voicecall *ovc = data;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);

	if (g_isi_msg_error(msg) < 0) {
		DBG("PN_CALL not reachable, removing client");
		g_isi_client_destroy(ivc->pn_call);
		ivc->pn_call = NULL;

		if (ivc->pn_modem_call == NULL)
			ofono_voicecall_remove(ovc);

		return;
	}

	ISI_RESOURCE_DBG(msg);

	if (ivc == NULL || ivc->client != NULL)
		return;

	ivc->client = ivc->pn_call;

	subscribe_indications(ivc->client, ovc);

	if (!isi_call_status_req(ovc, CALL_ID_ALL,
					CALL_STATUS_MODE_ADDR_AND_ORIGIN,
					NULL, NULL))
		DBG("Failed to request call status");

	ofono_voicecall_register(ovc);
}

static void pn_modem_call_verify_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_voicecall *ovc = data;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);

	if (g_isi_msg_error(msg) < 0) {
		DBG("PN_MODEM_CALL not reachable, removing client");
		g_isi_client_destroy(ivc->pn_modem_call);
		ivc->pn_modem_call = NULL;

		if (ivc->pn_call == NULL)
			ofono_voicecall_remove(ovc);

		return;
	}

	ISI_RESOURCE_DBG(msg);

	if (ivc == NULL || ivc->client != NULL)
		return;

	ivc->client = ivc->pn_modem_call;

	subscribe_indications(ivc->client, ovc);

	if (!isi_call_status_req(ovc, CALL_ID_ALL,
					CALL_STATUS_MODE_ADDR_AND_ORIGIN,
					NULL, NULL))
		DBG("Failed to request call status");

	ofono_voicecall_register(ovc);
}

static int isi_probe(struct ofono_voicecall *ovc, unsigned int vendor,
			void *user)
{
	GIsiModem *modem = user;
	struct isi_voicecall *ivc;
	int id;

	ivc = g_try_new0(struct isi_voicecall, 1);
	if (ivc == NULL)
		return -ENOMEM;

	for (id = 0; id <= 7; id++)
		ivc->calls[id].id = id;

	ivc->pn_call = g_isi_client_create(modem, PN_CALL);
	if (ivc->pn_call == NULL) {
		g_free(ivc);
		return -ENOMEM;
	}

	ivc->pn_modem_call = g_isi_client_create(modem, PN_MODEM_CALL);
	if (ivc->pn_call == NULL) {
		g_isi_client_destroy(ivc->pn_call);
		g_free(ivc);
		return -ENOMEM;
	}

	ofono_voicecall_set_data(ovc, ivc);

	g_isi_client_verify(ivc->pn_call, pn_call_verify_cb, ovc, NULL);
	g_isi_client_verify(ivc->pn_modem_call, pn_modem_call_verify_cb,
				ovc, NULL);

	return 0;
}

static void isi_remove(struct ofono_voicecall *call)
{
	struct isi_voicecall *data = ofono_voicecall_get_data(call);

	ofono_voicecall_set_data(call, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->pn_call);
	g_isi_client_destroy(data->pn_modem_call);
	g_free(data);
}

static struct ofono_voicecall_driver driver = {
	.name			= "isimodem",
	.probe			= isi_probe,
	.remove			= isi_remove,
	.dial			= isi_dial,
	.answer			= isi_answer,
	.hangup_active		= isi_hangup_current,
	.hold_all_active	= isi_hold_all_active,
	.release_all_held	= isi_release_all_held,
	.set_udub		= isi_set_udub,
	.release_all_active	= isi_release_all_active,
	.release_specific	= isi_release_specific,
	.private_chat		= isi_private_chat,
	.create_multiparty	= isi_create_multiparty,
	.transfer		= isi_transfer,
	.deflect		= isi_deflect,
	.swap_without_accept	= isi_swap_without_accept,
	.send_tones		= isi_send_tones,
};

void isi_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void isi_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
