/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: <Pekka.Pessi@nokia.com>
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

#include <glib.h>

#include <gisi/client.h>
#include <gisi/iter.h>

#include "isi-call.h"

#include <ofono/log.h>

#define DUMP(fmt, arg...) ofono_debug(fmt, ## arg)

char const *isi_call_status_name(enum isi_call_status value)
{
	switch (value) {
#define _(X) case CALL_STATUS_ ## X: return #X
		_(IDLE);
		_(CREATE);
		_(COMING);
		_(PROCEEDING);
		_(MO_ALERTING);
		_(MT_ALERTING);
		_(WAITING);
		_(ANSWERED);
		_(ACTIVE);
		_(MO_RELEASE);
		_(MT_RELEASE);
		_(HOLD_INITIATED);
		_(HOLD);
		_(RETRIEVE_INITIATED);
		_(RECONNECT_PENDING);
		_(TERMINATED);
		_(SWAP_INITIATED);
#undef _
	}
	return "<UNKNOWN>";
}

char const *isi_call_message_id_name(enum isi_call_message_id value)
{
	switch (value) {
#define _(X) case X: return #X
		_(CALL_CREATE_REQ);
		_(CALL_CREATE_RESP);
		_(CALL_COMING_IND);
		_(CALL_MO_ALERT_IND);
		_(CALL_MT_ALERT_IND);
		_(CALL_WAITING_IND);
		_(CALL_ANSWER_REQ);
		_(CALL_ANSWER_RESP);
		_(CALL_RELEASE_REQ);
		_(CALL_RELEASE_RESP);
		_(CALL_RELEASE_IND);
		_(CALL_TERMINATED_IND);
		_(CALL_STATUS_REQ);
		_(CALL_STATUS_RESP);
		_(CALL_STATUS_IND);
		_(CALL_SERVER_STATUS_IND);
		_(CALL_CONTROL_REQ);
		_(CALL_CONTROL_RESP);
		_(CALL_CONTROL_IND);
		_(CALL_MODE_SWITCH_REQ);
		_(CALL_MODE_SWITCH_RESP);
		_(CALL_MODE_SWITCH_IND);
		_(CALL_DTMF_SEND_REQ);
		_(CALL_DTMF_SEND_RESP);
		_(CALL_DTMF_STOP_REQ);
		_(CALL_DTMF_STOP_RESP);
		_(CALL_DTMF_STATUS_IND);
		_(CALL_DTMF_TONE_IND);
		_(CALL_RECONNECT_IND);
		_(CALL_PROPERTY_GET_REQ);
		_(CALL_PROPERTY_GET_RESP);
		_(CALL_PROPERTY_SET_REQ);
		_(CALL_PROPERTY_SET_RESP);
		_(CALL_PROPERTY_SET_IND);
		_(CALL_EMERGENCY_NBR_CHECK_REQ);
		_(CALL_EMERGENCY_NBR_CHECK_RESP);
		_(CALL_EMERGENCY_NBR_GET_REQ);
		_(CALL_EMERGENCY_NBR_GET_RESP);
		_(CALL_EMERGENCY_NBR_MODIFY_REQ);
		_(CALL_EMERGENCY_NBR_MODIFY_RESP);
		_(CALL_GSM_NOTIFICATION_IND);
		_(CALL_GSM_USER_TO_USER_REQ);
		_(CALL_GSM_USER_TO_USER_RESP);
		_(CALL_GSM_USER_TO_USER_IND);
		_(CALL_GSM_BLACKLIST_CLEAR_REQ);
		_(CALL_GSM_BLACKLIST_CLEAR_RESP);
		_(CALL_GSM_BLACKLIST_TIMER_IND);
		_(CALL_GSM_DATA_CH_INFO_IND);
		_(CALL_GSM_CCP_GET_REQ);
		_(CALL_GSM_CCP_GET_RESP);
		_(CALL_GSM_CCP_CHECK_REQ);
		_(CALL_GSM_CCP_CHECK_RESP);
		_(CALL_GSM_COMING_REJ_IND);
		_(CALL_GSM_RAB_IND);
		_(CALL_GSM_IMMEDIATE_MODIFY_IND);
		_(CALL_CREATE_NO_SIMATK_REQ);
		_(CALL_GSM_SS_DATA_IND);
		_(CALL_TIMER_REQ);
		_(CALL_TIMER_RESP);
		_(CALL_TIMER_NTF);
		_(CALL_TIMER_IND);
		_(CALL_TIMER_RESET_REQ);
		_(CALL_TIMER_RESET_RESP);
		_(CALL_EMERGENCY_NBR_IND);
		_(CALL_SERVICE_DENIED_IND);
		_(CALL_RELEASE_END_REQ);
		_(CALL_RELEASE_END_RESP);
		_(CALL_USER_CONNECT_IND);
		_(CALL_AUDIO_CONNECT_IND);
		_(CALL_KODIAK_ALLOW_CTRL_REQ);
		_(CALL_KODIAK_ALLOW_CTRL_RESP);
		_(CALL_SERVICE_ACTIVATE_IND);
		_(CALL_SERVICE_ACTIVATE_REQ);
		_(CALL_SERVICE_ACTIVATE_RESP);
		_(CALL_SIM_ATK_IND);
		_(CALL_CONTROL_OPER_IND);
		_(CALL_TEST_CALL_STATUS_IND);
		_(CALL_SIM_ATK_INFO_IND);
		_(CALL_SECURITY_IND);
		_(CALL_MEDIA_HANDLE_REQ);
		_(CALL_MEDIA_HANDLE_RESP);
		_(COMMON_MESSAGE);
#undef _
	}
	return "<UNKNOWN>";
}

char const *isi_call_isi_cause_name(enum isi_call_isi_cause value)
{
	switch (value)
	{
#define _(X) case CALL_CAUSE_ ## X: return "CAUSE_" #X
		_(NO_CAUSE);
		_(NO_CALL);
		_(TIMEOUT);
		_(RELEASE_BY_USER);
		_(BUSY_USER_REQUEST);
		_(ERROR_REQUEST);
		_(COST_LIMIT_REACHED);
		_(CALL_ACTIVE);
		_(NO_CALL_ACTIVE);
		_(INVALID_CALL_MODE);
		_(SIGNALLING_FAILURE);
		_(TOO_LONG_ADDRESS);
		_(INVALID_ADDRESS);
		_(EMERGENCY);
		_(NO_TRAFFIC_CHANNEL);
		_(NO_COVERAGE);
		_(CODE_REQUIRED);
		_(NOT_ALLOWED);
		_(NO_DTMF);
		_(CHANNEL_LOSS);
		_(FDN_NOT_OK);
		_(USER_TERMINATED);
		_(BLACKLIST_BLOCKED);
		_(BLACKLIST_DELAYED);
		_(NUMBER_NOT_FOUND);
		_(NUMBER_CANNOT_REMOVE);
		_(EMERGENCY_FAILURE);
		_(CS_SUSPENDED);
		_(DCM_DRIVE_MODE);
		_(MULTIMEDIA_NOT_ALLOWED);
		_(SIM_REJECTED);
		_(NO_SIM);
		_(SIM_LOCK_OPERATIVE);
		_(SIMATKCC_REJECTED);
		_(SIMATKCC_MODIFIED);
		_(DTMF_INVALID_DIGIT);
		_(DTMF_SEND_ONGOING);
		_(CS_INACTIVE);
		_(SECURITY_MODE);
		_(TRACFONE_FAILED);
		_(TRACFONE_WAIT_FAILED);
		_(TRACFONE_CONF_FAILED);
		_(TEMPERATURE_LIMIT);
		_(KODIAK_POC_FAILED);
		_(NOT_REGISTERED);
		_(CS_CALLS_ONLY);
		_(VOIP_CALLS_ONLY);
		_(LIMITED_CALL_ACTIVE);
		_(LIMITED_CALL_NOT_ALLOWED);
		_(SECURE_CALL_NOT_POSSIBLE);
		_(INTERCEPT);
#undef _
	}
	return "<UNKNOWN>";
}

char const *isi_call_gsm_cause_name(enum isi_call_gsm_cause value)
{
	switch (value)
	{
#define _(X) case CALL_GSM_CAUSE_ ## X: return "GSM_CAUSE_" #X
		_(UNASSIGNED_NUMBER);
		_(NO_ROUTE);
		_(CH_UNACCEPTABLE);
		_(OPER_BARRING);
		_(NORMAL);
		_(USER_BUSY);
		_(NO_USER_RESPONSE);
		_(ALERT_NO_ANSWER);
		_(CALL_REJECTED);
		_(NUMBER_CHANGED);
		_(NON_SELECT_CLEAR);
		_(DEST_OUT_OF_ORDER);
		_(INVALID_NUMBER);
		_(FACILITY_REJECTED);
		_(RESP_TO_STATUS);
		_(NORMAL_UNSPECIFIED);
		_(NO_CHANNEL);
		_(NETW_OUT_OF_ORDER);
		_(TEMPORARY_FAILURE);
		_(CONGESTION);
		_(ACCESS_INFO_DISC);
		_(CHANNEL_NA);
		_(RESOURCES_NA);
		_(QOS_NA);
		_(FACILITY_UNSUBS);
		_(COMING_BARRED_CUG);
		_(BC_UNAUTHORIZED);
		_(BC_NA);
		_(SERVICE_NA);
		_(BEARER_NOT_IMPL);
		_(ACM_MAX);
		_(FACILITY_NOT_IMPL);
		_(ONLY_RDI_BC);
		_(SERVICE_NOT_IMPL);
		_(INVALID_TI);
		_(NOT_IN_CUG);
		_(INCOMPATIBLE_DEST);
		_(INV_TRANS_NET_SEL);
		_(SEMANTICAL_ERR);
		_(INVALID_MANDATORY);
		_(MSG_TYPE_INEXIST);
		_(MSG_TYPE_INCOMPAT);
		_(IE_NON_EXISTENT);
		_(COND_IE_ERROR);
		_(MSG_INCOMPATIBLE);
		_(TIMER_EXPIRY);
		_(PROTOCOL_ERROR);
		_(INTERWORKING);
#undef _
	}
	return "<UNKNOWN>";
}

char const *isi_call_cause_name(uint8_t cause_type, uint8_t cause)
{
	switch (cause_type)
	{
	case CALL_CAUSE_TYPE_DEFAULT:
	case CALL_CAUSE_TYPE_CLIENT:
	case CALL_CAUSE_TYPE_SERVER:
		return isi_call_isi_cause_name(cause);
	case CALL_CAUSE_TYPE_NETWORK:
		return isi_call_gsm_cause_name(cause);
	}
	return "<UNKNOWN>";
}

static void isi_call_hex_dump(uint8_t const m[],
			      size_t len,
			      char const *name)
{
	char const *prefix;
	char hex[3 * 16 + 1];
	char ascii[16 + 1];
	size_t i, j, k;

	if (strncmp(name, "CALL_", 5))
		prefix = "CALL ";
	else
		prefix = "";

	DUMP("%s%s [%s=0x%02X len=%zu]:",
	     prefix, name, "message_id", m[1], len);

	strcpy(hex, " **"), j = 3;
	strcpy(ascii, "."), k = 1;

	for (i = 1; i < len; i++) {
		sprintf(hex + j, " %02X", m[i]), j += 3;
		ascii[k++] = g_ascii_isgraph(m[i]) ? m[i] : '.';

		if ((i & 15) == 15) {
			DUMP("    *%-48s : %.*s", hex, (int)k, ascii);
			j = 0, k = 0;
		}
	}

	if (j) {
		DUMP("    *%-48s : %.*s", hex, (int)k, ascii);
	}
}

void isi_call_debug(const void *restrict buf, size_t len, void *data)
{
	uint8_t const *m = buf;
	char const *name;

	m = buf, m--, len++, buf = m;

	if (len < 4) {
		DUMP("CALL: %s [len=%zu]", "RUNT", len);
		return;
	}

	name = isi_call_message_id_name(m[1]);

	isi_call_hex_dump(m, len, name);
}
