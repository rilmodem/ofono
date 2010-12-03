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
#include <assert.h>

#include <glib.h>

#include <gisi/netlink.h>
#include <gisi/client.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "isimodem.h"
#include "isiutil.h"
#include "call.h"
#include "debug.h"

struct isi_call {
	uint8_t id, call_id, status, mode, mode_info, cause_type, cause;
	uint8_t addr_type, presentation;
	uint8_t reason;
	char address[20], addr_pad[4];
};

struct isi_voicecall {
	GIsiClient *client;

	struct isi_call_req_context *queue;

	struct isi_call calls[8];
};

/* ------------------------------------------------------------------------- */

typedef void GIsiIndication(GIsiClient *client,
		const void *restrict data, size_t len,
		uint16_t object, void *opaque);

typedef void GIsiVerify(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque);

typedef gboolean GIsiResponse(GIsiClient *client,
				void const *restrict data, size_t len,
				uint16_t object, void *opaque);

enum {
	ISI_CALL_TIMEOUT = 1000,
};

/* ------------------------------------------------------------------------- */
/* Request context for voicecall cb */

typedef void isi_call_req_step(struct isi_call_req_context *,
				int id, int status);

struct isi_call_req_context {
	struct isi_call_req_context *next;
	struct isi_call_req_context **prev;
	isi_call_req_step *step;
	int id;
	struct ofono_voicecall *ovc;
	ofono_voicecall_cb_t cb;
	void *data;
};

static struct isi_call_req_context *isi_call_req_new(
					struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb,
					void *data)
{
	struct isi_call_req_context *irc;

	irc = g_try_new0(struct isi_call_req_context, 1);
	if (irc == NULL)
		goto failed;

	irc->ovc = ovc;
	irc->cb = cb;
	irc->data = data;

	return irc;

failed:
	if (cb)
		CALLBACK_WITH_FAILURE(cb, data);

	return NULL;
}

static struct isi_call_req_context *isi_call_req(struct ofono_voicecall *ovc,
						void const *restrict req,
						size_t len,
						GIsiResponse *handler,
						ofono_voicecall_cb_t cb,
						void *data)
{
	struct isi_voicecall *ivc;
	struct isi_call_req_context *irc;

	irc = isi_call_req_new(ovc, cb, data);
	if (irc == NULL)
		return NULL;

	ivc = ofono_voicecall_get_data(ovc);

	if (g_isi_send(ivc->client, req, len,
			ISI_CALL_TIMEOUT, handler, irc, NULL))
		return irc;

	g_free(irc);

	if (cb)
		CALLBACK_WITH_FAILURE(cb, data);

	return NULL;
}

static void isi_ctx_queue(struct isi_call_req_context *irc,
				isi_call_req_step *next,
				int id)
{
	if (irc->prev == NULL) {
		struct isi_voicecall *ivc = ofono_voicecall_get_data(irc->ovc);

		if (ivc->queue) {
			irc->next = ivc->queue;
			irc->next->prev = &irc->next;
		}
		irc->prev = &ivc->queue;
		ivc->queue = irc;
	}

	irc->step = next;
	irc->id = id;
}

static void isi_ctx_remove(struct isi_call_req_context *irc)
{
	if (irc->prev) {
		*irc->prev = irc->next;

		if (irc->next) {
			irc->next->prev = irc->prev;
			irc->next = NULL;
		}
		irc->prev = NULL;
	}
}

static void isi_ctx_free(struct isi_call_req_context *irc)
{
	if (irc) {
		isi_ctx_remove(irc);
		g_free(irc);
	}
}

static gboolean isi_ctx_return(struct isi_call_req_context *irc,
				enum ofono_error_type type,
				int error)
{
	if (irc == NULL)
		return TRUE;

	if (irc->cb) {
		struct ofono_error e = { .type = type, .error = error };
		irc->cb(&e, irc->data);
	}

	isi_ctx_free(irc);

	return TRUE;
}

static gboolean isi_ctx_return_failure(struct isi_call_req_context *irc)
{
	return isi_ctx_return(irc, OFONO_ERROR_TYPE_FAILURE, 0);
}

static gboolean isi_ctx_return_success(struct isi_call_req_context *irc)
{
	if (irc && irc->step) {
		irc->step(irc, 0, 0);
		return TRUE;
	}

	return isi_ctx_return(irc, OFONO_ERROR_TYPE_NO_ERROR, 0);
}

/* ------------------------------------------------------------------------- */
/* Notify */

enum clcc_status {
	CLCC_STATUS_EARLY		= -1,
	CLCC_STATUS_ACTIVE		= 0,
	CLCC_STATUS_HOLD		= 1,
	CLCC_STATUS_DIALING		= 2,
	CLCC_STATUS_ALERTING		= 3,
	CLCC_STATUS_INCOMING		= 4,
	CLCC_STATUS_WAITING		= 5,
	CLCC_STATUS_DISCONNECTED	= 6,
};

/** Get +CLCC status */
static int isi_call_status_to_clcc(struct isi_voicecall const *ivc,
					struct isi_call const *call)
{
	switch (call->status) {
	case CALL_STATUS_CREATE:
		return CLCC_STATUS_DIALING;

	case CALL_STATUS_COMING:
		return CLCC_STATUS_EARLY;

	case CALL_STATUS_PROCEEDING:
		if ((call->mode_info & CALL_MODE_ORIGINATOR))
			return CLCC_STATUS_EARLY; /* MT */
		else
			return CLCC_STATUS_DIALING; /* MO */

	case CALL_STATUS_MO_ALERTING:
		return CLCC_STATUS_ALERTING;

	case CALL_STATUS_MT_ALERTING:
		return CLCC_STATUS_INCOMING;

	case CALL_STATUS_WAITING:
		return CLCC_STATUS_WAITING;

	case CALL_STATUS_ANSWERED:
	case CALL_STATUS_ACTIVE:
	case CALL_STATUS_HOLD_INITIATED:
	case CALL_STATUS_RECONNECT_PENDING:
	case CALL_STATUS_SWAP_INITIATED:
		return CLCC_STATUS_ACTIVE;

	case CALL_STATUS_HOLD:
	case CALL_STATUS_RETRIEVE_INITIATED:
		return CLCC_STATUS_HOLD;

	case CALL_STATUS_MO_RELEASE:
	case CALL_STATUS_MT_RELEASE:
	case CALL_STATUS_TERMINATED:
	case CALL_STATUS_IDLE:
		return CLCC_STATUS_DISCONNECTED;
	}

	return CLCC_STATUS_ACTIVE;
}

static struct ofono_call isi_call_as_ofono_call(struct isi_voicecall const *ivc,
						struct isi_call const *call)
{
	struct ofono_call ocall = { call->id };
	struct ofono_phone_number *number = &ocall.phone_number;

	ocall.type = 0;	/* Voice call */
	ocall.direction = call->mode_info & CALL_MODE_ORIGINATOR;
	ocall.status = isi_call_status_to_clcc(ivc, call);
	memcpy(number->number, call->address, sizeof(number->number));
	number->type = 0x80 | call->addr_type;
	ocall.clip_validity = call->presentation & 3;

	if (ocall.clip_validity == 0 && strlen(number->number) == 0)
		ocall.clip_validity = 2;

	return ocall;
}

static struct isi_call *isi_call_set_idle(struct isi_call *call)
{
	uint8_t id;

	id = call->id;
	memset(call, 0, sizeof(*call));
	call->id = id;

	return call;
}

static void isi_call_disconnected(struct ofono_voicecall *ovc,
					struct isi_call *call)
{
	struct ofono_error error = { OFONO_ERROR_TYPE_NO_ERROR, 0 };
	enum ofono_disconnect_reason reason = call->reason;

	if (!reason)
		reason = OFONO_DISCONNECT_REASON_ERROR;

	DBG("disconnected id=%u reason=%u", call->id, reason);
	ofono_voicecall_disconnected(ovc, call->id, reason, &error);
	isi_call_set_idle(call);
}

static void isi_call_notify(struct ofono_voicecall *ovc,
				struct isi_call *call)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct isi_call_req_context *irc, **queue;
	struct ofono_call ocall;

	DBG("called with status=%s (0x%02X)",
		call_status_name(call->status), call->status);

	for (queue = &ivc->queue; (irc = *queue);) {
		irc->step(irc, call->id, call->status);

		if (*queue == irc)
			queue = &irc->next;
	}

	ocall = isi_call_as_ofono_call(ivc, call);

	DBG("id=%u,\"%s\",%u,\"%s\",%u,%u",
		ocall.id,
		ocall.direction ? "mt" : "mo",
		ocall.status,
		ocall.phone_number.number,
		ocall.phone_number.type,
		ocall.clip_validity);

	if (ocall.status == CLCC_STATUS_EARLY)
		return;

	ofono_voicecall_notify(ovc, &ocall);

	switch (call->status) {
	case CALL_STATUS_MO_RELEASE:
		call->reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
		break;

	case CALL_STATUS_MT_RELEASE:
		call->reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
		break;

	case CALL_STATUS_IDLE:
	case CALL_STATUS_TERMINATED:
		isi_call_disconnected(ovc, call);
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Decoding subblocks */

static void isi_call_any_address_sb_proc(struct isi_voicecall *ivc,
						struct isi_call *call,
						GIsiSubBlockIter const *sb)
{
	uint8_t addr_type, presentation, addr_len;
	char *address;

	if (!g_isi_sb_iter_get_byte(sb, &addr_type, 2) ||
		!g_isi_sb_iter_get_byte(sb, &presentation, 3) ||
		/* fillerbyte */
		!g_isi_sb_iter_get_byte(sb, &addr_len, 5) ||
		!g_isi_sb_iter_get_alpha_tag(sb, &address, 2 * addr_len, 6))
		return;

	call->addr_type = addr_type | 0x80;
	call->presentation = presentation;
	strncpy(call->address, address, sizeof call->address);

	g_free(address);
}

static void isi_call_origin_address_sb_proc(struct isi_voicecall *ivc,
						struct isi_call *call,
						GIsiSubBlockIter const *sb)
{
	if (!call->address[0])
		isi_call_any_address_sb_proc(ivc, call, sb);
}

static void isi_call_destination_address_sb_proc(struct isi_voicecall *ivc,
						struct isi_call *call,
						GIsiSubBlockIter const *sb)
{
	if (!call->address[0])
		isi_call_any_address_sb_proc(ivc, call, sb);
}

static void isi_call_mode_sb_proc(struct isi_voicecall *ivc,
					struct isi_call *call,
					GIsiSubBlockIter const *sb)
{
	uint8_t mode, mode_info;

	if (!g_isi_sb_iter_get_byte(sb, &mode, 2) ||
			!g_isi_sb_iter_get_byte(sb, &mode_info, 3))
		return;

	call->mode = mode;
	call->mode_info = mode_info;
}

static void isi_call_cause_sb_proc(struct isi_voicecall *ivc,
					struct isi_call *call,
					GIsiSubBlockIter const *sb)
{
	uint8_t cause_type, cause;

	if (!g_isi_sb_iter_get_byte(sb, &cause_type, 2) ||
			!g_isi_sb_iter_get_byte(sb, &cause, 3))
		return;

	call->cause_type = cause_type;
	call->cause = cause;
}

static void isi_call_status_sb_proc(struct isi_voicecall *ivc,
					struct isi_call *call,
					GIsiSubBlockIter const *sb)
{
	uint8_t status;

	if (!g_isi_sb_iter_get_byte(sb, &status, 2))
		return;

	call->status = status;
}

static struct isi_call *
isi_call_status_info_sb_proc(struct isi_voicecall *ivc,
				GIsiSubBlockIter const *sb)
{
	struct isi_call *call = NULL;
	int i;
	uint8_t call_id;
	uint8_t mode;
	uint8_t mode_info;
	uint8_t status;

	if (!g_isi_sb_iter_get_byte(sb, &call_id, 2) ||
			!g_isi_sb_iter_get_byte(sb, &mode, 3) ||
			!g_isi_sb_iter_get_byte(sb, &mode_info, 4) ||
			!g_isi_sb_iter_get_byte(sb, &status, 5))
		return NULL;

	i = call_id & 7;

	if (1 <= i && i <= 7) {
		call = &ivc->calls[i];
		call->call_id = call_id;
		call->status = status;
		call->mode = mode;
		call->mode_info = mode_info;
	}

	return call;
}

static struct isi_call *
isi_call_addr_and_status_info_sb_proc(struct isi_voicecall *ivc,
					GIsiSubBlockIter const *sb)
{
	struct isi_call *call = NULL;
	int i;
	uint8_t call_id;
	uint8_t mode;
	uint8_t mode_info;
	uint8_t status;
	uint8_t addr_type;
	uint8_t presentation;
	uint8_t addr_len;
	char *address;

	if (!g_isi_sb_iter_get_byte(sb, &call_id, 2) ||
		!g_isi_sb_iter_get_byte(sb, &mode, 3) ||
		!g_isi_sb_iter_get_byte(sb, &mode_info, 4) ||
		!g_isi_sb_iter_get_byte(sb, &status, 5) ||
		!g_isi_sb_iter_get_byte(sb, &addr_type, 8) ||
		!g_isi_sb_iter_get_byte(sb, &presentation, 9) ||
		!g_isi_sb_iter_get_byte(sb, &addr_len, 11) ||
		!g_isi_sb_iter_get_alpha_tag(sb, &address, 2 * addr_len, 12))
		return NULL;

	i = call_id & 7;

	if (1 <= i && i <= 7) {
		call = &ivc->calls[i];
		call->call_id = call_id;
		call->status = status;
		call->mode = mode;
		call->mode_info = mode_info;
		call->addr_type = addr_type | 0x80;
		call->presentation = presentation;
		strncpy(call->address, address, sizeof call->address);
	}

	free(address);

	return call;
}

/* ------------------------------------------------------------------------- */
/* PN_CALL messages */

static GIsiResponse isi_call_status_resp,
	isi_call_create_resp,
	isi_call_answer_resp,
	isi_call_release_resp,
	isi_call_control_resp,
	isi_call_dtmf_send_resp;

static struct isi_call_req_context *
isi_call_create_req(struct ofono_voicecall *ovc,
			uint8_t presentation,
			uint8_t addr_type,
			char const address[21],
			ofono_voicecall_cb_t cb,
			void *data)
{
	size_t addr_len = strlen(address);
	size_t sub_len = (6 + 2 * addr_len + 3) & ~3;
	size_t i, offset = 3 + 4 + 8 + 6;
	uint8_t req[3 + 4 + 8 + 6 + 40] = {
		CALL_CREATE_REQ,
		0,		/* No id */
		3,		/* Mode, Clir, Number */
		/* MODE SB */
		CALL_MODE, 4, CALL_MODE_SPEECH, CALL_MODE_INFO_NONE,
		/* ORIGIN_INFO SB */
		CALL_ORIGIN_INFO, 8, presentation, 0, 0, 0, 0, 0,
		/* DESTINATION_ADDRESS SB */
		CALL_DESTINATION_ADDRESS,
		sub_len,
		addr_type & 0x7F,
		0, 0,
		addr_len,
		/* uint16_t addr[20] */
	};
	size_t rlen = 3 + 4 + 8 + sub_len;

	if (addr_len > 20) {
		CALLBACK_WITH_FAILURE(cb, data);
		return NULL;
	}

	for (i = 0; i < addr_len; i++)
		req[offset + 2 * i + 1] = address[i];

	return isi_call_req(ovc, req, rlen, isi_call_create_resp, cb, data);
}

static gboolean isi_call_create_resp(GIsiClient *client,
					void const *restrict data,
					size_t len,
					uint16_t object,
					void *irc)
{
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;

	if (m != NULL && len < (sizeof *m))
		return FALSE;
	if (m == NULL || m->message_id == CALL_COMMON_MESSAGE)
		return isi_ctx_return_failure(irc);
	if (m->message_id != CALL_CREATE_RESP)
		return FALSE;

	if (m->call_id != CALL_ID_NONE && m->sub_blocks == 0)
		return isi_ctx_return_success(irc);

	/* Cause ? */
	return isi_ctx_return_failure(irc);
}

static void isi_call_status_ind_cb(GIsiClient *client,
					void const *restrict data,
					size_t len,
					uint16_t object,
					void *_ovc)
{
	struct ofono_voicecall *ovc = _ovc;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;
	struct isi_call *call;
	uint8_t old;
	GIsiSubBlockIter sb[1];

	if (len < 3)
		return;		/* runt */

	if ((m->call_id & 7) == 0)
		return;

	call = &ivc->calls[m->call_id & 7];

	old = call->status;
	call->call_id = m->call_id;

	for (g_isi_sb_iter_init(sb, data, len, (sizeof *m));
			g_isi_sb_iter_is_valid(sb);
				g_isi_sb_iter_next(sb)) {
		switch (g_isi_sb_iter_get_id(sb)) {
		case CALL_STATUS:
			isi_call_status_sb_proc(ivc, call, sb);
			break;

		case CALL_MODE:
			isi_call_mode_sb_proc(ivc, call, sb);
			break;

		case CALL_CAUSE:
			isi_call_cause_sb_proc(ivc, call, sb);
			break;

		case CALL_DESTINATION_ADDRESS:
			isi_call_destination_address_sb_proc(ivc, call, sb);
			break;

		case CALL_ORIGIN_ADDRESS:
			isi_call_origin_address_sb_proc(ivc, call, sb);
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

	if (old != call->status)
		isi_call_notify(ovc, call);
}

static struct isi_call_req_context *
isi_call_answer_req(struct ofono_voicecall *ovc,
			uint8_t call_id, ofono_voicecall_cb_t cb, void *data)
{
	uint8_t const req[] = {
		CALL_ANSWER_REQ, call_id, 0
	};
	size_t rlen = sizeof req;

	return isi_call_req(ovc, req, rlen, isi_call_answer_resp, cb, data);
}

static gboolean isi_call_answer_resp(GIsiClient *client,
					void const *restrict data,
					size_t len,
					uint16_t object,
					void *irc)
{
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;

	if (m != NULL && len < (sizeof *m))
		return FALSE;
	if (m == NULL || m->message_id == CALL_COMMON_MESSAGE)
		return isi_ctx_return_failure(irc);
	if (m->message_id != CALL_ANSWER_RESP)
		return FALSE;

	if (m->call_id != CALL_ID_NONE && m->sub_blocks == 0)
		return isi_ctx_return_success(irc);

	/* Cause ? */
	return isi_ctx_return_failure(irc);
}

static struct isi_call_req_context *
isi_call_release_req(struct ofono_voicecall *ovc,
			uint8_t call_id, uint8_t cause,
			ofono_voicecall_cb_t cb, void *data)
{
	uint8_t const req[] = {
		CALL_RELEASE_REQ, call_id, 2,
		CALL_CAUSE, 4, CALL_CAUSE_TYPE_CLIENT, cause,
		CALL_STATE_AUTO_CHANGE, 4, 0, 0
	};
	size_t rlen = sizeof req;

	return isi_call_req(ovc, req, rlen, isi_call_release_resp, cb, data);
}

static gboolean isi_call_release_resp(GIsiClient *client,
					void const *restrict data,
					size_t len,
					uint16_t object,
					void *irc)
{
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;
	GIsiSubBlockIter i[1];
	uint8_t cause_type = 0, cause = 0;

	if (m != NULL && len < (sizeof *m))
		return FALSE;
	if (m == NULL || m->message_id == CALL_COMMON_MESSAGE)
		return isi_ctx_return_failure(irc);
	if (m->message_id != CALL_RELEASE_RESP)
		return FALSE;

	for (g_isi_sb_iter_init(i, m, len, (sizeof *m));
			g_isi_sb_iter_is_valid(i);
				g_isi_sb_iter_next(i)) {
		if (g_isi_sb_iter_get_id(i) != CALL_CAUSE ||
				!g_isi_sb_iter_get_byte(i, &cause_type, 2) ||
				!g_isi_sb_iter_get_byte(i, &cause, 3))
			continue;
	}

	if ((cause_type == CALL_CAUSE_TYPE_SERVER ||
			cause_type == CALL_CAUSE_TYPE_CLIENT) &&
			(cause == CALL_CAUSE_RELEASE_BY_USER ||
			cause == CALL_CAUSE_BUSY_USER_REQUEST))
		return isi_ctx_return_success(irc);
	else
		return isi_ctx_return_failure(irc);
}

static struct isi_call_req_context *
isi_call_status_req(struct ofono_voicecall *ovc,
			uint8_t id, uint8_t mode,
			ofono_voicecall_cb_t cb, void *data)
{
	unsigned char req[] = {
		CALL_STATUS_REQ, id, 1,
		CALL_STATUS_MODE, 4, mode, 0,
	};
	size_t rlen = sizeof req;

	return isi_call_req(ovc, req, rlen, isi_call_status_resp, cb, data);
}


static gboolean isi_call_status_resp(GIsiClient *client,
					void const *restrict data,
					size_t len,
					uint16_t object,
					void *_irc)
{
	struct isi_call_req_context *irc = _irc;
	struct ofono_voicecall *ovc = irc->ovc;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;
	GIsiSubBlockIter sb[1];
	struct isi_call *call = NULL;

	if (m != NULL && len < (sizeof *m))
		return FALSE;
	if (m == NULL || m->message_id == CALL_COMMON_MESSAGE)
		return isi_ctx_return_failure(irc);
	if (m->message_id != CALL_STATUS_RESP)
		return FALSE;

	for (g_isi_sb_iter_init(sb, m, len, (sizeof *m));
			g_isi_sb_iter_is_valid(sb);
				g_isi_sb_iter_next(sb)) {
		switch (g_isi_sb_iter_get_id(sb)) {

		case CALL_STATUS_INFO:
			call = isi_call_status_info_sb_proc(ivc, sb);
			break;

		case CALL_ADDR_AND_STATUS_INFO:
			call = isi_call_addr_and_status_info_sb_proc(ivc, sb);
			if (call)
				isi_call_notify(ovc, call);
			break;

		case CALL_CAUSE:
			if (call)
				isi_call_cause_sb_proc(ivc, call, sb);
			break;
		}
	}

	return isi_ctx_return_success(irc);
}

static struct isi_call_req_context *
isi_call_control_req(struct ofono_voicecall *ovc,
			uint8_t call_id, enum call_operation op, uint8_t info,
			ofono_voicecall_cb_t cb, void *data)
{
	uint8_t const req[] = {
		CALL_CONTROL_REQ, call_id, 1,
		CALL_OPERATION, 4, op, info,
	};
	size_t rlen = sizeof req;

	return isi_call_req(ovc, req, rlen, isi_call_control_resp, cb, data);
}

static struct isi_call_req_context *
isi_call_deflect_req(struct ofono_voicecall *ovc,
			uint8_t call_id, uint8_t address_type,
			char const address[21],
			ofono_voicecall_cb_t cb, void *data)
{
	size_t addr_len = strlen(address);
	size_t sub_len = (6 + 2 * addr_len + 3) & ~3;
	size_t i, offset = 3 + 4 + 6;
	size_t rlen = 3 + 4 + sub_len;
	uint8_t req[3 + 4 + 6 + 40] = {
		CALL_CONTROL_REQ, call_id, 2,
		CALL_OPERATION, 4, CALL_GSM_OP_DEFLECT, 0,
		CALL_GSM_DEFLECTION_ADDRESS, sub_len,
		address_type & 0x7F,
		0x7,		/* default presentation */
		0,		/* filler */
		addr_len,
	};

	if (addr_len > 20) {
		CALLBACK_WITH_FAILURE(cb, data);
		return FALSE;
	}

	for (i = 0; i < addr_len; i++)
		req[offset + 2 * i + 1] = address[i];

	return isi_call_req(ovc, req, rlen, isi_call_control_resp, cb, data);
}

static gboolean isi_call_control_resp(GIsiClient *client,
					void const *restrict data,
					size_t len,
					uint16_t object,
					void *irc)
{
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;
	GIsiSubBlockIter i[1];
	uint8_t cause_type = 0, cause = 0;

	if (m != NULL && len < sizeof *m)
		return FALSE;
	if (m == NULL || m->message_id == CALL_COMMON_MESSAGE)
		return isi_ctx_return_failure(irc);
	if (m->message_id != CALL_CONTROL_RESP)
		return FALSE;

	for (g_isi_sb_iter_init(i, m, len, (sizeof *m));
			g_isi_sb_iter_is_valid(i);
				g_isi_sb_iter_next(i)) {
		if (g_isi_sb_iter_get_id(i) != CALL_CAUSE ||
				!g_isi_sb_iter_get_byte(i, &cause_type, 2) ||
				!g_isi_sb_iter_get_byte(i, &cause, 3))
			continue;
	}

	if (!cause)
		return isi_ctx_return_success(irc);
	else
		return isi_ctx_return_failure(irc);
}

static struct isi_call_req_context *
isi_call_dtmf_send_req(struct ofono_voicecall *ovc,
			uint8_t call_id, char const *string,
			ofono_voicecall_cb_t cb, void *data)
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

static gboolean isi_call_dtmf_send_resp(GIsiClient *client,
					void const *restrict data,
					size_t len,
					uint16_t object,
					void *irc)
{
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;
	GIsiSubBlockIter i[1];
	uint8_t cause_type = 0, cause = 0;

	if (m != NULL && len < (sizeof *m))
		return FALSE;
	if (m == NULL || m->message_id == CALL_COMMON_MESSAGE)
		return isi_ctx_return_failure(irc);
	if (m->message_id != CALL_DTMF_SEND_RESP)
		return FALSE;

	if (m->sub_blocks == 0)
		return isi_ctx_return_success(irc);

	for (g_isi_sb_iter_init(i, m, len, (sizeof *m));
			g_isi_sb_iter_is_valid(i);
				g_isi_sb_iter_next(i)) {
		if (g_isi_sb_iter_get_id(i) != CALL_CAUSE ||
				!g_isi_sb_iter_get_byte(i, &cause_type, 2) ||
				!g_isi_sb_iter_get_byte(i, &cause, 3))
			continue;
	}

	if (!cause)
		return isi_ctx_return_success(irc);
	else
		return isi_ctx_return_failure(irc);
}

/* ---------------------------------------------------------------------- */

static void isi_dial(struct ofono_voicecall *ovc,
			const struct ofono_phone_number *restrict number,
			enum ofono_clir_option clir,
			enum ofono_cug_option cug,
			ofono_voicecall_cb_t cb, void *data)
{
	unsigned char presentation = CALL_GSM_PRESENTATION_DEFAULT;

	switch (clir) {
	case OFONO_CLIR_OPTION_DEFAULT:
		presentation = CALL_GSM_PRESENTATION_DEFAULT;
		break;
	case OFONO_CLIR_OPTION_INVOCATION:
		presentation = CALL_PRESENTATION_RESTRICTED;
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		presentation = CALL_PRESENTATION_ALLOWED;
		break;
	}

	switch (cug) {
	case OFONO_CUG_OPTION_DEFAULT:
		break;
	case OFONO_CUG_OPTION_INVOCATION:
		/* Not implemented */
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	isi_call_create_req(ovc, presentation, number->type, number->number,
				cb, data);
}

static void isi_answer(struct ofono_voicecall *ovc,
			ofono_voicecall_cb_t cb, void *data)
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
	int id;
	uint8_t cause = CALL_CAUSE_RELEASE_BY_USER;

	for (id = 1; id <= 7; id++) {
		if (ivc->calls[id].call_id & CALL_ID_WAITING)
			continue;
		if (ivc->calls[id].call_id & CALL_ID_HOLD)
			continue;

		switch (ivc->calls[id].status) {
		case CALL_STATUS_CREATE:
		case CALL_STATUS_MT_ALERTING:
		case CALL_STATUS_ANSWERED:
			goto release_by_id;

		case CALL_STATUS_COMING:
		case CALL_STATUS_PROCEEDING:
		case CALL_STATUS_MO_ALERTING:
			cause = CALL_CAUSE_BUSY_USER_REQUEST;
			goto release_by_id;
		}
	}

	id = CALL_ID_ACTIVE;

release_by_id:
	isi_call_release_req(ovc, id, cause, cb, data);
}

static void isi_release_all_held(struct ofono_voicecall *ovc,
				ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CHLD=0 (w/out incoming calls) */
	isi_call_release_req(ovc, CALL_ID_HOLD,
				CALL_CAUSE_RELEASE_BY_USER, cb, data);
}

static void isi_set_udub(struct ofono_voicecall *ovc,
				ofono_voicecall_cb_t cb, void *data)
{
	/* Release waiting calls */
	isi_call_release_req(ovc, CALL_ID_WAITING,
				CALL_CAUSE_BUSY_USER_REQUEST, cb, data);
}

static void isi_retrieve(struct ofono_voicecall *ovc,
				ofono_voicecall_cb_t cb, void *data)
{
	isi_call_control_req(ovc, CALL_ID_HOLD, CALL_OP_RETRIEVE, 0, cb, data);
}

static void isi_wait_and_answer(struct isi_call_req_context *irc,
				int id, int status)
{
	DBG("irc=%p id=%d status=%d", (void *)irc, id, status);

	if (id != irc->id)
		return;

	switch (status) {
	case CALL_STATUS_MT_ALERTING:
		isi_call_answer_req(irc->ovc, irc->id, irc->cb, irc->data);
		isi_ctx_free(irc);
		break;

	default:
		isi_ctx_return_failure(irc);
		break;
	}
}

static void isi_release_all_active(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CHLD=1 */
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	struct isi_call_req_context *irc;
	int id;
	int waiting_id = 0;
	int active = 0;

	for (id = 1; id <= 7; id++) {
		if (ivc->calls[id].call_id & CALL_ID_WAITING)
			waiting_id = id;

		if (ivc->calls[id].call_id & CALL_ID_ACTIVE)
			active++;
	}

	if (!active) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	irc = isi_call_release_req(ovc, CALL_ID_ACTIVE,
					CALL_CAUSE_RELEASE_BY_USER,
					cb, data);
	if (irc == NULL)
		return;

	if (waiting_id)
		isi_ctx_queue(irc, isi_wait_and_answer, waiting_id);

	/* Retrieving held calls is currently a unwanted side-effect */
}

static void isi_hold_all_active(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CHLD=2 */
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	int id;
	int waiting = 0;
	int active = 0;
	int hold = 0;
	int op;

	for (id = 1; id <= 7; id++) {
		if (ivc->calls[id].call_id & CALL_ID_WAITING)
			waiting++;
		else if (ivc->calls[id].call_id & CALL_ID_HOLD)
			hold++;
		else if (ivc->calls[id].call_id & CALL_ID_ACTIVE)
			active++;
	}

	if (waiting) {
		isi_call_answer_req(ovc, CALL_ID_WAITING, cb, data);
		return;
	}

	if (active) {
		if (hold)
			op = CALL_OP_SWAP;
		else
			op = CALL_OP_HOLD;

		isi_call_control_req(ovc, CALL_ID_ACTIVE, op, 0, cb, data);

	} else if (hold)
		isi_retrieve(ovc, cb, data);
	else
		CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_release_specific(struct ofono_voicecall *ovc, int id,
					ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CHLD=1X */
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);

	if (1 <= id && id <= 7) {
		struct isi_call const *status = &ivc->calls[id];
		uint8_t cause = CALL_CAUSE_RELEASE_BY_USER;

		switch (status->status) {
		case CALL_STATUS_COMING:
		case CALL_STATUS_MT_ALERTING:
		case CALL_STATUS_WAITING:
			cause = CALL_CAUSE_BUSY_USER_REQUEST;
			break;
		case CALL_STATUS_PROCEEDING:
			if ((status->mode_info & CALL_MODE_ORIGINATOR))
				cause = CALL_CAUSE_BUSY_USER_REQUEST;
			break;
		}

		isi_call_release_req(ovc, id, cause, cb, data);
	} else
		CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_private_chat(struct ofono_voicecall *ovc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CHLD=2X */
	if (1 <= id && id <= 7)
		isi_call_control_req(ovc, id, CALL_OP_CONFERENCE_SPLIT, 0,
					cb, data);
	else
		CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_create_multiparty(struct ofono_voicecall *ovc,
					ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CHLD=3 */
	isi_call_control_req(ovc, CALL_ID_ALL, CALL_OP_CONFERENCE_BUILD, 0,
				cb, data);
}

static void isi_transfer(struct ofono_voicecall *ovc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CHLD=4 */
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);

	uint8_t id;

	for (id = 1; id <= 7; id++)
		if (ivc->calls[id].status == CALL_STATUS_MO_ALERTING)
			break;
	if (id > 7)
		id = CALL_ID_ACTIVE;

	isi_call_control_req(ovc, id, CALL_GSM_OP_TRANSFER, 0, cb, data);
}

static void isi_deflect(struct ofono_voicecall *ovc,
			const struct ofono_phone_number *ph,
			ofono_voicecall_cb_t cb, void *data)
{
	/* AT+CTFR=<number>,<type> */
	int id = CALL_ID_WAITING;
	isi_call_deflect_req(ovc, id, ph->type, ph->number, cb, data);
}

static void isi_swap_without_accept(struct ofono_voicecall *ovc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	int id = 0, op = 0, active = 0, hold = 0;

	for (id = 1; id <= 7; id++) {
		if (ivc->calls[id].call_id & CALL_ID_HOLD)
			hold++;
		if (ivc->calls[id].call_id & CALL_ID_ACTIVE)
			active++;
	}

	if (hold && active) {
		id = CALL_ID_ACTIVE, op = CALL_OP_SWAP;
	} else if (active) {
		id = CALL_ID_ACTIVE, op = CALL_OP_HOLD;
	} else if (hold) {
		id = CALL_ID_HOLD, op = CALL_OP_RETRIEVE;
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

static gboolean isi_call_register(gpointer _ovc)
{
	struct ofono_voicecall *ovc = _ovc;
	struct isi_voicecall *ivc = ofono_voicecall_get_data(ovc);
	const char *debug = getenv("OFONO_ISI_DEBUG");

	if (debug != NULL && (!strcmp(debug, "all") || !strcmp(debug, "call")))
		g_isi_client_set_debug(ivc->client, call_debug, NULL);

	g_isi_subscribe(ivc->client,
			CALL_STATUS_IND, isi_call_status_ind_cb,
			ovc);

	if (isi_call_status_req(ovc, CALL_ID_ALL,
					CALL_STATUS_MODE_ADDR_AND_ORIGIN,
					NULL, NULL) == NULL)
		DBG("Failed to request call status");

	ofono_voicecall_register(ovc);

	return FALSE;
}

static void isi_call_verify_cb(GIsiClient *client,
				gboolean alive, uint16_t object, void *ovc)
{
	if (!alive) {
		DBG("Unable to bootstrap voice call driver");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_idle_add(isi_call_register, ovc);
}

static int isi_voicecall_probe(struct ofono_voicecall *ovc,
				unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct isi_voicecall *ivc;
	int id;

	ivc = g_try_new0(struct isi_voicecall, 1);
	if (ivc == NULL)
		return -ENOMEM;

	for (id = 1; id <= 7; id++)
		ivc->calls[id].id = id;

	ivc->client = g_isi_client_create(idx, PN_CALL);
	if (ivc->client == NULL) {
		g_free(ivc);
		return -ENOMEM;
	}

	ofono_voicecall_set_data(ovc, ivc);

	if (g_isi_verify(ivc->client, isi_call_verify_cb, ovc) == NULL)
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_voicecall_remove(struct ofono_voicecall *call)
{
	struct isi_voicecall *data = ofono_voicecall_get_data(call);

	if (data == NULL)
		return;

	ofono_voicecall_set_data(call, NULL);
	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_voicecall_driver driver = {
	.name			= "isimodem",
	.probe			= isi_voicecall_probe,
	.remove			= isi_voicecall_remove,
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

void isi_voicecall_init()
{
	ofono_voicecall_driver_register(&driver);
}

void isi_voicecall_exit()
{
	ofono_voicecall_driver_unregister(&driver);
}
