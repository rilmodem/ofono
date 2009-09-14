/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
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
#include <arpa/inet.h>

#include <glib.h>

#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "isi.h"

#define PN_NETWORK		0x0A
#define NETWORK_TIMEOUT		5
#define NETWORK_SCAN_TIMEOUT	60
#define NETWORK_SET_TIMEOUT	100

/* Used with COMMON_MESSAGE */
enum sub_message_id {
	COMM_ISI_VERSION_GET_REQ = 0x12,
	COMM_ISI_VERSION_GET_RESP = 0x13,
	COMM_ISA_ENTITY_NOT_REACHABLE_RESP = 0x14
};

enum message_id {
	NET_SET_REQ = 0x07,
	NET_SET_RESP = 0x08,
	NET_RSSI_GET_REQ = 0x0B,
	NET_RSSI_GET_RESP = 0x0C,
	NET_RSSI_IND = 0x1E,
	NET_RAT_IND = 0x35,
	NET_RAT_REQ = 0x36,
	NET_RAT_RESP = 0x37,
	NET_REG_STATUS_GET_REQ = 0xE0,
	NET_REG_STATUS_GET_RESP = 0xE1,
	NET_REG_STATUS_IND = 0xE2,
	NET_AVAILABLE_GET_REQ = 0xE3,
	NET_AVAILABLE_GET_RESP = 0xE4,
	NET_OPER_NAME_READ_REQ = 0xE5,
	NET_OPER_NAME_READ_RESP = 0xE6,
	COMMON_MESSAGE = 0xF0
};

enum sub_block_id {
	NET_REG_INFO_COMMON = 0x00,
	NET_OPERATOR_INFO_COMMON = 0x02,
	NET_RSSI_CURRENT = 0x04,
 	NET_GSM_REG_INFO = 0x09,
 	NET_DETAILED_NETWORK_INFO = 0x0B,
	NET_GSM_OPERATOR_INFO = 0x0C,
	NET_GSM_BAND_INFO = 0x11,
	NET_RAT_INFO = 0x2C,
	NET_AVAIL_NETWORK_INFO_COMMON = 0xE1,
	NET_OPER_NAME_INFO = 0xE7
};

enum reg_status {
	NET_REG_STATUS_HOME = 0x00,
	NET_REG_STATUS_ROAM = 0x01,
	NET_REG_STATUS_NOSERV = 0x03,
	NET_REG_STATUS_NOSERV_SEARCHING = 0x04,
	NET_REG_STATUS_NOSERV_NOTSEARCHING = 0x05,
	NET_REG_STATUS_NOSERV_NOSIM = 0x06,
	NET_REG_STATUS_POWER_OFF = 0x08,
	NET_REG_STATUS_NSPS = 0x09,
	NET_REG_STATUS_NSPS_NO_COVERAGE = 0x0A,
	NET_REG_STATUS_NOSERV_SIM_REJECTED_BY_NW = 0x0B
};

enum cs_type {
	NET_CS_GSM = 0x00
};

enum rat_name {
	NET_GSM_RAT = 0x01,
	NET_UMTS_RAT = 0x02
};

enum rat_type {
	NET_CURRENT_RAT = 0x00,
	NET_SUPPORTED_RATS = 0x01
};

enum measurement_type {
	NET_CURRENT_CELL_RSSI = 0x02
};

enum search_mode {
	NET_MANUAL_SEARCH = 0x00
};

enum oper_name_type {
	NET_HARDCODED_LATIN_OPER_NAME = 0x00
};

enum band_info {
	NET_GSM_BAND_INFO_NOT_AVAIL = 0x02,
	NET_GSM_BAND_ALL_SUPPORTED_BANDS = 0x03
};

enum select_mode {
	NET_SELECT_MODE_UNKNOWN = 0x00,
	NET_SELECT_MODE_MANUAL = 0x01,
	NET_SELECT_MODE_AUTOMATIC = 0x02,
	NET_SELECT_MODE_USER_RESELECTION = 0x03,
	NET_SELECT_MODE_NO_SELECTION = 0x04
};

enum return_code {
	NET_CAUSE_OK = 0x00,
	NET_CAUSE_COMMUNICATION_ERROR = 0x01,
	NET_CAUSE_NET_NOT_FOUND = 0x05,
	NET_CAUSE_NO_SELECTED_NETWORK = 0x11
};

struct netreg_data {
	GIsiClient *client;
	struct isi_version version;

	guint8 last_reg_mode;
	guint8 rat;
	guint8 gsm_compact;
};

struct isi_sb_iter {
	guint8 *start;
	guint8 *end;
};

static inline unsigned char *mccmnc_to_bcd(const char *mcc, const char *mnc,
						unsigned char *bcd)
{
	bcd[0] = (mcc[0] - '0') | (mcc[1] - '0') << 4;
	bcd[1] = (mcc[2] - '0');
	bcd[1] |= (mnc[2] == '\0' ? 0x0f : (mnc[2] - '0')) << 4;
	bcd[2] = (mnc[0] - '0') | (mnc[1] - '0') << 4;
	return bcd;
}

static inline void bcd_to_mccmnc(const unsigned char *bcd, char *mcc,
					char *mnc)
{
	mcc[0] = '0' + (bcd[0] & 0x0f);
	mcc[1] = '0' + ((bcd[0] & 0xf0) >> 4);
	mcc[2] = '0' + (bcd[1] & 0x0f);
	mcc[3] = '\0';

	mnc[0] = '0' + (bcd[2] & 0x0f);
	mnc[1] = '0' + ((bcd[2] & 0xf0) >> 4);
	mnc[2] = (bcd[1] & 0xf0) == 0xf0 ? '\0' : '0' +
			(bcd[1] & 0xf0);
	mnc[3] = '\0';
}

static gboolean isi_sb_iter_is_valid(struct isi_sb_iter *iter)
{
	if (!iter || iter->end - iter->start < 2)
		return FALSE;

	if (iter->start + iter->start[1] > iter->end)
		return FALSE;

	return TRUE;
}

static guint8 isi_sb_iter_get_id(struct isi_sb_iter *iter)
{
	return iter->start[0];
}

static guint8 isi_sb_iter_get_len(struct isi_sb_iter *iter)
{
	return iter->start[1];
}

static gboolean isi_sb_iter_get_byte(struct isi_sb_iter *iter, guint8 *byte,
					int pos)
{
	if (pos > iter->start[1] || iter->start + pos > iter->end)
		return FALSE;

	*byte = iter->start[pos];
	return TRUE;
}

static gboolean isi_sb_iter_get_word(struct isi_sb_iter *iter, guint16 *word,
					int pos)
{
	guint16 val;

	if (pos + 1 > iter->start[1])
		return FALSE;

	memcpy(&val, iter->start + pos, sizeof(guint16));
	*word = ntohs(val);
	return TRUE;
}

static gboolean isi_sb_iter_get_dword(struct isi_sb_iter *iter, guint32 *dword,
					int pos)
{
	guint32 val;

	if (pos + 3 > iter->start[1])
		return FALSE;

	memcpy(&val, iter->start + pos, sizeof(guint32));
	*dword = ntohl(val);
	return TRUE;
}

static gboolean isi_sb_iter_get_oper_code(struct isi_sb_iter *iter, char *mcc,
						char *mnc, int pos)
{
	if (pos + 2 > iter->start[1])
		return FALSE;

	bcd_to_mccmnc(iter->start + pos, mcc, mnc);
	return TRUE;
}

static gboolean isi_sb_iter_get_alpha_tag(struct isi_sb_iter *iter, char **utf8,
						int pos)
{
	guint8 *ucs2 = NULL;
	int len = 0;

	if (pos > iter->start[1])
		return FALSE;

	len = iter->start[pos] * 2; /* UCS-2 alpha tag */

	if (!utf8 || len == 0 || pos + 1 + len > iter->start[1])
		return FALSE;

	ucs2 = iter->start + pos + 1;
	*utf8 = g_convert((const char *)ucs2, len, "UTF-8", "UCS-2BE",
				NULL, NULL, NULL);
	return utf8 != NULL;
}

static gboolean isi_sb_iter_init(const guint8 *data, size_t len,
					struct isi_sb_iter *iter)
{
	if (!iter || !data || len == 0)
		return FALSE;

	iter->start = (guint8 *)data;
	iter->end = iter->start + len;

	return TRUE;
}

static gboolean isi_sb_iter_next(struct isi_sb_iter *iter)
{
	guint8 len = iter->start[1] == 0 ? 2 : iter->start[1];

	if (iter->start + len > iter->end)
		return FALSE;

	iter->start += len;
	return TRUE;
}

static void net_debug(const void *restrict buf, size_t len, void *data)
{
	DBG("");
	dump_msg(buf, len);
}

static inline int isi_status_to_at_status(guint8 status)
{
	switch (status) {

	case NET_REG_STATUS_NOSERV:
	case NET_REG_STATUS_NOSERV_NOTSEARCHING:
	case NET_REG_STATUS_NOSERV_NOSIM:
	case NET_REG_STATUS_POWER_OFF:
	case NET_REG_STATUS_NSPS:
	case NET_REG_STATUS_NSPS_NO_COVERAGE:
		return 0;

	case NET_REG_STATUS_HOME:
		return 1;

	case NET_REG_STATUS_NOSERV_SEARCHING:
		return 2;

	case NET_REG_STATUS_NOSERV_SIM_REJECTED_BY_NW:
		return 3;

	case NET_REG_STATUS_ROAM:
		return 5;

	default:
		return 4;
	}
}

static gboolean decode_reg_status(struct netreg_data *nd, const guint8 *msg,
					size_t len, int *status, int *lac,
					int *ci, int *tech)
{
	struct isi_sb_iter iter;

	if (!isi_sb_iter_init(msg, len, &iter))
		return FALSE;

	while (isi_sb_iter_is_valid(&iter)) {

		switch (isi_sb_iter_get_id(&iter)) {

		case NET_REG_INFO_COMMON: {

			guint8 byte = 0;

			if (isi_sb_iter_get_len(&iter) < 12)
				return FALSE;

			if (!isi_sb_iter_get_byte(&iter, &byte, 2))
				return FALSE;

			if (!isi_sb_iter_get_byte(&iter, &nd->last_reg_mode, 3))
				return FALSE;

			*status = byte;

			/* FIXME: decode alpha tag */
			break;
		}

		case NET_GSM_REG_INFO: {

			guint16 word = 0;
			guint32 dword = 0;
			guint8 egprs = 0;
			guint8 hsdpa = 0;
			guint8 hsupa = 0;

			if (isi_sb_iter_get_len(&iter) != 24)
				return FALSE;

			if (!isi_sb_iter_get_word(&iter, &word, 2) ||
				!isi_sb_iter_get_dword(&iter, &dword, 4) ||
				!isi_sb_iter_get_byte(&iter, &egprs, 17) ||
				!isi_sb_iter_get_byte(&iter, &hsdpa, 20) ||
				!isi_sb_iter_get_byte(&iter, &hsupa, 21))
				return FALSE;

			*ci = word;
			*lac = dword;

			switch (nd->rat) {

			case NET_GSM_RAT:

				*tech = 0;
				if (nd->gsm_compact)
					*tech = 1;
				else if (egprs)
					*tech = 3;
				break;

			case NET_UMTS_RAT:

				*tech = 2;
				if (hsdpa)
					*tech = 4;
				if (hsupa)
					*tech = 5;
				if (hsdpa && hsupa)
					*tech = 6;
				break;

			default:
				*tech = 0;
			}

			break;
		}

		default:
			DBG("Skipping sub-block: 0x%02X (%d bytes)",
				isi_sb_iter_get_id(&iter),
				isi_sb_iter_get_len(&iter));
			break;
		}

		isi_sb_iter_next(&iter);
	}

	DBG("status=%d, lac=%d, ci=%d, tech=%d", *status, *lac, *ci, *tech);
	return TRUE;
}

static void reg_status_ind_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_netreg *netreg = opaque;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	int status = -1;
	int lac = -1;
	int ci = -1;
	int tech = -1;

	if (!msg || len < 3 || msg[0] != NET_REG_STATUS_IND)
		return;

	if (decode_reg_status(nd, msg+3, len-3, &status, &lac, &ci, &tech)) {
		status = isi_status_to_at_status(status);
		ofono_netreg_status_notify(netreg, status, lac, ci, tech);
	}
}

static bool reg_status_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	struct ofono_netreg *netreg = cbd->user;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	ofono_netreg_status_cb_t cb = cbd->cb;

	int status = -1;
	int lac = -1;
	int ci = -1;
	int tech = -1;

	DBG("");

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_REG_STATUS_GET_RESP)
		goto error;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	if (decode_reg_status(nd, msg+3, len-3, &status, &lac, &ci, &tech)) {

		CALLBACK_WITH_SUCCESS(cb, isi_status_to_at_status(status),
					lac, ci, tech, cbd->data);
		goto out;
	}

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct isi_cb_data *cbd = isi_cb_data_new(netreg, cb, data);

	const unsigned char msg[] = {
		NET_REG_STATUS_GET_REQ
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg), NETWORK_TIMEOUT,
				reg_status_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);  
}

static bool name_get_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_netreg_operator_cb_t cb = cbd->cb;

	struct ofono_network_operator op;
	struct isi_sb_iter iter;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_OPER_NAME_READ_RESP)
		goto error;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	if (!isi_sb_iter_init(msg + 7, len - 7, &iter))
		goto error;

	while (isi_sb_iter_is_valid(&iter)) {

		switch (isi_sb_iter_get_id(&iter)) {

		case NET_GSM_OPERATOR_INFO:

			if (isi_sb_iter_get_len(&iter) < 8)
				goto error;

			isi_sb_iter_get_oper_code(&iter, op.mcc, op.mnc, 2);
			break;

		case NET_OPER_NAME_INFO: {

			char *tag = NULL;

			if (isi_sb_iter_get_len(&iter) < 4)
				goto error;

			if (!isi_sb_iter_get_alpha_tag(&iter, &tag, 3))
				goto error;

			strncpy(op.name, tag, OFONO_MAX_OPERATOR_NAME_LENGTH);
			g_free(tag);
			break;
		}

		default:
			DBG("Skipping sub-block: 0x%02X (%u bytes)",
				isi_sb_iter_get_id(&iter),
				isi_sb_iter_get_len(&iter));
			break;
		}

		isi_sb_iter_next(&iter);
	}

	DBG("mnc=%s, mcc=%s, name=%s", op.mnc, op.mcc, op.name);
	CALLBACK_WITH_SUCCESS(cb, &op, cbd->data);
	goto out;
		
error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

out:
	g_free(cbd);
	return true;
}


static void isi_current_operator(struct ofono_netreg *netreg,
					ofono_netreg_operator_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct isi_cb_data *cbd = isi_cb_data_new(netreg, cb, data);

	const unsigned char msg[] = {
		NET_OPER_NAME_READ_REQ,
		NET_HARDCODED_LATIN_OPER_NAME,
		OFONO_MAX_OPERATOR_NAME_LENGTH,
		0x00, 0x00,  /* Index not used */
		0x00,  /* Filler */
		0x00  /* No sub-blocks */
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg), NETWORK_TIMEOUT,
				name_get_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}


static bool available_get_resp_cb(GIsiClient *client, const void *restrict data,
					size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct ofono_network_operator *list = NULL;
	int total = 0;

	struct isi_sb_iter iter;
	int common = 0;
	int detail = 0;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_AVAILABLE_GET_RESP)
		goto error;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	/* Each description of an operator has a pair of sub-blocks */
	total = msg[2] / 2;
	list = alloca(total * sizeof(struct ofono_network_operator));

	if (!isi_sb_iter_init(msg + 3, len - 3, &iter))
		goto error;

	while (isi_sb_iter_is_valid(&iter)) {

		switch (isi_sb_iter_get_id(&iter)) {

		case NET_AVAIL_NETWORK_INFO_COMMON: {
		
			struct ofono_network_operator *op;
			char *tag = NULL;

			if (isi_sb_iter_get_len(&iter) < 12)
				goto error;

			op = list + common++;
			if (!isi_sb_iter_get_byte(&iter, (guint8 *)&op->status, 2) ||
				!isi_sb_iter_get_alpha_tag(&iter, &tag, 5))
				goto error;

			strncpy(op->name, tag, OFONO_MAX_OPERATOR_NAME_LENGTH);
			op->name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';
			g_free(tag);
			break;
		}

		case NET_DETAILED_NETWORK_INFO: {

			struct ofono_network_operator *op;

			if (isi_sb_iter_get_len(&iter) < 8)
				goto error;

			op = list + detail++;
			if (!isi_sb_iter_get_oper_code(&iter, op->mcc, op->mnc, 2))
				goto error;
			break;
		}

		default:
			DBG("Skipping sub-block: 0x%02X (%u bytes)",
				isi_sb_iter_get_id(&iter),
				isi_sb_iter_get_len(&iter));
			break;
		}
		isi_sb_iter_next(&iter);
	}

	if (common == detail && detail == total) {
		CALLBACK_WITH_SUCCESS(cb, total, list, cbd->data);
		goto out;
	}

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb,
				void *data)
{
	struct netreg_data *net = ofono_netreg_get_data(netreg);
	struct isi_cb_data *cbd = isi_cb_data_new(netreg, cb, data);

	const unsigned char msg[] = {
		NET_AVAILABLE_GET_REQ,
		NET_MANUAL_SEARCH,
		0x01,  /* Sub-block count */
		NET_GSM_BAND_INFO,
		0x04,  /* Sub-block length */
		NET_GSM_BAND_ALL_SUPPORTED_BANDS,
		0x00
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(net->client, msg, sizeof(msg), NETWORK_SCAN_TIMEOUT,
				available_get_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
}

static bool net_set_auto_resp_cb(GIsiClient *client, const void *restrict data,
					size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	struct netreg_data *net = cbd->user;
	ofono_netreg_register_cb_t cb = cbd->cb;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (!msg|| len < 3 || msg[0] != NET_SET_RESP || msg[1] != NET_CAUSE_OK)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	net->last_reg_mode = NET_SELECT_MODE_AUTOMATIC;
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb,
				void *data)
{
	struct netreg_data *net = ofono_netreg_get_data(netreg);
	struct isi_cb_data *cbd = isi_cb_data_new(netreg, cb, data);

	const unsigned char msg[] = {
		NET_SET_REQ,
		0x00,  /* Registered in another protocol? */
		0x01,  /* Sub-block count */
		NET_OPERATOR_INFO_COMMON,
		0x04,  /* Sub-block length */
		net->last_reg_mode == NET_SELECT_MODE_AUTOMATIC
			? NET_SELECT_MODE_USER_RESELECTION
			: NET_SELECT_MODE_AUTOMATIC,
		0x00  /* Index not used */
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(net->client, msg, sizeof(msg), NETWORK_SET_TIMEOUT,
				net_set_auto_resp_cb, cbd)) {
		net->last_reg_mode = NET_SELECT_MODE_AUTOMATIC;
		return;
	}

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static bool set_manual_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	struct ofono_netreg *netreg = cbd->user;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	ofono_netreg_register_cb_t cb = cbd->cb;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_SET_RESP)
		goto error;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	nd->last_reg_mode = NET_SELECT_MODE_MANUAL;
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_register_manual(struct ofono_netreg *netreg,
				const struct ofono_network_operator *oper,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct isi_cb_data *cbd = isi_cb_data_new(netreg, cb, data);

	unsigned char buffer[3] = { 0 };
	unsigned char *bcd = mccmnc_to_bcd(oper->mcc, oper->mnc, buffer);

	const unsigned char msg[] = {
		NET_SET_REQ,
		0x00,  /* Registered in another protocol? */
		0x02,  /* Sub-block count */
		NET_OPERATOR_INFO_COMMON,
		0x04,  /* Sub-block length */
		NET_SELECT_MODE_MANUAL,
		0x00,  /* Index not used */
		NET_GSM_OPERATOR_INFO,
		0x08,  /* Sub-block length */
		bcd[0], bcd[1], bcd[2],
		NET_GSM_BAND_INFO_NOT_AVAIL,  /* Pick any supported band */
		0x00, 0x00  /* Filler */ 
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg), NETWORK_SET_TIMEOUT,
				set_manual_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_deregister(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb,
				void *data)
{
	DBG("Not implemented.");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void rat_ind_cb(GIsiClient *client, const void *restrict data,
			size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_netreg *netreg = opaque;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	struct isi_sb_iter iter;
	
	if (!msg || len < 3 || msg[0] != NET_RAT_IND)
		return;

	if (!isi_sb_iter_init(msg + 3, len - 3, &iter))
		return;

	while (isi_sb_iter_is_valid(&iter)) {

		switch (isi_sb_iter_get_id(&iter)) {

		case NET_RAT_INFO: {

			guint8 info = 0;

			if (isi_sb_iter_get_len(&iter) < 4)
				goto error;
			
			isi_sb_iter_get_byte(&iter, &nd->rat, 2);
			isi_sb_iter_get_byte(&iter, &info, 3);

			if (info)
				isi_sb_iter_get_byte(&iter, &nd->gsm_compact, 4);
			break;
		}

		default:
			DBG("Skipping sub-block: 0x%02X (%u bytes)",
				isi_sb_iter_get_id(&iter),
				isi_sb_iter_get_len(&iter));
			break;
		}
		isi_sb_iter_next(&iter);
	}

error:
	DBG("Truncated message");
	return;
}

static bool rat_resp_cb(GIsiClient *client, const void *restrict data,
			size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_netreg *netreg = opaque;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	struct isi_sb_iter iter;
	
	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return true;
	}

	if (len < 3 || msg[0] != NET_RAT_RESP)
		return true;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		return true;
	}

	if (!isi_sb_iter_init(msg + 3, len - 3, &iter))
		return true;

	while (isi_sb_iter_is_valid(&iter)) {

		switch (isi_sb_iter_get_id(&iter)) {

		case NET_RAT_INFO: {

			guint8 info = 0;

			if (isi_sb_iter_get_len(&iter) < 4)
				return true;
			
			isi_sb_iter_get_byte(&iter, &nd->rat, 2);
			isi_sb_iter_get_byte(&iter, &info, 3);

			if (info)
				isi_sb_iter_get_byte(&iter, &nd->gsm_compact, 4);
			break;
		}

		default:
			DBG("Skipping sub-block: 0x%02X (%u bytes)",
				isi_sb_iter_get_id(&iter),
				isi_sb_iter_get_len(&iter));
			break;
		}
		isi_sb_iter_next(&iter);
	}
	return true;
}

static void rssi_ind_cb(GIsiClient *client, const void *restrict data,
			size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_netreg *netreg = opaque;

	if (!msg || len < 3 || msg[0] != NET_RSSI_IND)
		return;

	ofono_netreg_strength_notify(netreg, msg[1]);
}

static bool rssi_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_netreg_strength_cb_t cb = cbd->cb;

	struct isi_sb_iter iter;
	int strength = -1;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_RSSI_GET_RESP)
		goto error;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	if (!isi_sb_iter_init(msg + 3, len - 3, &iter))
		goto error;

	while (isi_sb_iter_is_valid(&iter)) {

		switch (isi_sb_iter_get_id(&iter)) {

		case NET_RSSI_CURRENT: {

			guint8 rssi = 0;

			if (isi_sb_iter_get_len(&iter) < 4 ||
				!isi_sb_iter_get_byte(&iter, &rssi, 2))
				goto error;

			strength = rssi != 0 ? rssi : -1;
			break;
		}

		default:
			DBG("Skipping sub-block: 0x%02X (%d bytes)",
				isi_sb_iter_get_id(&iter),
				isi_sb_iter_get_len(&iter));
			break;
		}
		isi_sb_iter_next(&iter);
	}

	CALLBACK_WITH_SUCCESS(cb, strength, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb,
				void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct isi_cb_data *cbd = isi_cb_data_new(netreg, cb, data);

	const unsigned char msg[] = {
		NET_RSSI_GET_REQ,
		NET_CS_GSM,
		NET_CURRENT_CELL_RSSI
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg), NETWORK_TIMEOUT,
				rssi_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static gboolean isi_netreg_register(gpointer user)
{
	struct ofono_netreg *netreg = user;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	const unsigned char rat[] = {
		NET_RAT_REQ,
		NET_CURRENT_RAT
	};

	g_isi_client_set_debug(nd->client, net_debug, NULL);

	g_isi_subscribe(nd->client, NET_RSSI_IND, rssi_ind_cb, netreg);
	g_isi_subscribe(nd->client, NET_REG_STATUS_IND, reg_status_ind_cb,
			netreg);
	g_isi_subscribe(nd->client, NET_RAT_IND, rat_ind_cb, netreg);

	/* Bootstrap current RAT setting */
	if (!g_isi_request_make(nd->client, rat, sizeof(rat), NETWORK_TIMEOUT,
				rat_resp_cb, netreg))
		DBG("Failed to bootstrap RAT");

	ofono_netreg_register(netreg);

	return FALSE;
}

static int isi_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct netreg_data *nd = g_try_new0(struct netreg_data, 1);

	if (!nd)
		return -ENOMEM;

	nd->client = g_isi_client_create(idx, PN_NETWORK);
	if (!nd->client) {
		g_free(nd);
		return -ENOMEM;
	}

	ofono_netreg_set_data(netreg, nd);

	g_idle_add(isi_netreg_register, netreg);

	return 0;
}

static void isi_netreg_remove(struct ofono_netreg *net)
{
	struct netreg_data *data = ofono_netreg_get_data(net);

	if (data && data->client) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_netreg_driver driver = {
	.name			= "isimodem",
	.probe			= isi_netreg_probe,
	.remove			= isi_netreg_remove,
	.registration_status 	= isi_registration_status,
	.current_operator 	= isi_current_operator,
	.list_operators		= isi_list_operators,
	.register_auto		= isi_register_auto,
	.register_manual	= isi_register_manual,
	.deregister		= isi_deregister,
	.strength		= isi_strength,
};

void isi_netreg_init()
{
	ofono_netreg_driver_register(&driver);
}

void isi_netreg_exit()
{
	ofono_netreg_driver_unregister(&driver);
}
