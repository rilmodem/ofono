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
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "isimodem.h"
#include "isiutil.h"
#include "network.h"
#include "debug.h"

struct netreg_data {
	GIsiClient *client;
	guint8 last_reg_mode;
	guint8 rat;
	guint8 gsm_compact;
};

static inline guint8 *mccmnc_to_bcd(const char *mcc, const char *mnc,
						guint8 *bcd)
{
	bcd[0] = (mcc[0] - '0') | (mcc[1] - '0') << 4;
	bcd[1] = (mcc[2] - '0');
	bcd[1] |= (mnc[2] == '\0' ? 0x0f : (mnc[2] - '0')) << 4;
	bcd[2] = (mnc[0] - '0') | (mnc[1] - '0') << 4;
	return bcd;
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
	GIsiSubBlockIter iter;

	g_isi_sb_iter_init(&iter, msg, len, 0);

	while (g_isi_sb_iter_is_valid(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case NET_REG_INFO_COMMON: {
			guint8 byte = 0;

			if (!g_isi_sb_iter_get_byte(&iter, &byte, 2))
				return FALSE;

			if (!g_isi_sb_iter_get_byte(&iter,
						&nd->last_reg_mode, 3))
				return FALSE;

			*status = byte;

			/* FIXME: decode alpha tag(s) */
			break;
		}

		case NET_GSM_REG_INFO: {
			guint16 word = 0;
			guint32 dword = 0;
			guint8 egprs = 0;
			guint8 hsdpa = 0;
			guint8 hsupa = 0;

			if (!g_isi_sb_iter_get_word(&iter, &word, 2) ||
				!g_isi_sb_iter_get_dword(&iter, &dword, 4) ||
				!g_isi_sb_iter_get_byte(&iter, &egprs, 17) ||
				!g_isi_sb_iter_get_byte(&iter, &hsdpa, 20) ||
				!g_isi_sb_iter_get_byte(&iter, &hsupa, 21))
				return FALSE;

			*ci = (int)dword;
			*lac = (int)word;

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
			DBG("Skipping sub-block: %s (%zu bytes)",
				net_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}

		g_isi_sb_iter_next(&iter);
	}

	DBG("status=%s, lac=%d, ci=%d, tech=%d",
		net_status_name(*status), *lac, *ci, *tech);

	return TRUE;
}

static void reg_status_ind_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
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

static gboolean reg_status_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
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

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_REG_STATUS_GET_RESP)
		return FALSE;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: %s", net_isi_cause_name(msg[1]));
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
	return TRUE;
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

	if (!cbd || !nd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg),
				NETWORK_TIMEOUT,
				reg_status_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
	g_free(cbd);
}

static gboolean name_get_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_netreg_operator_cb_t cb = cbd->cb;

	struct ofono_network_operator op;
	GIsiSubBlockIter iter;

	DBG("");

	memset(&op, 0, sizeof(struct ofono_network_operator));

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_OPER_NAME_READ_RESP)
		return FALSE;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: %s", net_isi_cause_name(msg[1]));
		goto error;
	}

	g_isi_sb_iter_init(&iter, msg, len, 7);

	while (g_isi_sb_iter_is_valid(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case NET_GSM_OPERATOR_INFO:
			if (!g_isi_sb_iter_get_oper_code(&iter, op.mcc,
								op.mnc, 2))
				goto error;
			break;

		case NET_OPER_NAME_INFO: {
			char *tag = NULL;
			guint8 taglen = 0;

			if (!g_isi_sb_iter_get_byte(&iter, &taglen, 3)
				|| !g_isi_sb_iter_get_alpha_tag(&iter, &tag,
								taglen * 2, 4))
				goto error;

			strncpy(op.name, tag, OFONO_MAX_OPERATOR_NAME_LENGTH);
			op.name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';
			g_free(tag);
			break;
		}

		default:
			DBG("Skipping sub-block: %s (%zu bytes)",
				net_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
		g_isi_sb_iter_next(&iter);
	}

	CALLBACK_WITH_SUCCESS(cb, &op, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

out:
	g_free(cbd);
	return TRUE;
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

	if (!cbd || !nd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg),
				NETWORK_TIMEOUT,
				name_get_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
}


static gboolean available_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct ofono_network_operator *list = NULL;
	int total = 0;

	GIsiSubBlockIter iter;
	int common = 0;
	int detail = 0;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_AVAILABLE_GET_RESP)
		return FALSE;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: %s", net_isi_cause_name(msg[1]));
		goto error;
	}

	/* Each description of an operator has a pair of sub-blocks */
	total = msg[2] / 2;
	list = alloca(total * sizeof(struct ofono_network_operator));

	for (g_isi_sb_iter_init(&iter, msg, len, 3);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		struct ofono_network_operator *op;
		char *tag = NULL;
		guint8 taglen = 0;
		guint8 status = 0;
		guint8 umts = 0;

		switch (g_isi_sb_iter_get_id(&iter)) {

		case NET_AVAIL_NETWORK_INFO_COMMON:

			if (!g_isi_sb_iter_get_byte(&iter, &status, 2)
				|| !g_isi_sb_iter_get_byte(&iter, &taglen, 5)
				|| !g_isi_sb_iter_get_alpha_tag(&iter, &tag,
						taglen * 2, 6))
				goto error;

			op = list + common++;
			op->status = status;

			strncpy(op->name, tag, OFONO_MAX_OPERATOR_NAME_LENGTH);
			op->name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';
			g_free(tag);
			break;

		case NET_DETAILED_NETWORK_INFO:
			op = list + detail++;

			if (!g_isi_sb_iter_get_oper_code(&iter, op->mcc,
								op->mnc, 2)
				|| !g_isi_sb_iter_get_byte(&iter, &umts, 7))
				goto error;

			op->tech = umts ? 2 : 3;
			break;

		default:
			DBG("Skipping sub-block: %s (%zu bytes)",
				net_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	if (common == detail && detail == total) {
		CALLBACK_WITH_SUCCESS(cb, total, list, cbd->data);
		goto out;
	}

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);

out:
	g_free(cbd);
	return TRUE;
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

	if (!cbd || !net)
		goto error;

	if (g_isi_request_make(net->client, msg, sizeof(msg),
				NETWORK_SCAN_TIMEOUT,
				available_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
	g_free(cbd);
}

static gboolean set_auto_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	struct netreg_data *net = cbd->user;
	ofono_netreg_register_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (!msg || len < 3 || msg[0] != NET_SET_RESP)
		return FALSE;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: %s", net_isi_cause_name(msg[1]));
		goto error;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	net->last_reg_mode = NET_SELECT_MODE_AUTOMATIC;
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;
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

	if (!cbd || !net)
		goto error;

	if (g_isi_request_make(net->client, msg, sizeof(msg),
				NETWORK_SET_TIMEOUT,
				set_auto_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static gboolean set_manual_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	struct ofono_netreg *netreg = cbd->user;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	ofono_netreg_register_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_SET_RESP)
		return FALSE;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: %s", net_isi_cause_name(msg[1]));
		goto error;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	nd->last_reg_mode = NET_SELECT_MODE_MANUAL;
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;
}

static void isi_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct isi_cb_data *cbd = isi_cb_data_new(netreg, cb, data);

	guint8 buffer[3] = { 0 };
	guint8 *bcd = mccmnc_to_bcd(mcc, mnc, buffer);

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

	if (!cbd || !nd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg),
				NETWORK_SET_TIMEOUT,
				set_manual_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void isi_deregister(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb,
				void *data)
{
	DBG("Not implemented.");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void rat_ind_cb(GIsiClient *client,
			const void *restrict data, size_t len,
			uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_netreg *netreg = opaque;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	GIsiSubBlockIter iter;

	if (!msg || len < 3 || msg[0] != NET_RAT_IND)
		return;

	g_isi_sb_iter_init(&iter, msg, len, 3);

	while (g_isi_sb_iter_is_valid(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case NET_RAT_INFO: {
			guint8 info = 0;

			if (!g_isi_sb_iter_get_byte(&iter, &nd->rat, 2)
				|| !g_isi_sb_iter_get_byte(&iter, &info, 3))
				return;

			if (info) {

				if (!g_isi_sb_iter_get_byte(&iter,
							&nd->gsm_compact, 4))
					return;
			}
			break;
		}

		default:
			DBG("Skipping sub-block: %s (%zu bytes)",
				net_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
		g_isi_sb_iter_next(&iter);
	}
}

static gboolean rat_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_netreg *netreg = opaque;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	GIsiSubBlockIter iter;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return TRUE;
	}

	if (len < 3 || msg[0] != NET_RAT_RESP)
		return FALSE;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: %s", net_isi_cause_name(msg[1]));
		return TRUE;
	}

	g_isi_sb_iter_init(&iter, msg, len, 3);

	while (g_isi_sb_iter_is_valid(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case NET_RAT_INFO: {
			guint8 info = 0;

			if (!g_isi_sb_iter_get_byte(&iter, &nd->rat, 2)
				|| !g_isi_sb_iter_get_byte(&iter, &info, 3)
				|| !info
				|| !g_isi_sb_iter_get_byte(&iter,
							&nd->gsm_compact, 4))
				return TRUE;

			break;
		}

		default:
			DBG("Skipping sub-block: %s (%zu bytes)",
				net_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
		g_isi_sb_iter_next(&iter);
	}
	return TRUE;
}

static void rssi_ind_cb(GIsiClient *client,
			const void *restrict data, size_t len,
			uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_netreg *netreg = opaque;

	if (!msg || len < 3 || msg[0] != NET_RSSI_IND)
		return;

	ofono_netreg_strength_notify(netreg, msg[1]);
}

static void time_ind_cb(GIsiClient *client,
			const void *restrict data, size_t len,
			uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	const unsigned char *nitz = msg + 3;
	struct ofono_netreg *netreg = opaque;

	struct ofono_network_time info;

	if (!msg || len < 13 || msg[0] != NET_TIME_IND
		|| nitz[0] != NET_TIME_INFO)
		return;

	nitz += 2;

	/* Value is years since last turn of century */
	info.year = nitz[0] != NET_INVALID_TIME ? nitz[0] : -1;
	info.year += 2000;

	info.mon = nitz[1] != NET_INVALID_TIME ? nitz[1] : -1;
	info.mday = nitz[2] != NET_INVALID_TIME ? nitz[2] : -1;
	info.hour = nitz[3] != NET_INVALID_TIME ? nitz[3] : -1;
	info.min = nitz[4] != NET_INVALID_TIME ? nitz[4] : -1;
	info.sec = nitz[5] != NET_INVALID_TIME ? nitz[5] : -1;

	/* Most significant bit set indicates negative offset. The
	 * second most significant bit is 'reserved'. The value is the
	 * offset from UTCin a count of 15min intervals, possibly
	 * including the current DST adjustment. */
	info.utcoff = (nitz[6] & 0x3F) * 15 * 60;
	if (nitz[6] & 0x80)
		info.utcoff *= -1;

	info.dst = nitz[7] != NET_INVALID_TIME ? nitz[7] : -1;

	ofono_netreg_time_notify(netreg, &info);
}

static gboolean rssi_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_netreg_strength_cb_t cb = cbd->cb;

	GIsiSubBlockIter iter;
	int strength = -1;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != NET_RSSI_GET_RESP)
		return FALSE;

	if (msg[1] != NET_CAUSE_OK) {
		DBG("Request failed: %s (0x%02X)",
			net_isi_cause_name(msg[1]), msg[1]);
		goto error;
	}

	g_isi_sb_iter_init(&iter, msg, len, 3);

	while (g_isi_sb_iter_is_valid(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case NET_RSSI_CURRENT: {
			guint8 rssi = 0;

			if (!g_isi_sb_iter_get_byte(&iter, &rssi, 2))
				goto error;

			strength = rssi != 0 ? rssi : -1;
			break;
		}

		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				net_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
		g_isi_sb_iter_next(&iter);
	}

	CALLBACK_WITH_SUCCESS(cb, strength, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

out:
	g_free(cbd);
	return TRUE;
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

	if (!cbd || !nd)
		goto error;

	if (g_isi_request_make(nd->client, msg, sizeof(msg),
				NETWORK_TIMEOUT,
				rssi_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static gboolean isi_netreg_register(gpointer user)
{
	struct ofono_netreg *netreg = user;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	const unsigned char rat[] = {
		NET_RAT_REQ,
		NET_CURRENT_RAT
	};

	const char *debug = getenv("OFONO_ISI_DEBUG");

	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "net") == 0))
		g_isi_client_set_debug(nd->client, net_debug, NULL);

	g_isi_subscribe(nd->client, NET_RSSI_IND, rssi_ind_cb, netreg);
	g_isi_subscribe(nd->client, NET_REG_STATUS_IND, reg_status_ind_cb,
			netreg);
	g_isi_subscribe(nd->client, NET_RAT_IND, rat_ind_cb, netreg);
	g_isi_subscribe(nd->client, NET_TIME_IND, time_ind_cb, netreg);

	/* Bootstrap current RAT setting */
	if (!g_isi_request_make(nd->client, rat, sizeof(rat),
				NETWORK_TIMEOUT,
				rat_resp_cb, netreg))
		DBG("Failed to bootstrap RAT");

	ofono_netreg_register(netreg);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	struct ofono_netreg *netreg = opaque;

	if (!alive) {
		DBG("Unable to bootstrap netreg driver");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_idle_add(isi_netreg_register, netreg);
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
	g_isi_verify(nd->client, reachable_cb, netreg);

	return 0;
}

static void isi_netreg_remove(struct ofono_netreg *net)
{
	struct netreg_data *data = ofono_netreg_get_data(net);

	if (!data)
		return;

	ofono_netreg_set_data(net, NULL);
	g_isi_client_destroy(data->client);
	g_free(data);
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
