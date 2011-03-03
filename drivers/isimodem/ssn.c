/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) ST-Ericsson SA 2011.
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
#include <ofono/ssn.h>

#include "call.h"
#include "isimodem.h"
#include "ss.h"
#include "isiutil.h"
#include "debug.h"



struct ssn_data {
	GIsiClient *client;
};

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

	if (byte & CALL_HOLD_IND_RETRIEVED) {
		*cssu = SS_MT_VOICECALL_RETRIEVED;
		DBG("Call has been retrieved");
	}

	if (byte & CALL_HOLD_IND_ON_HOLD) {
		*cssu = SS_MT_VOICECALL_ON_HOLD;
		DBG("Call has been put on hold");
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
	struct ofono_ssn *ssn = data;
	struct ssn_data *sd = ofono_ssn_get_data(ssn);
	GIsiSubBlockIter iter;

	struct ofono_phone_number number;
	int index = 0;
	int cssi = -1;
	int cssu = -1;
	uint8_t byte;

	if (ssn == NULL || sd == NULL)
		return;

	if (g_isi_msg_id(msg) != CALL_GSM_NOTIFICATION_IND)
		return;

	if (!g_isi_msg_data_get_byte(msg, 0, &byte))
		return;

	DBG("Received CallServer notification for call: 0x%02X", byte);

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
		ofono_ssn_cssi_notify(ssn, cssi, index);

	if (cssu != -1)
		ofono_ssn_cssu_notify(ssn, cssu, index, &number);
}

static void reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_ssn *ssn = data;
	struct ssn_data *sd = ofono_ssn_get_data(ssn);

	if (g_isi_msg_error(msg) < 0)
		return;

	if (sd == NULL)
		return;

	ISI_VERSION_DBG(msg);

	g_isi_client_ind_subscribe(sd->client, CALL_GSM_NOTIFICATION_IND,
					notification_ind_cb, ssn);

	ofono_ssn_register(ssn);
}

static int probe_by_resource(struct ofono_ssn *ssn, uint8_t resource,
				void *user)
{
	GIsiModem *modem = user;
	struct ssn_data *sd;

	sd = g_try_new0(struct ssn_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->client = g_isi_client_create(modem, resource);
	if (sd->client == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	g_isi_client_verify(sd->client, reachable_cb, ssn, NULL);

	ofono_ssn_set_data(ssn, sd);

	return 0;
}

static int isi_ssn_probe(struct ofono_ssn *ssn, unsigned int vendor,
				void *user)
{
	return probe_by_resource(ssn, PN_CALL, user);
}

static int wg_ssn_probe(struct ofono_ssn *ssn, unsigned int vendor,
				void *user)
{
	return probe_by_resource(ssn, PN_MODEM_CALL, user);
}

static void isi_ssn_remove(struct ofono_ssn *ssn)
{
	struct ssn_data *data = ofono_ssn_get_data(ssn);

	ofono_ssn_set_data(ssn, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_ssn_driver driver = {
		.name			= "isimodem",
		.probe			= isi_ssn_probe,
		.remove			= isi_ssn_remove
};

static struct ofono_ssn_driver wgdriver = {
		.name			= "wgmodem2.5",
		.probe			= wg_ssn_probe,
		.remove			= isi_ssn_remove
};

void isi_ssn_init(void)
{
	ofono_ssn_driver_register(&driver);
	ofono_ssn_driver_register(&wgdriver);
}

void isi_ssn_exit(void)
{
	ofono_ssn_driver_unregister(&driver);
	ofono_ssn_driver_unregister(&wgdriver);
}
