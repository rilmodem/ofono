/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include <glib.h>

#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include <ofono/types.h>

#include "grilutil.h"
#include "parcel.h"
#include "ril_constants.h"

/* Constants used by CALL_LIST, and SETUP_DATA_CALL RIL requests */
#define PROTO_IP_STR "IP"
#define PROTO_IPV6_STR "IPV6"
#define PROTO_IPV4V6_STR "IPV4V6"

const char *ril_ofono_protocol_to_ril_string(guint protocol)
{
	char *result;

	switch (protocol) {
	case OFONO_GPRS_PROTO_IPV6:
		result = PROTO_IPV6_STR;
		break;
	case OFONO_GPRS_PROTO_IPV4V6:
		result = PROTO_IPV4V6_STR;
		break;
	case OFONO_GPRS_PROTO_IP:
		result = PROTO_IP_STR;
		break;
	default:
		result = NULL;
	}

	return result;
}

int ril_protocol_string_to_ofono_protocol(gchar *protocol_str)
{
	int result;

	if (g_strcmp0(protocol_str, PROTO_IPV6_STR) == 0)
		result = OFONO_GPRS_PROTO_IPV6;
	else if (g_strcmp0(protocol_str, PROTO_IPV4V6_STR) == 0)
		result = OFONO_GPRS_PROTO_IPV4V6;
	else if (g_strcmp0(protocol_str, PROTO_IP_STR) == 0)
		result = OFONO_GPRS_PROTO_IP;
	else
		result = -1;

	return result;
}

const char *ril_appstate_to_string(int app_state)
{
	switch (app_state) {
	case RIL_APPSTATE_UNKNOWN:
		return "UNKNOWN";
	case RIL_APPSTATE_DETECTED:
		return "DETECTED";
	case RIL_APPSTATE_PIN:
		return "PIN";
	case RIL_APPSTATE_PUK:
		return "PUK";
	case RIL_APPSTATE_SUBSCRIPTION_PERSO:
		return "";
	case RIL_APPSTATE_READY:
		return "READY";
	default:
		return "<INVALID>";
	}
}

const char *ril_apptype_to_string(int app_type)
{

	switch (app_type) {
	case RIL_APPTYPE_UNKNOWN:
		return "UNKNOWN";
	case RIL_APPTYPE_SIM:
		return "SIM";
	case RIL_APPTYPE_USIM:
		return "USIM";
	case RIL_APPTYPE_RUIM:
		return "RUIM";
	case RIL_APPTYPE_CSIM:
		return "CSIM";
	case RIL_APPTYPE_ISIM:
		return "ISIM";
	default:
		return "<INVALID>";
	}
}

const char *ril_cardstate_to_string(int card_state)
{
	switch (card_state) {
	case RIL_CARDSTATE_ABSENT:
		return "ABSENT";
	case RIL_CARDSTATE_PRESENT:
		return "PRESENT";
	case RIL_CARDSTATE_ERROR:
		return "ERROR";
	default:
		return "<INVALID>";
	}
}

const char *ril_error_to_string(int error)
{
	switch(error) {
	case RIL_E_SUCCESS: return "SUCCESS";
	case RIL_E_RADIO_NOT_AVAILABLE: return "RADIO_NOT_AVAILABLE";
	case RIL_E_GENERIC_FAILURE: return "GENERIC_FAILURE";
	case RIL_E_PASSWORD_INCORRECT: return "PASSWORD_INCORRECT";
	case RIL_E_SIM_PIN2: return "SIM_PIN2";
	case RIL_E_SIM_PUK2: return "SIM_PUK2";
	case RIL_E_REQUEST_NOT_SUPPORTED: return "REQUEST_NOT_SUPPORTED";
	case RIL_E_CANCELLED: return "CANCELLED";
	case RIL_E_OP_NOT_ALLOWED_DURING_VOICE_CALL: return "OP_NOT_ALLOWED_DURING_VOICE_CALL";
	case RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW: return "OP_NOT_ALLOWED_BEFORE_REG_TO_NW";
	case RIL_E_SMS_SEND_FAIL_RETRY: return "SMS_SEND_FAIL_RETRY";
	case RIL_E_SIM_ABSENT: return "SIM_ABSENT";
	case RIL_E_SUBSCRIPTION_NOT_AVAILABLE: return "SUBSCRIPTION_NOT_AVAILABLE";
	case RIL_E_MODE_NOT_SUPPORTED: return "MODE_NOT_SUPPORTED";
	case RIL_E_FDN_CHECK_FAILURE: return "FDN_CHECK_FAILURE";
	case RIL_E_ILLEGAL_SIM_OR_ME: return "ILLEGAL_SIM_OR_ME";
	case RIL_E_DIAL_MODIFIED_TO_USSD: return "DIAL_MODIFIED_TO_USSD";
	case RIL_E_DIAL_MODIFIED_TO_SS: return "DIAL_MODIFIED_TO_SS";
	case RIL_E_DIAL_MODIFIED_TO_DIAL: return "DIAL_MODIFIED_TO_DIAL";
	case RIL_E_USSD_MODIFIED_TO_DIAL: return "USSD_MODIFIED_TO_DIAL";
	case RIL_E_USSD_MODIFIED_TO_SS: return "USSD_MODIFIED_TO_SS";
	case RIL_E_USSD_MODIFIED_TO_USSD: return "USSD_MODIFIED_TO_USSD";
	case RIL_E_SS_MODIFIED_TO_DIAL: return "SS_MODIFIED_TO_DIAL";
	case RIL_E_SS_MODIFIED_TO_USSD: return "SS_MODIFIED_TO_USSD";
	case RIL_E_SS_MODIFIED_TO_SS: return "SS_MODIFIED_TO_SS";
	case RIL_E_SUBSCRIPTION_NOT_SUPPORTED:
		return "SUBSCRIPTION_NOT_SUPPORTED";
	default: return "<unknown errno>";
	}
}

const char *ril_pinstate_to_string(int pin_state)
{
	switch (pin_state) {
	case RIL_PINSTATE_UNKNOWN:
		return "UNKNOWN";
	case RIL_PINSTATE_ENABLED_NOT_VERIFIED:
		return "ENABLED_NOT_VERIFIED";
	case RIL_PINSTATE_ENABLED_VERIFIED:
		return "ENABLED_VERIFIED";
	case RIL_PINSTATE_DISABLED:
		return "DISABLED";
	case RIL_PINSTATE_ENABLED_BLOCKED:
		return "ENABLED_BLOCKED";
	case RIL_PINSTATE_ENABLED_PERM_BLOCKED:
		return "ENABLED_PERM_BLOCKED";
	default:
		return "<INVALID>";
	}
}

const char *ril_radio_state_to_string(int radio_state)
{
	switch (radio_state) {
	case RADIO_STATE_OFF:
		return "OFF";
	case RADIO_STATE_UNAVAILABLE:
		return "UNAVAILABLE";
	case RADIO_STATE_SIM_NOT_READY:
		return "SIM_NOT_READY";
	case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
		return "SIM_LOCKED_OR_ABSENT";
	case RADIO_STATE_SIM_READY:
		return "SIM_READY";
	case RADIO_STATE_ON:
		return "ON";
	default:
		return "<INVALID>";
	}
}

const char *ril_request_id_to_string(int req)
{
	switch (req) {
	case RIL_REQUEST_GET_SIM_STATUS:
		return "RIL_REQUEST_GET_SIM_STATUS";
	case RIL_REQUEST_ENTER_SIM_PIN:
		return "RIL_REQUEST_ENTER_SIM_PIN";
	case RIL_REQUEST_ENTER_SIM_PUK:
		return "RIL_REQUEST_ENTER_SIM_PUK";
	case RIL_REQUEST_ENTER_SIM_PIN2:
		return "RIL_REQUEST_ENTER_SIM_PIN2";
	case RIL_REQUEST_ENTER_SIM_PUK2:
		return "RIL_REQUEST_ENTER_SIM_PUK2";
	case RIL_REQUEST_CHANGE_SIM_PIN:
		return "RIL_REQUEST_CHANGE_SIM_PIN";
	case RIL_REQUEST_CHANGE_SIM_PIN2:
		return "RIL_REQUEST_CHANGE_SIM_PIN2";
	case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
		return "RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION";
	case RIL_REQUEST_GET_CURRENT_CALLS:
		return "RIL_REQUEST_GET_CURRENT_CALLS";
	case RIL_REQUEST_DIAL:
		return "RIL_REQUEST_DIAL";
	case RIL_REQUEST_GET_IMSI:
		return "RIL_REQUEST_GET_IMSI";
	case RIL_REQUEST_HANGUP:
		return "RIL_REQUEST_HANGUP";
	case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
		return "RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND";
	case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
		return "RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND";
	case RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE:
		return "RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE";
	case RIL_REQUEST_CONFERENCE:
		return "RIL_REQUEST_CONFERENCE";
	case RIL_REQUEST_UDUB:
		return "RIL_REQUEST_UDUB";
	case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
		return "RIL_REQUEST_LAST_CALL_FAIL_CAUSE";
	case RIL_REQUEST_SIGNAL_STRENGTH:
		return "RIL_REQUEST_SIGNAL_STRENGTH";
	case RIL_REQUEST_VOICE_REGISTRATION_STATE:
		return "RIL_REQUEST_VOICE_REGISTRATION_STATE";
	case RIL_REQUEST_DATA_REGISTRATION_STATE:
		return "RIL_REQUEST_DATA_REGISTRATION_STATE";
	case RIL_REQUEST_OPERATOR:
		return "RIL_REQUEST_OPERATOR";
	case RIL_REQUEST_RADIO_POWER:
		return "RIL_REQUEST_RADIO_POWER";
	case RIL_REQUEST_DTMF:
		return "RIL_REQUEST_DTMF";
	case RIL_REQUEST_SEND_SMS:
		return "RIL_REQUEST_SEND_SMS";
	case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
		return "RIL_REQUEST_SEND_SMS_EXPECT_MORE";
	case RIL_REQUEST_SETUP_DATA_CALL:
		return "RIL_REQUEST_SETUP_DATA_CALL";
	case RIL_REQUEST_SIM_IO:
		return "RIL_REQUEST_SIM_IO";
	case RIL_REQUEST_SEND_USSD:
		return "RIL_REQUEST_SEND_USSD";
	case RIL_REQUEST_CANCEL_USSD:
		return "RIL_REQUEST_CANCEL_USSD";
	case RIL_REQUEST_GET_CLIR:
		return "RIL_REQUEST_GET_CLIR";
	case RIL_REQUEST_SET_CLIR:
		return "RIL_REQUEST_SET_CLIR";
	case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
		return "RIL_REQUEST_QUERY_CALL_FORWARD_STATUS";
	case RIL_REQUEST_SET_CALL_FORWARD:
		return "RIL_REQUEST_SET_CALL_FORWARD";
	case RIL_REQUEST_QUERY_CALL_WAITING:
		return "RIL_REQUEST_QUERY_CALL_WAITING";
	case RIL_REQUEST_SET_CALL_WAITING:
		return "RIL_REQUEST_SET_CALL_WAITING";
	case RIL_REQUEST_SMS_ACKNOWLEDGE :
		return "RIL_REQUEST_SMS_ACKNOWLEDGE ";
	case RIL_REQUEST_GET_IMEI:
		return "RIL_REQUEST_GET_IMEI";
	case RIL_REQUEST_GET_IMEISV:
		return "RIL_REQUEST_GET_IMEISV";
	case RIL_REQUEST_ANSWER:
		return "RIL_REQUEST_ANSWER";
	case RIL_REQUEST_DEACTIVATE_DATA_CALL:
		return "RIL_REQUEST_DEACTIVATE_DATA_CALL";
	case RIL_REQUEST_QUERY_FACILITY_LOCK:
		return "RIL_REQUEST_QUERY_FACILITY_LOCK";
	case RIL_REQUEST_SET_FACILITY_LOCK:
		return "RIL_REQUEST_SET_FACILITY_LOCK";
	case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
		return "RIL_REQUEST_CHANGE_BARRING_PASSWORD";
	case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
		return "RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE";
	case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
		return "RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC";
	case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
		return "RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL";
	case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
		return "RIL_REQUEST_QUERY_AVAILABLE_NETWORKS";
	case RIL_REQUEST_DTMF_START:
		return "RIL_REQUEST_DTMF_START";
	case RIL_REQUEST_DTMF_STOP:
		return "RIL_REQUEST_DTMF_STOP";
	case RIL_REQUEST_BASEBAND_VERSION:
		return "RIL_REQUEST_BASEBAND_VERSION";
	case RIL_REQUEST_SEPARATE_CONNECTION:
		return "RIL_REQUEST_SEPARATE_CONNECTION";
	case RIL_REQUEST_SET_MUTE:
		return "RIL_REQUEST_SET_MUTE";
	case RIL_REQUEST_GET_MUTE:
		return "RIL_REQUEST_GET_MUTE";
	case RIL_REQUEST_QUERY_CLIP:
		return "RIL_REQUEST_QUERY_CLIP";
	case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
		return "RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE";
	case RIL_REQUEST_DATA_CALL_LIST:
		return "RIL_REQUEST_DATA_CALL_LIST";
	case RIL_REQUEST_RESET_RADIO:
		return "RIL_REQUEST_RESET_RADIO";
	case RIL_REQUEST_OEM_HOOK_RAW:
		return "RIL_REQUEST_OEM_HOOK_RAW";
	case RIL_REQUEST_OEM_HOOK_STRINGS:
		return "RIL_REQUEST_OEM_HOOK_STRINGS";
	case RIL_REQUEST_SCREEN_STATE:
		return "RIL_REQUEST_SCREEN_STATE";
	case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
		return "RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION";
	case RIL_REQUEST_WRITE_SMS_TO_SIM:
		return "RIL_REQUEST_WRITE_SMS_TO_SIM";
	case RIL_REQUEST_DELETE_SMS_ON_SIM:
		return "RIL_REQUEST_DELETE_SMS_ON_SIM";
	case RIL_REQUEST_SET_BAND_MODE:
		return "RIL_REQUEST_SET_BAND_MODE";
	case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
		return "RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE";
	case RIL_REQUEST_STK_GET_PROFILE:
		return "RIL_REQUEST_STK_GET_PROFILE";
	case RIL_REQUEST_STK_SET_PROFILE:
		return "RIL_REQUEST_STK_SET_PROFILE";
	case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
		return "RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND";
	case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
		return "RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE";
	case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM:
		return "RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM";
	case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
		return "RIL_REQUEST_EXPLICIT_CALL_TRANSFER";
	case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
		return "RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE";
	case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
		return "RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE";
	case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
		return "RIL_REQUEST_GET_NEIGHBORING_CELL_IDS";
	case RIL_REQUEST_SET_LOCATION_UPDATES:
		return "RIL_REQUEST_SET_LOCATION_UPDATES";
	case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
		return "RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE";
	case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
		return "RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE";
	case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:
		return "RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE";
	case RIL_REQUEST_SET_TTY_MODE:
		return "RIL_REQUEST_SET_TTY_MODE";
	case RIL_REQUEST_QUERY_TTY_MODE:
		return "RIL_REQUEST_QUERY_TTY_MODE";
	case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
		return "RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE";
	case RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE:
		return "RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE";
	case RIL_REQUEST_CDMA_FLASH:
		return "RIL_REQUEST_CDMA_FLASH";
	case RIL_REQUEST_CDMA_BURST_DTMF:
		return "RIL_REQUEST_CDMA_BURST_DTMF";
	case RIL_REQUEST_CDMA_VALIDATE_AND_WRITE_AKEY:
		return "RIL_REQUEST_CDMA_VALIDATE_AND_WRITE_AKEY";
	case RIL_REQUEST_CDMA_SEND_SMS:
		return "RIL_REQUEST_CDMA_SEND_SMS";
	case RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE:
		return "RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE";
	case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
		return "RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG";
	case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
		return "RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG";
	case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
		return "RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION";
	case RIL_REQUEST_CDMA_GET_BROADCAST_SMS_CONFIG:
		return "RIL_REQUEST_CDMA_GET_BROADCAST_SMS_CONFIG";
	case RIL_REQUEST_CDMA_SET_BROADCAST_SMS_CONFIG:
		return "RIL_REQUEST_CDMA_SET_BROADCAST_SMS_CONFIG";
	case RIL_REQUEST_CDMA_SMS_BROADCAST_ACTIVATION:
		return "RIL_REQUEST_CDMA_SMS_BROADCAST_ACTIVATION";
	case RIL_REQUEST_CDMA_SUBSCRIPTION:
		return "RIL_REQUEST_CDMA_SUBSCRIPTION";
	case RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM:
		return "RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM";
	case RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM:
		return "RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM";
	case RIL_REQUEST_DEVICE_IDENTITY:
		return "RIL_REQUEST_DEVICE_IDENTITY";
	case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
		return "RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE";
	case RIL_REQUEST_GET_SMSC_ADDRESS:
		return "RIL_REQUEST_GET_SMSC_ADDRESS";
	case RIL_REQUEST_SET_SMSC_ADDRESS:
		return "RIL_REQUEST_SET_SMSC_ADDRESS";
	case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:
		return "RIL_REQUEST_REPORT_SMS_MEMORY_STATUS";
	case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
		return "RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING";
	case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
		return "RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE";
	case RIL_REQUEST_ISIM_AUTHENTICATION:
		return "RIL_REQUEST_ISIM_AUTHENTICATION";
	case RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU:
		return "RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU";
	case RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS:
		return "RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS";
	default:
		return "<INVALID>";
	}
}

const char *ril_unsol_request_to_string(int request)
{
	switch(request) {
        case RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED: return "UNSOL_RESPONSE_RADIO_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED: return "UNSOL_RESPONSE_CALL_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED: return "UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_NEW_SMS: return "UNSOL_RESPONSE_NEW_SMS";
        case RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT: return "UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT";
        case RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM: return "UNSOL_RESPONSE_NEW_SMS_ON_SIM";
        case RIL_UNSOL_ON_USSD: return "UNSOL_ON_USSD";
        case RIL_UNSOL_ON_USSD_REQUEST: return "UNSOL_ON_USSD_REQUEST(obsolete)";
        case RIL_UNSOL_NITZ_TIME_RECEIVED: return "UNSOL_NITZ_TIME_RECEIVED";
        case RIL_UNSOL_SIGNAL_STRENGTH: return "UNSOL_SIGNAL_STRENGTH";
        case RIL_UNSOL_SUPP_SVC_NOTIFICATION: return "UNSOL_SUPP_SVC_NOTIFICATION";
        case RIL_UNSOL_STK_SESSION_END: return "UNSOL_STK_SESSION_END";
        case RIL_UNSOL_STK_PROACTIVE_COMMAND: return "UNSOL_STK_PROACTIVE_COMMAND";
        case RIL_UNSOL_STK_EVENT_NOTIFY: return "UNSOL_STK_EVENT_NOTIFY";
        case RIL_UNSOL_STK_CALL_SETUP: return "UNSOL_STK_CALL_SETUP";
        case RIL_UNSOL_SIM_SMS_STORAGE_FULL: return "UNSOL_SIM_SMS_STORAGE_FUL";
        case RIL_UNSOL_SIM_REFRESH: return "UNSOL_SIM_REFRESH";
        case RIL_UNSOL_DATA_CALL_LIST_CHANGED: return "UNSOL_DATA_CALL_LIST_CHANGED";
        case RIL_UNSOL_CALL_RING: return "UNSOL_CALL_RING";
        case RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED: return "UNSOL_RESPONSE_SIM_STATUS_CHANGED";
        case RIL_UNSOL_RESPONSE_CDMA_NEW_SMS: return "UNSOL_NEW_CDMA_SMS";
        case RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS: return "UNSOL_NEW_BROADCAST_SMS";
        case RIL_UNSOL_CDMA_RUIM_SMS_STORAGE_FULL: return "UNSOL_CDMA_RUIM_SMS_STORAGE_FULL";
        case RIL_UNSOL_RESTRICTED_STATE_CHANGED: return "UNSOL_RESTRICTED_STATE_CHANGED";
        case RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE: return "UNSOL_ENTER_EMERGENCY_CALLBACK_MODE";
        case RIL_UNSOL_CDMA_CALL_WAITING: return "UNSOL_CDMA_CALL_WAITING";
        case RIL_UNSOL_CDMA_OTA_PROVISION_STATUS: return "UNSOL_CDMA_OTA_PROVISION_STATUS";
        case RIL_UNSOL_CDMA_INFO_REC: return "UNSOL_CDMA_INFO_REC";
        case RIL_UNSOL_OEM_HOOK_RAW: return "UNSOL_OEM_HOOK_RAW";
        case RIL_UNSOL_RINGBACK_TONE: return "UNSOL_RINGBACK_TONE";
        case RIL_UNSOL_RESEND_INCALL_MUTE: return "UNSOL_RESEND_INCALL_MUTE";
        case RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED: return "UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED";
        case RIL_UNSOL_CDMA_PRL_CHANGED: return "UNSOL_CDMA_PRL_CHANGED";
        case RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE: return "UNSOL_EXIT_EMERGENCY_CALLBACK_MODE";
        case RIL_UNSOL_RIL_CONNECTED: return "UNSOL_RIL_CONNECTED";
        default:
		return "<unknown request>";
	}
}

void g_ril_util_debug_chat(gboolean in, const char *str, gsize len,
				GRilDebugFunc debugf, gpointer user_data)
{
	char type = in ? '<' : '>';
	gsize escaped = 2; /* Enough for '<', ' ' */
	char *escaped_str;
	const char *esc = "<ESC>";
	gsize esc_size = strlen(esc);
	const char *ctrlz = "<CtrlZ>";
	gsize ctrlz_size = strlen(ctrlz);
	gsize i;

	if (debugf == NULL || !len)
		return;

	for (i = 0; i < len; i++) {
		char c = str[i];

		if (g_ascii_isprint(c))
			escaped += 1;
		else if (c == '\r' || c == '\t' || c == '\n')
			escaped += 2;
		else if (c == 26)
			escaped += ctrlz_size;
		else if (c == 25)
			escaped += esc_size;
		else
			escaped += 4;
	}

	escaped_str = g_try_malloc(escaped + 1);
	if (escaped_str == NULL)
		return;

	escaped_str[0] = type;
	escaped_str[1] = ' ';
	escaped_str[2] = '\0';
	escaped_str[escaped] = '\0';

	for (escaped = 2, i = 0; i < len; i++) {
		unsigned char c = str[i];

		switch (c) {
		case '\r':
			escaped_str[escaped++] = '\\';
			escaped_str[escaped++] = 'r';
			break;
		case '\t':
			escaped_str[escaped++] = '\\';
			escaped_str[escaped++] = 't';
			break;
		case '\n':
			escaped_str[escaped++] = '\\';
			escaped_str[escaped++] = 'n';
			break;
		case 26:
			strncpy(&escaped_str[escaped], ctrlz, ctrlz_size);
			escaped += ctrlz_size;
			break;
		case 25:
			strncpy(&escaped_str[escaped], esc, esc_size);
			escaped += esc_size;
			break;
		default:
			if (g_ascii_isprint(c))
				escaped_str[escaped++] = c;
			else {
				escaped_str[escaped++] = '\\';
				escaped_str[escaped++] = '0' + ((c >> 6) & 07);
				escaped_str[escaped++] = '0' + ((c >> 3) & 07);
				escaped_str[escaped++] = '0' + (c & 07);
			}
		}
	}

	debugf(escaped_str, user_data);
	g_free(escaped_str);
}

void g_ril_util_debug_dump(gboolean in, const unsigned char *buf, gsize len,
				GRilDebugFunc debugf, gpointer user_data)
{
	char type = in ? '<' : '>';
	GString *str;
	gsize i;

	if (debugf == NULL || !len)
		return;

	str = g_string_sized_new(1 + (len * 2));
	if (str == NULL)
		return;

	g_string_append_c(str, type);

	for (i = 0; i < len; i++)
		g_string_append_printf(str, " %02x", buf[i]);

	debugf(str->str, user_data);
	g_string_free(str, TRUE);
}

void g_ril_util_debug_hexdump(gboolean in, const unsigned char *buf, gsize len,
				GRilDebugFunc debugf, gpointer user_data)
{
	static const char hexdigits[] = "0123456789abcdef";
	char str[68];
	gsize i;

	if (debugf == NULL || !len)
		return;

	str[0] = in ? '<' : '>';

	for (i = 0; i < len; i++) {
		str[((i % 16) * 3) + 1] = ' ';
		str[((i % 16) * 3) + 2] = hexdigits[buf[i] >> 4];
		str[((i % 16) * 3) + 3] = hexdigits[buf[i] & 0xf];
		str[(i % 16) + 51] = g_ascii_isprint(buf[i]) ? buf[i] : '.';

		if ((i + 1) % 16 == 0) {
			str[49] = ' ';
			str[50] = ' ';
			str[67] = '\0';
			debugf(str, user_data);
			str[0] = ' ';
		}
	}

	if (i % 16 > 0) {
		gsize j;
		for (j = (i % 16); j < 16; j++) {
			str[(j * 3) + 1] = ' ';
			str[(j * 3) + 2] = ' ';
			str[(j * 3) + 3] = ' ';
			str[j + 51] = ' ';
		}
		str[49] = ' ';
		str[50] = ' ';
		str[67] = '\0';
		debugf(str, user_data);
	}
}

gboolean g_ril_util_setup_io(GIOChannel *io, GIOFlags flags)
{
	GIOFlags io_flags;

	if (g_io_channel_set_encoding(io, NULL, NULL) != G_IO_STATUS_NORMAL)
		return FALSE;

	g_io_channel_set_buffered(io, FALSE);

	if (flags & G_IO_FLAG_SET_MASK) {
		io_flags = g_io_channel_get_flags(io);

		io_flags |= (flags & G_IO_FLAG_SET_MASK);

		if (g_io_channel_set_flags(io, io_flags, NULL) !=
							G_IO_STATUS_NORMAL)
			return FALSE;
	}

	g_io_channel_set_close_on_unref(io, TRUE);

	return TRUE;
}
