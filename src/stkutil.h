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

/*
 * TS 101.220, Section 7.2, Card Application Toolkit assigned templates,
 * These are the same as 3GPP 11.14 Sections 13.1 and 13.2
 */
enum stk_envelope_type {
	STK_ENVELOPE_TYPE_SMS_PP_DOWNLOAD =	0xD1,
	STK_ENVELOPE_TYPE_CBS_PP_DOWNLOAD =	0xD2,
	STK_ENVELOPE_TYPE_MENU_SELECTION =	0xD3,
	STK_ENVELOPE_TYPE_CALL_CONTROL =	0xD4,
	STK_ENVELOPE_TYPE_MO_SMS_CONTROL =	0xD5,
	STK_ENVELOPE_TYPE_EVENT_DOWNLOAD =	0xD6,
	STK_ENVELOPE_TYPE_TIMER_EXPIRATION =	0xD7,
	STK_ENVELOPE_TYPE_USSD_DOWNLOAD =	0xD9,
	STK_ENVELOPE_TYPE_MMS_TRANSFER_STATUS =	0xDA,
	STK_ENVELOPE_TYPE_MMS_NOTIFICATION =	0xDB,
	STK_ENVELOPE_TYPE_TERMINAL_APP =	0xDC,
	STK_ENVELOPE_TYPE_GEOLOCATION_REPORT =	0xDD,
};

/* TS 102.223 Section 9.4 */
enum stk_command_type {
	STK_COMMAND_TYPE_REFRESH =			0x01,
	STK_COMMAND_TYPE_MORE_TIME =			0x02,
	STK_COMMAND_TYPE_POLL_INTERVAL =		0x03,
	STK_COMMAND_TYPE_POLLING_OFF =			0x04,
	STK_COMMAND_TYPE_SETUP_EVENT_LIST =		0x05,
	STK_COMMAND_TYPE_SETUP_CALL =			0x10,
	STK_COMMAND_TYPE_SEND_SS =			0x11,
	STK_COMMAND_TYPE_SEND_USSD =			0x12,
	STK_COMMAND_TYPE_SEND_SMS =			0x13,
	STK_COMMAND_TYPE_SEND_DTMF =			0x14,
	STK_COMMAND_TYPE_LAUNCH_BROWSER =		0x15,
	STK_COMMAND_TYPE_GEOGRAPICAL_LOCATION_REQUEST =	0x16,
	STK_COMMAND_TYPE_PLAY_TONE =			0x20,
	STK_COMMAND_TYPE_DISPLAY_TEXT =			0x21,
	STK_COMMAND_TYPE_GET_INKEY =			0x22,
	STK_COMMAND_TYPE_GET_INPUT =			0x23,
	STK_COMMAND_TYPE_SELECT_ITEM =			0x24,
	STK_COMMAND_TYPE_SETUP_MENU =			0x25,
	STK_COMMAND_TYPE_PROVIDE_LOCAL_INFO =		0x26,
	STK_COMMAND_TYPE_TIMER_MANAGEMENT =		0x27,
	STK_COMMAND_TYPE_SETUP_IDLE_MODE_TEXT =		0x28,
	STK_COMMAND_TYPE_PERFORM_CARD_APDU =		0x30,
	STK_COMMAND_TYPE_POWER_ON_CARD =		0x31,
	STK_COMMAND_TYPE_POWER_OFF_CARD =		0x32,
	STK_COMMAND_TYPE_GET_READER_STATUS =		0x33,
	STK_COMMAND_TYPE_RUN_AT_COMMAND =		0x34,
	STK_COMMAND_TYPE_LANGUAGE_NOTIFICATION =	0x35,
	STK_COMMAND_TYPE_OPEN_CHANNEL =			0x40,
	STK_COMMAND_TYPE_CLOSE_CHANNEL =		0x41,
	STK_COMMAND_TYPE_RECEIVE_DATA =			0x42,
	STK_COMMAND_TYPE_SEND_DATA =			0x43,
	STK_COMMAND_TYPE_GET_CHANNEL_STATUS =		0x44,
	STK_COMMAND_TYPE_SERVICE_SEARCH =		0x45,
	STK_COMMAND_TYPE_GET_SERVICE_INFO =		0x46,
	STK_COMMAND_TYPE_DECLARE_SERVICE =		0x47,
	STK_COMMAND_TYPE_SET_FRAMES =			0x50,
	STK_COMMAND_TYPE_GET_FRAMES_STATUS =		0x51,
	STK_COMMAND_TYPE_RETRIEVE_MMS =			0x60,
	STK_COMMAND_TYPE_SUBMIT_MMS =			0x61,
	STK_COMMAND_TYPE_DISPLAY_MMS =			0x62,
	STK_COMMAND_TYPE_ACTIVATE =			0x70,
	STK_COMMAND_TYPE_END_SESSION =			0x81,
};

enum stk_data_object_type {
	STK_DATA_OBJECT_TYPE_INVALID =				0x00,
	STK_DATA_OBJECT_TYPE_COMMAND_DETAILS =			0x01,
	STK_DATA_OBJECT_TYPE_DEVICE_IDENTITIES =		0x02,
	STK_DATA_OBJECT_TYPE_RESULT =				0x03,
	STK_DATA_OBJECT_TYPE_DURATION =				0x04,
	STK_DATA_OBJECT_TYPE_ALPHA_ID =				0x05,
	STK_DATA_OBJECT_TYPE_ADDRESS =				0x06,
	STK_DATA_OBJECT_TYPE_CCP =				0x07,
	STK_DATA_OBJECT_TYPE_SUBADDRESS =			0x08,
	STK_DATA_OBJECT_TYPE_SS_STRING =			0x09,
	STK_DATA_OBJECT_TYPE_USSD_STRING =			0x0A,
	STK_DATA_OBJECT_TYPE_GSM_SMS_TPDU =			0x0B,
	STK_DATA_OBJECT_TYPE_CBS_PAGE =				0x0C,
	STK_DATA_OBJECT_TYPE_TEXT =				0x0D,
	STK_DATA_OBJECT_TYPE_TONE =				0x0E,
	STK_DATA_OBJECT_TYPE_ITEM =				0x0F,
	STK_DATA_OBJECT_TYPE_ITEM_ID =				0x10,
	STK_DATA_OBJECT_TYPE_RESPONSE_LENGTH =			0x11,
	STK_DATA_OBJECT_TYPE_FILE_LIST =			0x12,
	STK_DATA_OBJECT_TYPE_LOCATION_INFO =			0x13,
	STK_DATA_OBJECT_TYPE_IMEI =				0x14,
	STK_DATA_OBJECT_TYPE_HELP_REQUEST =			0x15,
	STK_DATA_OBJECT_TYPE_NETWORK_MEASUREMENT_RESULTS =	0x16,
	STK_DATA_OBJECT_TYPE_DEFAULT_TEXT =			0x17,
	STK_DATA_OBJECT_TYPE_ITEMS_NEXT_ACTION_INDICATOR =	0x18,
	STK_DATA_OBJECT_TYPE_EVENT_LIST =			0x19,
	STK_DATA_OBJECT_TYPE_CAUSE =				0x1A,
	STK_DATA_OBJECT_TYPE_LOCATION_STATUS =			0x1B,
	STK_DATA_OBJECT_TYPE_TRANSACTION_ID =			0x1C,
	STK_DATA_OBJECT_TYPE_BCCH_CHANNEL_LIST =		0x1D,
	STK_DATA_OBJECT_TYPE_ICON_ID =				0x1E,
	STK_DATA_OBJECT_TYPE_ITEM_ICON_ID_LIST =		0x1F,
	STK_DATA_OBJECT_TYPE_CARD_READER_STATUS =		0x20,
	STK_DATA_OBJECT_TYPE_CARD_ATR =				0x21,
	STK_DATA_OBJECT_TYPE_C_APDU =				0x22,
	STK_DATA_OBJECT_TYPE_R_APDU =				0x23,
	STK_DATA_OBJECT_TYPE_TIMER_ID =				0x24,
	STK_DATA_OBJECT_TYPE_TIMER_VALUE =			0x25,
	STK_DATA_OBJECT_TYPE_DATETIME_TIMEZONE =		0x26,
	STK_DATA_OBJECT_TYPE_CALL_CONTROL_REQUESTED_ACTION =	0x27,
	STK_DATA_OBJECT_TYPE_AT_COMMAND =			0x28,
	STK_DATA_OBJECT_TYPE_AT_RESPONSE =			0x29,
	STK_DATA_OBJECT_TYPE_BC_REPEAT_INDICATOR =		0x2A,
	STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE =		0x2B,
	STK_DATA_OBJECT_TYPE_DTMF_STRING =			0x2C,
	STK_DATA_OBJECT_TYPE_LANGUAGE =				0x2D,
	STK_DATA_OBJECT_TYPE_TIMING_ADVANCE =			0x2E,
	STK_DATA_OBJECT_TYPE_AID =				0x2F,
	STK_DATA_OBJECT_TYPE_BROWSER_ID =			0x30,
	STK_DATA_OBJECT_TYPE_URL =				0x31,
	STK_DATA_OBJECT_TYPE_BEARER =				0x32,
	STK_DATA_OBJECT_TYPE_PROVISIONING_FILE_REF =		0x33,
	STK_DATA_OBJECT_TYPE_BROWSER_TERMINATION_CAUSE =	0x34,
	STK_DATA_OBJECT_TYPE_BEARER_DESCRIPTION =		0x35,
	STK_DATA_OBJECT_TYPE_CHANNEL_DATA =			0x36,
	STK_DATA_OBJECT_TYPE_CHANNEL_DATA_LENGTH =		0x37,
	STK_DATA_OBJECT_TYPE_CHANNEL_STATUS =			0x38,
	STK_DATA_OBJECT_TYPE_BUFFER_SIZE =			0x39,
	STK_DATA_OBJECT_TYPE_CARD_READER_ID =			0x3A,
	STK_DATA_OBJECT_TYPE_FILE_UPDATE_INFO =			0x3B,
	STK_DATA_OBJECT_TYPE_UICC_TE_INTERFACE =		0x3C,
	STK_DATA_OBJECT_TYPE_OTHER_ADDRESS =			0x3E,
	STK_DATA_OBJECT_TYPE_ACCESS_TECHNOLOGY =		0x3F,
	STK_DATA_OBJECT_TYPE_DISPLAY_PARAMETERS =		0x40,
	STK_DATA_OBJECT_TYPE_SERVICE_RECORD =			0x41,
	STK_DATA_OBJECT_TYPE_DEVICE_FILTER =			0x42,
	STK_DATA_OBJECT_TYPE_SERVICE_SEARCH =			0x43,
	STK_DATA_OBJECT_TYPE_ATTRIBUTE_INFO =			0x44,
	STK_DATA_OBJECT_TYPE_SERVICE_AVAILABILITY =		0x45,
	STK_DATA_OBJECT_TYPE_ESN =				0x46,
	STK_DATA_OBJECT_TYPE_NETWORK_ACCESS_NAME =		0x47,
	STK_DATA_OBJECT_TYPE_CDMA_SMS_TPDU =			0x48,
	STK_DATA_OBJECT_TYPE_REMOTE_ENTITY_ADDRESS =		0x49,
	STK_DATA_OBJECT_TYPE_I_WLAN_ID_TAG =			0x4A,
	STK_DATA_OBJECT_TYPE_I_WLAN_ACCESS_STATUS =		0x4B,
	STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE =			0x50,
	STK_DATA_OBJECT_TYPE_ITEM_TEXT_ATTRIBUTE_LIST =		0x51,
	STK_DATA_OBJECT_TYPE_PDP_ACTIVATION_PARAMETER =		0x52,
	STK_DATA_OBJECT_TYPE_IMEISV =				0x62,
	STK_DATA_OBJECT_TYPE_BATTERY_STATE =			0x63,
	STK_DATA_OBJECT_TYPE_BROWSING_STATUS =			0x64,
	STK_DATA_OBJECT_TYPE_NETWORK_SEARCH_MODE =		0x65,
	STK_DATA_OBJECT_TYPE_FRAME_LAYOUT =			0x66,
	STK_DATA_OBJECT_TYPE_FRAMES_INFO =			0x67,
	STK_DATA_OBJECT_TYPE_FRAME_ID =				0x68,
	STK_DATA_OBJECT_TYPE_UTRAN_MEASUREMENT_QUALIFIER =	0x69,
	STK_DATA_OBJECT_TYPE_MMS_REFERENCE =			0x6A,
	STK_DATA_OBJECT_TYPE_MMS_ID =				0x6B,
	STK_DATA_OBJECT_TYPE_MMS_TRANSFER_STATUS =		0x6C,
	STK_DATA_OBJECT_TYPE_MEID =				0x6D,
	STK_DATA_OBJECT_TYPE_MMS_CONTENT_ID =			0x6E,
	STK_DATA_OBJECT_TYPE_MMS_NOTIFICATION =			0x6F,
	STK_DATA_OBJECT_TYPE_LAST_ENVELOPE =			0x70,
	STK_DATA_OBJECT_TYPE_REGISTRY_APPLICATION_DATA =	0x71,
	STK_DATA_OBJECT_TYPE_ROUTING_AREA_INFO =		0x73,
	STK_DATA_OBJECT_TYPE_UPDATE_ATTACH_TYPE =		0x74,
	STK_DATA_OBJECT_TYPE_REJECTION_CAUSE_CODE =		0x75,
	STK_DATA_OBJECT_TYPE_NMEA_SENTENCE =			0x78,
	STK_DATA_OBJECT_TYPE_PLMN_LIST =			0x79,
	STK_DATA_OBJECT_TYPE_BROADCAST_NETWORK_INFO =		0x7A,
	STK_DATA_OBJECT_TYPE_ACTIVATE_DESCRIPTOR =		0x7B,
	STK_DATA_OBJECT_TYPE_EPS_PDN_CONN_ACTIVATION_REQ =	0x7C,
	STK_DATA_OBJECT_TYPE_TRACKING_AREA_ID =			0x7D,
};

enum stk_device_identity_type {
	STK_DEVICE_IDENTITY_TYPE_KEYPAD =		0x01,
	STK_DEVICE_IDENTITY_TYPE_DISPLAY =		0x02,
	STK_DEVICE_IDENTITY_TYPE_EARPIECE =		0x03,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_0 =	0x10,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_1 =	0x11,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_2 =	0x12,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_3 =	0x13,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_4 =	0x14,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_5 =	0x15,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_6 =	0x16,
	STK_DEVICE_IDENTITY_TYPE_CARD_READER_7 =	0x17,
	STK_DEVICE_IDENTITY_TYPE_CHANNEL_1 =		0x21,
	STK_DEVICE_IDENTITY_TYPE_CHANNEL_7 =		0x27,
	STK_DEVICE_IDENTITY_TYPE_UICC =			0x81,
	STK_DEVICE_IDENTITY_TYPE_TERMINAL =		0x82,
	STK_DEVICE_IDENTITY_TYPE_NETWORK =		0x83,
};

enum stk_qualifier_get_reader_status_type {
	STK_QUALIFIER_TYPE_CARD_READER_STATUS =		0x00,
	STK_QUALIFIER_TYPE_CARD_READER_ID =		0x01,
};

enum stk_duration_type {
	STK_DURATION_TYPE_MINUTES =		0x00,
	STK_DURATION_TYPE_SECONDS =		0x01,
	STK_DURATION_TYPE_SECOND_TENTHS =	0x02,
};

/* Defined according to TS 102.223 Section 8.12 */
enum stk_result_type {
	/* 0x00 to 0x1F are used to indicate that command has been performed */
	STK_RESULT_TYPE_SUCCESS =			0x00,
	STK_RESULT_TYPE_PARTIAL =			0x01,
	STK_RESULT_TYPE_MISSING_INFO =			0x02,
	STK_RESULT_TYPE_REFRESH_WITH_EFS =		0x03,
	STK_RESULT_TYPE_NO_ICON =			0x04,
	STK_RESULT_TYPE_CALL_CONTROL =			0x05,
	STK_RESULT_TYPE_NO_SERVICE =			0x06,
	STK_RESULT_TYPE_MODIFED =			0x07,
	STK_RESULT_TYPE_REFRES_NO_NAA =			0x08,
	STK_RESULT_TYPE_NO_TONE =			0x09,
	STK_RESULT_TYPE_USER_TERMINATED =		0x10,
	STK_RESULT_TYPE_GO_BACK =			0x11,
	STK_RESULT_TYPE_NO_RESPONSE =			0x12,
	STK_RESULT_TYPE_HELP_REQUESTED =		0x13,
	STK_RESULT_TYPE_USSD_OR_SS_USER_TERMINATION =	0x14,

	/* 0x20 to 0x2F are used to indicate that SIM should retry */
	STK_RESULT_TYPE_TERMINAL_BUSY =			0x20,
	STK_RESULT_TYPE_NETWORK_UNAVAILABLE =		0x21,
	STK_RESULT_TYPE_USER_REJECT =			0x22,
	STK_RESULT_TYPE_USER_CANCEL =			0x23,
	STK_RESULT_TYPE_TIMER_CONFLICT =		0x24,
	STK_RESULT_TYPE_CALL_CONTROL_TEMPORARY =	0x25,
	STK_RESULT_TYPE_BROWSER_TEMPORARY =		0x26,
	STK_RESULT_TYPE_MMS_TEMPORARY =			0x27,

	/* 0x30 to 0x3F are used to indicate permanent problems */
	STK_RESULT_TYPE_NOT_CAPABLE =			0x30,
	STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD =	0x31,
	STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD =		0x32,
	STK_RESULT_TYPE_COMMAND_ID_UNKNOWN =		0x33,
	STK_RESULT_TYPE_SS_RETURN_ERROR =		0x34,
	STK_RESULT_TYPE_SMS_RP_ERROR =			0x35,
	STK_RESULT_TYPE_MINIMUM_NOT_MET =		0x36,
	STK_RESULT_TYPE_USSD_RETURN_ERROR =		0x37,
	STK_RESULT_TYPE_CALL_CONTROL_PERMANENT =	0x39,
	STK_RESULT_TYPE_BIP_ERROR =			0x3A,
	STK_RESULT_TYPE_ACCESS_TECHNOLOGY_ERROR =	0x3B,
	STK_RESULT_TYPE_FRAMES_ERROR =			0x3C,
	STK_RESULT_TYPE_MMS_ERROR =			0x3D,
};

/* Defined according to TS 102.223 Section 8.12.2 */
enum stk_result_addnl_me_pb {
	STK_RESULT_ADDNL_ME_PB_NO_SPECIFIC_CAUSE =	0x00,
	STK_RESULT_ADDNL_ME_PB_SCREEN_BUSY =		0x01,
	STK_RESULT_ADDNL_ME_PB_BUSY_ON_CALL =		0x02,
	STK_RESULT_ADDNL_ME_PB_SS_BUSY =		0x03,
	STK_RESULT_ADDNL_ME_PB_NO_SERVICE =		0x04,
	STK_RESULT_ADDNL_ME_PB_NO_ACCESS =		0x05,
	STK_RESULT_ADDNL_ME_PB_NO_RADIO_RESOURCE =	0x06,
	STK_RESULT_ADDNL_ME_PB_NOT_IN_SPEECH_CALL =	0x07,
	STK_RESULT_ADDNL_ME_PB_USSD_BUSY =		0x08,
	STK_RESULT_ADDNL_ME_PB_BUSY_ON_SEND_DTMF =	0x09,
	STK_RESULT_ADDNL_ME_PB_NO_NAA_ACTIVE =		0x0A
};

/* Defined according to TS 31.111 Section 8.12.4 */
enum stk_result_addnl_ss_pb {
	STK_RESULT_ADDNL_SS_PB_NO_SPECIFIC_CAUSE =	0x00
};

/* Defined according to TS 31.111 Section 8.12.4 */
enum stk_result_addnl_bip_pb {
	STK_RESULT_ADDNL_BIP_PB_NO_SPECIFIC_CAUSE =		0x00,
	STK_RESULT_ADDNL_BIP_PB_NO_CHANNEL_AVAIL =		0x01,
	STK_RESULT_ADDNL_BIP_PB_CHANNEL_CLOSED =		0x02,
	STK_RESULT_ADDNL_BIP_PB_CHANNEL_ID_NOT_VALID =		0x03,
	STK_RESULT_ADDNL_BIP_PB_BUFFER_SIZE_NOT_AVAIL =		0x04,
	STK_RESULT_ADDNL_BIP_PB_SECURITY_ERROR =		0x05,
	STK_RESULT_ADDNL_BIP_PB_INTERFACE_NOT_AVAIL =		0x06,
	STK_RESULT_ADDNL_BIP_PB_DEVICE_NOT_REACHABLE =		0x07,
	STK_RESULT_ADDNL_BIP_PB_SERVICE_ERROR =			0x08,
	STK_RESULT_ADDNL_BIP_PB_SERVICE_ID_UNKNOWN =		0x09,
	STK_RESULT_ADDNL_BIP_PB_PORT_NOT_AVAIL =		0x10,
	STK_RESULT_ADDNL_BIP_PB_LAUNCH_PARAMETERS_MISSING =	0x11,
	STK_RESULT_ADDNL_BIP_PB_APPLICATION_LAUNCH_FAILED =	0x12
};

enum stk_tone_type {
	STK_TONE_TYPE_DIAL_TONE =	0x01,
	STK_TONE_TYPE_BUSY_TONE =	0x02,
	STK_TONE_TYPE_CONGESTION =	0x03,
	STK_TONE_TYPE_RP_ACK =		0x04,
	STK_TONE_TYPE_CALL_DROPPED =	0x05,
	STK_TONE_TYPE_ERROR =		0x06,
	STK_TONE_TYPE_CALL_WAITING =	0x07,
	STK_TONE_TYPE_RINGING =		0x08,
	STK_TONE_TYPE_GENERAL_BEEP =	0x10,
	STK_TONE_TYPE_POSITIVE_ACK =	0x11,
	STK_TONE_TYPE_NEGATIVE_ACK =	0x12,
	STK_TONE_TYPE_INCOMING_CALL =	0x13,
	STK_TONE_TYPE_INCOMING_SMS =	0x14,
	STK_TONE_TYPE_CRITICAL_ALERT =	0x15,
	STK_TONE_TYPE_VIBRATE =		0x20,
	STK_TONE_TYPE_HAPPY_TONE =	0x31,
	STK_TONE_TYPE_SAD_TONE =	0x32,
	STK_TONE_TYPE_URGENT_TONE =	0x33,
	STK_TONE_TYPE_QUESTION_TONE =	0x34,
	STK_TONE_TYPE_MESSAGE_TONE =	0x35,
	STK_TONE_TYPE_MELODY_1 =	0x40,
	STK_TONE_TYPE_MELODY_2 =	0x41,
	STK_TONE_TYPE_MELODY_3 =	0x42,
	STK_TONE_TYPE_MELODY_4 =	0x43,
	STK_TONE_TYPE_MELODY_5 =	0x44,
	STK_TONE_TYPE_MELODY_6 =	0x45,
	STK_TONE_TYPE_MELODY_7 =	0x46,
	STK_TONE_TYPE_MELODY_8 =	0x47
};

enum stk_event_type {
	STK_EVENT_TYPE_MT_CALL =				0x00,
	STK_EVENT_TYPE_CALL_CONNECTED =				0x01,
	STK_EVENT_TYPE_CALL_DISCONNECTED =			0x02,
	STK_EVENT_TYPE_LOCATION_STATUS =			0x03,
	STK_EVENT_TYPE_USER_ACTIVITY =				0x04,
	STK_EVENT_TYPE_IDLE_SCREEN_AVAILABLE =			0x05,
	STK_EVENT_TYPE_CARD_READER_STATUS =			0x06,
	STK_EVENT_TYPE_LANGUAGE_SELECTION =			0x07,
	STK_EVENT_TYPE_BROWSER_TERMINATION =			0x08,
	STK_EVENT_TYPE_DATA_AVAILABLE =				0x09,
	STK_EVENT_TYPE_CHANNEL_STATUS =				0x0A,
	STK_EVENT_TYPE_SINGLE_ACCESS_TECHNOLOGY_CHANGE =	0x0B,
	STK_EVENT_TYPE_DISPLAY_PARAMETERS_CHANGED =		0x0C,
	STK_EVENT_TYPE_LOCAL_CONNECTION =			0x0D,
	STK_EVENT_TYPE_NETWORK_SEARCH_MODE_CHANGE =		0x0E,
	STK_EVENT_TYPE_BROWSING_STATUS =			0x0F,
	STK_EVENT_TYPE_FRAMES_INFORMATION_CHANGE =		0x10,
	STK_EVENT_TYPE_I_WLAN_ACCESS_STATUS =			0x11,
	STK_EVENT_TYPE_NETWORK_REJECTION =			0x12,
	STK_EVENT_TYPE_HCI_CONNECTIVITY_EVENT =			0x13,
	STK_EVENT_TYPE_MULTIPLE_ACCESS_TECHNOLOGIES_CHANGE =	0x14
};

enum stk_service_state {
	STK_NORMAL_SERVICE =	0x00,
	STK_LIMITED_SERVICE =	0x01,
	STK_NO_SERVICE =	0x02
};

enum stk_icon_qualifier {
	STK_ICON_QUALIFIER_TYPE_SELF_EXPLANATORY =	0x00,
	STK_ICON_QUALIFIER_TYPE_NON_SELF_EXPLANATORY =	0x01
};

enum stk_ins {
	STK_INS_DEACTIVATE_FILE =			0x04,
	STK_INS_ERASE_RECORDS =				0x0C,
	STK_INS_ERASE_BINARY_0E =			0x0E,
	STK_INS_ERASE_BINARY_0F =			0x0F,
	STK_INS_PERFORM_SCQL_OPERATION =		0x10,
	STK_INS_PERFORM_TRANSACTION_OPERATION =		0x12,
	STK_INS_PERFORM_USER_OPERATION =		0x14,
	STK_INS_VERIFY_20 =				0x20,
	STK_INS_VERIFY_21 =				0x21,
	STK_INS_MANAGE_SECURITY_ENVIRONMENT =		0x22,
	STK_INS_CHANGE_REFERENCE_DATA =			0x24,
	STK_INS_DISABLE_VERIFICATION_REQUIREMENT =	0x26,
	STK_INS_ENABLE_VERIFICATION_REQUIREMENT =	0x28,
	STK_INS_PERFORM_SECURITY_OPERATION =		0x2A,
	STK_INS_RESET_RETRY_COUNTER =			0x2C,
	STK_INS_ACTIVATE_FILE =				0x44,
	STK_INS_GENERATE_ASYMMETRIC_KEY_PAIR =		0x46,
	STK_INS_MANAGE_CHANNEL =			0x70,
	STK_INS_EXTERNAL_AUTHENTICATE =			0x82,
	STK_INS_GET_CHALLENGE =				0x84,
	STK_INS_GENERAL_AUTHENTICATE_86 =		0x86,
	STK_INS_GENERAL_AUTHENTICATE_87 =		0x87,
	STK_INS_INTERNAL_AUTHENTICATE =			0x88,
	STK_INS_SEARCH_BINARY_A0 =			0xA0,
	STK_INS_SEARCH_BINARY_A1 =			0xA1,
	STK_INS_SEARCH_RECORD =				0xA2,
	STK_INS_SELECT =				0xA4,
	STK_INS_READ_BINARY_B0 =			0xB0,
	STK_INS_READ_BINARY_B1 =			0xB1,
	STK_INS_READ_RECORDS_B2 =			0xB2,
	STK_INS_READ_RECORDS_B3 =			0xB3,
	STK_INS_GET_RESPONSE =				0xC0,
	STK_INS_ENVELOPE_C2 =				0xC2,
	STK_INS_ENVELOPE_C3 =				0xC3,
	STK_INS_GET_DATA_CA =				0xCA,
	STK_INS_GET_DATA_CB =				0xCB,
	STK_INS_WRITE_BINARY_D0 =			0xD0,
	STK_INS_WRITE_BINARY_D1 =			0xD1,
	STK_INS_WRITE_RECORD =				0xD2,
	STK_INS_UPDATE_BINARY_D6 =			0xD6,
	STK_INS_UPDATE_BINARY_D7 =			0xD7,
	STK_INS_PUT_DATA_DA =				0xDA,
	STK_INS_PUT_DATA_DB =				0xDB,
	STK_INS_UPDATE_RECORD_DC =			0xDC,
	STK_INS_UPDATE_RECORD_DD =			0xDD,
	STK_INS_CREATE_FILE =				0xE0,
	STK_INS_APPEND_RECORD =				0xE2,
	STK_INS_DELETE_FILE =				0xE4,
	STK_INS_TERMINATE_DF =				0xE6,
	STK_INS_TERMINATE_EF =				0xE8,
	STK_INS_TERMINATE_CARD_USAGE =			0xFE
};

enum stk_browser_id {
	STK_BROWSER_ID_DEFAULT =	0x00,
	STK_BROWSER_ID_WML =		0x01,
	STK_BROWSER_ID_HTML =		0x02,
	STK_BROWSER_ID_XHTML =		0x03,
	STK_BROWSER_ID_CHTML =		0x04
};

enum stk_bearer {
	STK_BEARER_SMS =	0x00,
	STK_BEARER_CS_DATA =	0x01,
	STK_BEARER_GSM_3G =	0x02,
	STK_BEARER_PS =		0x03
};

enum stk_browser_termination_cause {
	STK_BROWSER_USER_TERMINATION =		0x00,
	STK_BROWSER_ERROR_TERMINATION =		0x01
};

/* Defined in TS 31.111 Section 8.52 */
enum stk_bearer_type {
	STK_BEARER_TYPE_CS =			0x01,
	STK_BEARER_TYPE_GPRS_UTRAN =		0x02,
	STK_BEARER_TYPE_DEFAULT =		0x03,
	STK_BEARER_TYPE_INDEPENDENT =		0x04,
	STK_BEARER_TYPE_BLUETOOTH =		0x05,
	STK_BEARER_TYPE_IRDA =			0x06,
	STK_BEARER_TYPE_RS232 =			0x07,
	STK_BEARER_TYPE_TIA_EIA_IS_820 =	0x08,
	STK_BEARER_TYPE_UTRAN_WITH_EXT_PARAMS = 0x09,
	STK_BEARER_TYPE_I_WLAN =		0x0A,
	STK_BEARER_TYPE_EUTRAN_MAPPED_UTRAN =	0x0B,
	STK_BEARER_TYPE_USB =			0x10
};

enum stk_address_type {
	STK_ADDRESS_AUTO =	-1,
	STK_ADDRESS_IPV4 =	0x21,
	STK_ADDRESS_IPV6 =	0x57
};

enum stk_access_technology_type {
	STK_ACCESS_TECHNOLOGY_GSM =		0x00,
	STK_ACCESS_TECHNOLOGY_TIA_EIA_553 =	0x01,
	STK_ACCESS_TECHNOLOGY_TIA_EIA_136_C =	0x02,
	STK_ACCESS_TECHNOLOGY_UTRAN =		0x03,
	STK_ACCESS_TECHNOLOGY_TETRA =		0x04,
	STK_ACCESS_TECHNOLOGY_TIA_EIA_95 =	0x05,
	STK_ACCESS_TECHNOLOGY_CDMA2000_1X =	0x06,
	STK_ACCESS_TECHNOLOGY_CDMA2000_HRPD =	0x07,
	STK_ACCESS_TECHNOLOGY_EUTRAN =		0x08
};

enum stk_technology_id {
	STK_TECHNOLOGY_INDEPENDENT =	0x00,
	STK_TECHNOLOGY_BLUETOOTH =	0x01,
	STK_TECHNOLOGY_IRDA =		0x02,
	STK_TECHNOLOGY_RS232 =		0x03,
	STK_TECHNOLOGY_USB =		0x04
};

enum stk_battery_state {
	STK_BATTERY_VERY_LOW =	0x00,
	STK_BATTERY_LOW =	0x01,
	STK_BATTERY_AVERAGE =	0x02,
	STK_BATTERY_GOOD =	0x03,
	STK_BATTERY_FULL =	0x04
};

enum stk_frame_layout_type {
	STK_LAYOUT_HORIZONTAL =		0x01,
	STK_LAYOUT_VERTICAL =		0x02
};

enum stk_broadcast_network_technology {
	STK_BROADCAST_NETWORK_DVB_H = 0x00,
	STK_BROADCAST_NETWORK_DVB_T = 0x01,
	STK_BROADCAST_NETWORK_DVB_SH = 0x02,
	STK_BROADCAST_NETWORK_T_DMB = 0x03
};

enum stk_i_wlan_access_status {
	STK_I_WLAN_STATUS_NO_COVERAGE		= 0x00,
	STK_I_WLAN_STATUS_NOT_CONNECTED		= 0x01,
	STK_I_WLAN_STATUS_CONNECTED		= 0x02,
};

enum stk_update_attach_type {
	STK_UPDATE_ATTACH_NORMAL_LOCATION_UPDATING	= 0x00,
	STK_UPDATE_ATTACH_PERIODIC_UPDATING		= 0x01,
	STK_UPDATE_ATTACH_IMSI_ATTACH			= 0x02,
	STK_UPDATE_ATTACH_GPRS_ATTACH			= 0x03,
	STK_UPDATE_ATTACH_GPRS_IMSI_ATTACH		= 0x04,
	STK_UPDATE_ATTACH_RA_UPDATING			= 0x05,
	STK_UPDATE_ATTACH_RA_LA_UPDATING		= 0x06,
	STK_UPDATE_ATTACH_RA_LA_UPDATING_IMSI_ATTACH	= 0x07,
	STK_UPDATE_ATTACH_PERIODIC_RA_UPDATING		= 0x08,
	STK_UPDATE_ATTACH_EPS_ATTACH			= 0x09,
	STK_UPDATE_ATTACH_EPS_IMSI_ATTACH		= 0x0a,
	STK_UPDATE_ATTACH_TA_UPDATING			= 0x0b,
	STK_UPDATE_ATTACH_TA_LA_UPDATING		= 0x0c,
	STK_UPDATE_ATTACH_TA_LA_UPDATING_IMSI_ATTACH	= 0x0d,
	STK_UPDATE_ATTACH_PERIDIC_TA_UPDATING		= 0x0e,
};

enum stk_rejection_cause_code {
	/* MM and GMM codes (GERAN/UTRAN) */
	STK_CAUSE_GMM_IMSI_UNKNOWN_IN_HLR		= 0x02,
	STK_CAUSE_GMM_ILLEGAL_MS			= 0x03,
	STK_CAUSE_GMM_IMSI_UNKNOWN_IN_VLR		= 0x04,
	STK_CAUSE_GMM_IMEI_NOT_ACCEPTED			= 0x05,
	STK_CAUSE_GMM_ILLEGAL_ME			= 0x06,
	STK_CAUSE_GMM_GPRS_NOT_ALLOWED			= 0x07,
	STK_CAUSE_GMM_GPRS_AND_NON_GPRS_NOT_ALLOWED	= 0x08,
	STK_CAUSE_GMM_IMEI_NOT_DERIVED_BY_NETWORK	= 0x09,
	STK_CAUSE_GMM_IMPLICITLY_DETACHED		= 0x0a,
	STK_CAUSE_GMM_PLMN_NOT_ALLOWED			= 0x0b,
	STK_CAUSE_GMM_LAC_NOT_ALLOWED			= 0x0c,
	STK_CAUSE_GMM_ROAMING_NOT_ALLOWED		= 0x0d,
	STK_CAUSE_GMM_GPRS_NOT_ALLOWED_IN_PLMN		= 0x0e,
	STK_CAUSE_GMM_NO_SUITABLE_CELLS			= 0x0f,
	STK_CAUSE_GMM_MSC_TEMPORARILY_UNREACHABLE	= 0x10,
	STK_CAUSE_GMM_NETWORK_FAILURE			= 0x11,
	STK_CAUSE_GMM_MAC_FAILURE			= 0x14,
	STK_CAUSE_GMM_SYNCH_FAILURE			= 0x15,
	STK_CAUSE_GMM_CONGESTION			= 0x16,
	STK_CAUSE_GMM_GSM_AUTHENTICATION_UNACCEPTABLE	= 0x17,
	STK_CAUSE_GMM_NOT_AUTHORISED_FOR_CSG		= 0x19,
	STK_CAUSE_GMM_SERVICE_OPTION_NOT_SUPPORTED	= 0x20,
	STK_CAUSE_GMM_SERVICE_OPTION_NOT_SUBSCRIBED	= 0x21,
	STK_CAUSE_GMM_SERVICE_OPTION_TEMPORARY_DEFUNC	= 0x22,
	STK_CAUSE_GMM_CALL_NOT_IDENTIFIED		= 0x26,
	STK_CAUSE_GMM_NO_PDP_CONTEXT_ACTIVATED		= 0x28,
	STK_CAUSE_GMM_RETRY_ON_NEW_CELL			= 0x30, /* to 0x3f */
	STK_CAUSE_GMM_SEMANTICALLY_INCORRECT_MESSAGE	= 0x5f,
	STK_CAUSE_GMM_INVALID_MANDATORY_INFO		= 0x60,
	STK_CAUSE_GMM_MESSAGE_TYPE_UNKNOWN		= 0x61,
	STK_CAUSE_GMM_MESSAGE_TYPE_INCOMPATIBLE_STATE	= 0x62,
	STK_CAUSE_GMM_IE_UNKNOWN			= 0x63,
	STK_CAUSE_GMM_CONDITIONAL_IE_ERROR		= 0x64,
	STK_CAUSE_GMM_MESSAGE_INCOMPATIBLE_WITH_STATE	= 0x65,
	STK_CAUSE_GMM_PROTOCOL_ERROR			= 0x6f,
	/* EMM codes (E-UTRAN) */
	STK_CAUSE_EMM_IMSI_UNKNOWN_IN_HSS		= 0x02,
	STK_CAUSE_EMM_ILLEGAL_UE			= 0x03,
	STK_CAUSE_EMM_ILLEGAL_ME			= 0x06,
	STK_CAUSE_EMM_EPS_NOT_ALLOWED			= 0x07,
	STK_CAUSE_EMM_EPS_AND_NON_EPS_NOT_ALLOWED	= 0x08,
	STK_CAUSE_EMM_IMEI_NOT_DERIVED_BY_NETWORK	= 0x09,
	STK_CAUSE_EMM_IMPLICITLY_DETACHED		= 0x0a,
	STK_CAUSE_EMM_PLMN_NOT_ALLOWED			= 0x0b,
	STK_CAUSE_EMM_TAC_NOT_ALLOWED			= 0x0c,
	STK_CAUSE_EMM_ROAMING_NOT_ALLOWED		= 0x0d,
	STK_CAUSE_EMM_EPS_NOT_ALLOWED_IN_PLMN		= 0x0e,
	STK_CAUSE_EMM_NO_SUITABLE_CELLS			= 0x0f,
	STK_CAUSE_EMM_MSC_TEMPORARILY_UNREACHABLE	= 0x10,
	STK_CAUSE_EMM_NETWORK_FAILURE			= 0x11,
	STK_CAUSE_EMM_MAC_FAILURE			= 0x14,
	STK_CAUSE_EMM_SYNCH_FAILURE			= 0x15,
	STK_CAUSE_EMM_CONGESTION			= 0x16,
	STK_CAUSE_EMM_SECURITY_MODE_REJECTED		= 0x18,
	STK_CAUSE_EMM_NOT_AUTHORISED_FOR_CSG		= 0x19,
	STK_CAUSE_EMM_CS_FALLBACK_NOT_ALLOWED		= 0x26,
	STK_CAUSE_EMM_CS_DOMAIN_TEMPORARY_UNAVAILABLE	= 0x27,
	STK_CAUSE_EMM_NO_EPS_BEARER_CONTEXT_ACTIVATED	= 0x28,
	STK_CAUSE_EMM_SEMANTICALLY_INCORRECT_MESSAGE	= 0x5f,
	STK_CAUSE_EMM_INVALID_MANDATORY_INFO		= 0x60,
	STK_CAUSE_EMM_MESSAGE_TYPE_UNKNOWN		= 0x61,
	STK_CAUSE_EMM_MESSAGE_TYPE_INCOMPATIBLE_STATE	= 0x62,
	STK_CAUSE_EMM_IE_UNKNOWN			= 0x63,
	STK_CAUSE_EMM_CONDITIONAL_IE_ERROR		= 0x64,
	STK_CAUSE_EMM_MESSAGE_INCOMPATIBLE_WITH_STATE	= 0x65,
	STK_CAUSE_EMM_PROTOCOL_ERROR			= 0x6f,
};

enum stk_me_status {
	STK_ME_STATUS_IDLE =		0x00,
	STK_ME_STATUS_NOT_IDLE =	0x01
};

enum stk_img_scheme {
	STK_IMG_SCHEME_BASIC =		0x11,
	STK_IMG_SCHEME_COLOR =		0x21,
	STK_IMG_SCHEME_TRANSPARENCY =	0x22,
};

/* Defined in TS 102.223 Section 8.6 */
enum stk_qualifier_open_channel {
	STK_OPEN_CHANNEL_FLAG_IMMEDIATE =		0x01,
	STK_OPEN_CHANNEL_FLAG_AUTO_RECONNECT =		0x02,
	STK_OPEN_CHANNEL_FLAG_BACKGROUND =		0x04,
};

/* Defined in TS 102.223 Section 8.6 */
enum stk_qualifier_send_data {
	STK_SEND_DATA_STORE_DATA =	0x00,
	STK_SEND_DATA_IMMEDIATELY =	0x01,
};

/* Defined in TS 102.223 Section 8.56 */
enum stk_channel_status {
	STK_CHANNEL_PACKET_DATA_SERVICE_NOT_ACTIVATED = 0x00,
	STK_CHANNEL_PACKET_DATA_SERVICE_ACTIVATED =	0x01,
	STK_CHANNEL_TCP_IN_CLOSED_STATE =		0x02,
	STK_CHANNEL_TCP_IN_LISTEN_STATE =		0x03,
	STK_CHANNEL_TCP_IN_ESTABLISHED_STATE =		0x04,
	STK_CHANNEL_LINK_DROPPED =			0x05,
};

/* Defined in TS 102.223 Section 8.59 */
enum stk_transport_protocol_type {
	STK_TRANSPORT_PROTOCOL_UDP_CLIENT_REMOTE =	0x01,
	STK_TRANSPORT_PROTOCOL_TCP_CLIENT_REMOTE =	0x02,
	STK_TRANSPORT_PROTOCOL_TCP_SERVER =		0x03,
	STK_TRANSPORT_PROTOCOL_UDP_CLIENT_LOCAL =	0x04,
	STK_TRANSPORT_PROTOCOL_TCP_CLIENT_LOCAL =	0x05,
	STK_TRANSPORT_PROTOCOL_DIRECT =			0x06,
};

/* For data object that only has a byte array with undetermined length */
struct stk_common_byte_array {
	unsigned char *array;
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.1 */
struct stk_address {
	unsigned char ton_npi;
	char *number;
};

/*
 * Defined in TS 102.223 Section 8.3
 *
 * The maximum size of the subaddress is different depending on the referenced
 * specification.  According to TS 24.008 Section 10.5.4.8: "The called party
 * subaddress is a type 4 information element with a minimum length of 2 octets
 * and a maximum length of 23 octets"
 *
 * According to TS 31.102 Section 4.4.2.4: "The subaddress data contains
 * information as defined for this purpose in TS 24.008 [9]. All information
 * defined in TS 24.008, except the information element identifier, shall be
 * stored in the USIM. The length of this subaddress data can be up to 22
 * bytes."
 */
struct stk_subaddress {
	ofono_bool_t has_subaddr;
	unsigned char len;
	unsigned char subaddr[23];
};

/*
 * Defined in TS 102.223 Section 8.4
 *
 * According to 24.008 Section 10.5.4.5 "The bearer capability is a type 4
 * information element with a minimum length of 3 octets and a maximum length
 * of 16 octets."
 *
 * According to TS 31.102 Section 4.2.38 the CCP length is 15 bytes.
 *
 * The CCP structure is not decoded, but stored as is from the CTLV
 */
struct stk_ccp {
	unsigned char len;
	unsigned char ccp[16];
};

/* Defined in TS 31.111 Section 8.5 */
struct stk_cbs_page {
	unsigned char len;
	unsigned char page[88];
};

/*
 * According to 102.223 Section 8.8 interval values of 0x00 are reserved.
 * We use this to denote empty duration objects.
 */
struct stk_duration {
	enum stk_duration_type unit;
	unsigned char interval;
};

/* Defined in TS 102.223 Section 8.9 */
struct stk_item {
	unsigned char id;
	char *text;
};

/*
 * According to 102.223 Section 8.11, the maximum length should never be set
 * to 0.
 */
struct stk_response_length {
	unsigned char min;
	unsigned char max;
};

/* Defined in TS 102.223 Section 8.12 */
struct stk_result {
	enum stk_result_type type;
	unsigned int additional_len;
	unsigned char *additional;
};

/* Defined in TS 102.223 Section 8.14 */
struct stk_ss {
	unsigned char ton_npi;
	char *ss;
};

/* Defined in TS 131.111 Section 8.17.  Length limit of 160 chars in 23.028 */
struct stk_ussd_string {
	unsigned char dcs;
	unsigned char string[160];
	int len;
};

/*
 * Define the struct of single file in TS102.223 Section 8.18.
 * According to TS 11.11 Section 6.2, each file id has two bytes, and the
 * maximum Dedicated File level is 2. So the maximum size of file is 8, which
 * contains two bytes of Master File, 2 bytes of 1st level Dedicated File,
 * 2 bytes of 2nd level Dedicated File and 2 bytes of Elementary File.
 */
struct stk_file {
	unsigned char file[8];
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.19 */
struct stk_location_info {
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	unsigned short lac_tac;
	ofono_bool_t has_ci;
	unsigned short ci;
	ofono_bool_t has_ext_ci;
	unsigned short ext_ci;
	ofono_bool_t has_eutran_ci;
	guint32 eutran_ci;
};

/*
 * According to 102.223 Section 8.24 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_items_next_action_indicator {
	unsigned char list[127];
	unsigned int len;
};

/*
 * According to 102.223 Section 8.25, there are 21 kinds of event type and no
 * one should appear more than once.
 */
struct stk_event_list {
	unsigned char list[21];
	unsigned int len;
};

/*
 * According to 102.223 Section 8.26, the maximum length of cause is 30.
 */
struct stk_cause {
	unsigned char cause[30];
	unsigned int len;
	ofono_bool_t has_cause;
};

/*
 * According to 102.223 Section 8.28 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_transaction_id {
	unsigned char list[127];
	unsigned int len;
};

/*
 * According to 31.111 Section 8.29 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs. Each channel
 * is represented as 10 bits, so the maximum number of channel is 127*8/10=101.
 */
struct stk_bcch_channel_list {
	unsigned short channels[101];
	unsigned int num;
	ofono_bool_t has_list;
};

/*
 * Defined in TS 102.223 Section 8.31
 * Icon ID denotes a file on the SIM filesystem.  Since EF cannot have record
 * ids of 0, we use icon_id with 0 to denote empty icon_identifier objects
 */
struct stk_icon_id {
	unsigned char qualifier;
	unsigned char id;
};

/*
 * According to 102.223 Section 8.32 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs. This size also
 * includes icon list qualifier for 1 byte, so the maxmimum size of icon
 * identifier list is 126.
 */
struct stk_item_icon_id_list {
	unsigned char qualifier;
	unsigned char list[126];
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.33 */
struct stk_reader_status {
	int id;
	ofono_bool_t removable;
	ofono_bool_t present;
	ofono_bool_t id1_size;
	ofono_bool_t card_present;
	ofono_bool_t card_powered;
};

/*
 * According to 102.223 Section 8.34 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_card_atr {
	unsigned char atr[127];
	unsigned int len;
};

/*
 * Defined in TS 102.223 Section 8.35. According to it, the maximum size
 * of data is 236.
 */
struct stk_c_apdu {
	unsigned char cla;
	unsigned char ins;
	unsigned char p1;
	unsigned char p2;
	unsigned char lc;
	unsigned char data[236];
	ofono_bool_t has_le;
	unsigned char le;
};

/* Defined in TS 102.223 Section 8.36. According to it, the maximum size
 * of data is 237.
 */
struct stk_r_apdu {
	unsigned char sw1;
	unsigned char sw2;
	unsigned char data[237];
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.38 */
struct stk_timer_value {
	ofono_bool_t has_value;
	unsigned char hour;
	unsigned char minute;
	unsigned char second;
};

/* Defined in TS 102.223 Section 8.42 */
struct stk_bc_repeat {
	ofono_bool_t has_bc_repeat;
	unsigned char value;
};

/* Defined in TS 31.111 Section 8.46 */
struct stk_timing_advance {
	ofono_bool_t has_value;
	enum stk_me_status status;
	/*
	 * Contains bit periods number according to 3GPP TS
	 * 44.118 Section 9.3.106 / 3GPP TS 44.018 Section
	 * 10.5.2.40.1, not microseconds
	 */
	unsigned char advance;
};

/* Bearer parameters for GPRS/UTRAN Packet Service/E-UTRAN */
struct stk_gprs_bearer_parameters {
	unsigned char precedence;
	unsigned char delay;
	unsigned char reliability;
	unsigned char peak;
	unsigned char mean;
	unsigned char pdp_type;
};

/* Defined in TS 31.111 Section 8.52 */
struct stk_bearer_description {
	enum stk_bearer_type type;
	struct stk_gprs_bearer_parameters gprs;
};

/*
 * According to 102.223 Section 8.57 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_card_reader_id {
	unsigned char id[127];
	unsigned char len;
};

/*
 * According to 102.223 Section 8.58 the address can be either ipv4 or ipv6.
 * So the maximum size is 16 (for ipv6).
 */
struct stk_other_address {
	union {
		/* Network Byte Order */
		guint32 ipv4;
		unsigned char ipv6[16];
	} addr;
	enum stk_address_type type;
};

/* Defined in TS 102.223 Section 8.59 */
struct stk_uicc_te_interface {
	enum stk_transport_protocol_type protocol;
	unsigned short port;
};

/*
 * Defined in TS 102.223 Section 8.60.
 * According to 101.220, Section 4, aid contains two fields RID and PIX.
 * RID has 5 bytes, while PIX contains information between 7 to 11 bytes.
 * So the maximum size of aid is 16 bytes.
 */
struct stk_aid {
	unsigned char aid[16];
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.62 */
struct stk_display_parameters {
	unsigned char height;
	unsigned char width;
	unsigned char effects;
};

/* Defined in TS 102.223 Section 8.63 */
struct stk_service_record {
	unsigned char tech_id;
	unsigned char serv_id;
	unsigned char *serv_rec;
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.64 */
struct stk_device_filter {
	unsigned char tech_id;
	unsigned char *dev_filter;
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.65 */
struct stk_service_search {
	unsigned char tech_id;
	unsigned char *ser_search;
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.66 */
struct stk_attribute_info {
	unsigned char tech_id;
	unsigned char *attr_info;
	unsigned int len;
};

/*
 * According to TS 102.223 Section 8.68, remote entity address can be either
 * 6-bytes IEEE-802 address, or 4-bytes IrDA device address.
 */
struct stk_remote_entity_address {
	unsigned char coding_type;
	ofono_bool_t has_address;
	union {
		unsigned char ieee802[6];
		unsigned char irda[4];
	} addr;
};

/*
 * According to 102.223 Section 8.72 the length of text attribute CTLV is 1
 * byte.  This means that the maximum size is 127 according to the rules
 * of CTLVs.  Empty attribute options will have len of 0.
 */
struct stk_text_attribute {
	unsigned char attributes[127];
	unsigned char len;
};

/* Defined in TS 31.111 Section 8.72 */
struct stk_pdp_act_par {
	unsigned char par[127];
	unsigned char len;
};

/*
 * According to 102.223 Section 8.73 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs. In addition,
 * the length should be also the number multiplied by 4, so the maximum number
 * is 124.
 */
struct stk_item_text_attribute_list {
	unsigned char list[124];
	unsigned char len;
};

/*
 * According to 102.223 Section 8.78 the length of CTLV is 1 byte. This means
 * that the maximum length is 127 bytes for the total length of layout and
 * relative-sized frame. Thus the maximum length of relative size is 126 bytes.
 */
struct stk_frame_layout {
	unsigned char layout;
	unsigned char size[126];
	unsigned int len;
};

/*
 * According to 102.223 Section 8.79 the length of CTLV is 1 byte. This means
 * that the maximum length is 127 bytes for the total length of default frame
 * id and frame information list. Thus the maximum length of frame information
 * list is 126 bytes.
 */
struct stk_frames_info {
	unsigned char id;
	struct {
		unsigned char width, height;
	} list[63];
	unsigned int len;
};

/* Defined in TS 102.223 Section 8.80 */
struct stk_frame_id {
	ofono_bool_t has_id;
	unsigned char id;
};

/*
 * According to 102.223 Section 8.82 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_mms_reference {
	unsigned char ref[127];
	unsigned char len;
};

/*
 * According to 102.223 Section 8.83 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_mms_id {
	unsigned char id[127];
	unsigned char len;
};

/*
 * According to 102.223 Section 8.84 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_mms_transfer_status {
	unsigned char status[127];
	unsigned char len;
};

/*
 * According to 102.223 Section 8.85 the length of CTLV is 1 byte. This means
 * that the maximum size is 127 according to the rules of CTLVs.
 */
struct stk_mms_content_id {
	unsigned char id[127];
	unsigned char len;
};

/* Defined in TS 102.223 Section 8.88 */
struct stk_registry_application_data {
	unsigned short port;
	unsigned char type;
	char *name;
};

/*
 * According to 102.223 Section 8.90 the length of CTLV is 1 byte. This means
 * that the maximum length is 127 bytes for the total length of broadcast
 * network technology and location information. Thus the maximum length of
 * location information is 126 bytes.
 */
struct stk_broadcast_network_information {
	unsigned char tech;
	unsigned char loc_info[126];
	unsigned int len;
};

/* Defined in TS 131.111 Section 8.91 */
struct stk_routing_area_info {
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	unsigned short lac;
	unsigned char rac;
};

/* Defined in TS 131.111 Section 8.99 */
struct stk_tracking_area_id {
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	unsigned short tac;
};

struct stk_command_display_text {
	char *text;
	struct stk_icon_id icon_id;
	ofono_bool_t immediate_response;
	struct stk_duration duration;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_get_inkey {
	char *text;
	struct stk_icon_id icon_id;
	struct stk_duration duration;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_get_input {
	char *text;
	struct stk_response_length resp_len;
	char *default_text;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_play_tone {
	char *alpha_id;
	unsigned char tone;
	struct stk_duration duration;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_poll_interval {
	struct stk_duration duration;
};

struct stk_command_setup_menu {
	char *alpha_id;
	GSList *items;
	struct stk_items_next_action_indicator next_act;
	struct stk_icon_id icon_id;
	struct stk_item_icon_id_list item_icon_id_list;
	struct stk_text_attribute text_attr;
	struct stk_item_text_attribute_list item_text_attr_list;
};

struct stk_command_select_item {
	char *alpha_id;
	GSList *items;
	struct stk_items_next_action_indicator next_act;
	unsigned char item_id;
	struct stk_icon_id icon_id;
	struct stk_item_icon_id_list item_icon_id_list;
	struct stk_text_attribute text_attr;
	struct stk_item_text_attribute_list item_text_attr_list;
	struct stk_frame_id frame_id;
};

struct stk_command_send_sms {
	char *alpha_id;
	struct sms gsm_sms;
	struct stk_common_byte_array cdma_sms;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_send_ss {
	char *alpha_id;
	struct stk_ss ss;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_send_ussd {
	char *alpha_id;
	struct stk_ussd_string ussd_string;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_setup_call {
	char *alpha_id_usr_cfm;
	struct stk_address addr;
	struct stk_ccp ccp;
	struct stk_subaddress subaddr;
	struct stk_duration duration;
	struct stk_icon_id icon_id_usr_cfm;
	char *alpha_id_call_setup;
	struct stk_icon_id icon_id_call_setup;
	struct stk_text_attribute text_attr_usr_cfm;
	struct stk_text_attribute text_attr_call_setup;
	struct stk_frame_id frame_id;
};

struct stk_command_refresh {
	GSList *file_list;
	struct stk_aid aid;
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_setup_event_list {
	struct stk_event_list event_list;
};

struct stk_command_perform_card_apdu {
	struct stk_c_apdu c_apdu;
};

struct stk_command_timer_mgmt {
	unsigned char timer_id;
	struct stk_timer_value timer_value;
};

struct stk_command_setup_idle_mode_text {
	char *text;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_run_at_command {
	char *alpha_id;
	char *at_command;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_send_dtmf {
	char *alpha_id;
	char *dtmf;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_language_notification {
	char language[3];
};

struct stk_command_launch_browser {
	unsigned char browser_id;
	char *url;
	struct stk_common_byte_array bearer;
	GSList *prov_file_refs;
	char *text_gateway_proxy_id;
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
	struct stk_common_byte_array network_name;
	char *text_usr;
	char *text_passwd;
};

struct stk_command_open_channel {
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_bearer_description bearer_desc;
	unsigned short buf_size;
	char *apn;
	struct stk_other_address local_addr;
	char *text_usr;
	char *text_passwd;
	struct stk_uicc_te_interface uti;
	struct stk_other_address data_dest_addr;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_close_channel {
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_receive_data {
	char *alpha_id;
	struct stk_icon_id icon_id;
	unsigned char data_len;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_send_data {
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_common_byte_array data;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_service_search {
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_service_search serv_search;
	struct stk_device_filter dev_filter;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_get_service_info {
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_attribute_info attr_info;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_declare_service {
	struct stk_service_record serv_rec;
	struct stk_uicc_te_interface intf;
};

struct stk_command_set_frames {
	struct stk_frame_id frame_id;
	struct stk_frame_layout frame_layout;
	struct stk_frame_id frame_id_default;
};

struct stk_command_retrieve_mms {
	char *alpha_id;
	struct stk_icon_id icon_id;
	struct stk_mms_reference mms_ref;
	GSList *mms_rec_files;
	struct stk_mms_content_id mms_content_id;
	struct stk_mms_id mms_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_submit_mms {
	char *alpha_id;
	struct stk_icon_id icon_id;
	GSList *mms_subm_files;
	struct stk_mms_id mms_id;
	struct stk_text_attribute text_attr;
	struct stk_frame_id frame_id;
};

struct stk_command_display_mms {
	GSList *mms_subm_files;
	struct stk_mms_id mms_id;
	ofono_bool_t imd_resp;
	struct stk_frame_id frame_id;
};

struct stk_command_activate {
	unsigned char actv_desc;
};

enum stk_command_parse_result {
	STK_PARSE_RESULT_OK,
	STK_PARSE_RESULT_TYPE_NOT_UNDERSTOOD,
	STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD,
	STK_PARSE_RESULT_MISSING_VALUE,
};

struct stk_command {
	unsigned char number;
	unsigned char type;
	unsigned char qualifier;
	enum stk_device_identity_type src;
	enum stk_device_identity_type dst;
	enum stk_command_parse_result status;

	union {
		struct stk_command_display_text display_text;
		struct stk_command_get_inkey get_inkey;
		struct stk_command_get_input get_input;
		struct stk_command_play_tone play_tone;
		struct stk_command_poll_interval poll_interval;
		struct stk_command_refresh refresh;
		struct stk_command_setup_menu setup_menu;
		struct stk_command_select_item select_item;
		struct stk_command_send_sms send_sms;
		struct stk_command_send_ss send_ss;
		struct stk_command_send_ussd send_ussd;
		struct stk_command_setup_call setup_call;
		struct stk_command_setup_event_list setup_event_list;
		struct stk_command_perform_card_apdu perform_card_apdu;
		struct stk_command_timer_mgmt timer_mgmt;
		struct stk_command_setup_idle_mode_text setup_idle_mode_text;
		struct stk_command_run_at_command run_at_command;
		struct stk_command_send_dtmf send_dtmf;
		struct stk_command_language_notification language_notification;
		struct stk_command_launch_browser launch_browser;
		struct stk_command_open_channel open_channel;
		struct stk_command_close_channel close_channel;
		struct stk_command_receive_data receive_data;
		struct stk_command_send_data send_data;
		struct stk_command_service_search service_search;
		struct stk_command_get_service_info get_service_info;
		struct stk_command_declare_service declare_service;
		struct stk_command_set_frames set_frames;
		struct stk_command_retrieve_mms retrieve_mms;
		struct stk_command_submit_mms submit_mms;
		struct stk_command_display_mms display_mms;
		struct stk_command_activate activate;
	};

	void (*destructor)(struct stk_command *command);
};

/* TERMINAL RESPONSEs defined in TS 102.223 Section 6.8 */
struct stk_response_generic {
};

struct stk_answer_text {
	char *text;
	ofono_bool_t packed;
	ofono_bool_t yesno;
	/*
	 * If a "Yes/No" answer was requested in a GET INKEY command,
	 * .yesno must be TRUE and text should be non-NULL to indicate
	 * a Yes response or NULL to indicate a No response.
	 */
};

struct stk_ussd_text {
	ofono_bool_t has_text;
	const unsigned char *text;
	int dcs;
	int len;
};

struct stk_channel {
	unsigned char id;
	enum stk_channel_status status;
};

struct stk_response_get_inkey {
	struct stk_answer_text text;
	struct stk_duration duration;
};

struct stk_response_get_input {
	struct stk_answer_text text;
};

struct stk_response_poll_interval {
	struct stk_duration max_interval;
};

struct stk_response_select_item {
	unsigned char item_id;
};

struct stk_response_set_up_call {
	struct stk_common_byte_array cc_requested_action;
	struct {
		ofono_bool_t cc_modified;
		struct stk_result result;
	} modified_result;
};

struct stk_response_local_info {
	union {
		struct stk_location_info location;
		const char *imei;
		struct stk_network_measurement_results {
			struct stk_common_byte_array nmr;
			struct stk_bcch_channel_list bcch_ch_list;
		} nmr;
		struct sms_scts datetime;
		const char *language;
		enum stk_battery_state battery_charge;
		enum stk_access_technology_type access_technology;
		struct stk_timing_advance tadv;
		/* Bits[31:24]: manufacturer, bits[23:0]: serial number */
		guint32 esn;
		const char *imeisv;
		enum stk_network_search_mode {
			STK_NETWORK_SEARCH_MODE_MANUAL = 0x00,
			STK_NETWORK_SEARCH_MODE_AUTOMATIC = 0x01,
		} search_mode;
		const char *meid;
		struct stk_broadcast_network_information broadcast_network_info;
		struct stk_access_technologies {
			const enum stk_access_technology_type *techs;
			int length;
		} access_technologies;
		struct {
			struct stk_access_technologies access_techs;
			struct stk_location_info *locations;
		} location_infos;
		struct {
			struct stk_access_technologies access_techs;
			struct stk_network_measurement_results *nmrs;
		} nmrs;
	};
};

struct stk_response_timer_mgmt {
	unsigned char id;
	struct stk_timer_value value;
};

struct stk_response_run_at_command {
	const char *at_response;
};

struct stk_response_send_ussd {
	struct stk_ussd_text text;
};

struct stk_response_open_channel {
	struct stk_channel channel;
	struct stk_bearer_description bearer_desc;
	unsigned short buf_size;
};

struct stk_response_receive_data {
	struct stk_common_byte_array rx_data;
	unsigned short rx_remaining;
};

struct stk_response_send_data {
	unsigned short tx_avail;
};

struct stk_response_channel_status {
	struct stk_channel channel;
};

struct stk_response {
	unsigned char number;
	unsigned char type;
	unsigned char qualifier;
	enum stk_device_identity_type src;
	enum stk_device_identity_type dst;
	struct stk_result result;

	union {
		struct stk_response_generic display_text;
		struct stk_response_get_inkey get_inkey;
		struct stk_response_get_input get_input;
		struct stk_response_generic more_time;
		struct stk_response_generic play_tone;
		struct stk_response_poll_interval poll_interval;
		struct stk_response_generic refresh;
		struct stk_response_generic set_up_menu;
		struct stk_response_select_item select_item;
		struct stk_response_generic send_sms;
		struct stk_response_set_up_call set_up_call;
		struct stk_response_generic polling_off;
		struct stk_response_local_info provide_local_info;
		struct stk_response_generic set_up_event_list;
		struct stk_response_timer_mgmt timer_mgmt;
		struct stk_response_generic set_up_idle_mode_text;
		struct stk_response_run_at_command run_at_command;
		struct stk_response_generic send_dtmf;
		struct stk_response_generic language_notification;
		struct stk_response_generic launch_browser;
		struct stk_response_send_ussd send_ussd;
		struct stk_response_open_channel open_channel;
		struct stk_response_receive_data receive_data;
		struct stk_response_send_data send_data;
		struct stk_response_channel_status channel_status;
	};

	void (*destructor)(struct stk_response *response);
};

/* ENVELOPEs defined in TS 102.223 Section 7 */
struct stk_envelope_sms_pp_download {
	struct stk_address address;
	struct sms_deliver message;
};

struct stk_envelope_cbs_pp_download {
	struct cbs page;
};

struct stk_envelope_menu_selection {
	unsigned char item_id;
	ofono_bool_t help_request;
};

struct stk_envelope_sms_mo_control {
	struct stk_address sc_address;
	struct stk_address dest_address;
	struct stk_location_info location;
};

enum stk_call_control_type {
	STK_CC_TYPE_CALL_SETUP,
	STK_CC_TYPE_SUPPLEMENTARY_SERVICE,
	STK_CC_TYPE_USSD_OP,
	STK_CC_TYPE_PDP_CTX_ACTIVATION,
	STK_CC_TYPE_EPS_PDN_CONNECTION_ACTIVATION,
};

/* Used both in the ENVELOPE message to UICC and response from UICC */
struct stk_envelope_call_control {
	enum stk_call_control_type type;
	union {
		struct stk_address address;
		struct stk_address ss_string;
		struct stk_ussd_string ussd_string;
		struct stk_common_byte_array pdp_ctx_params;
		struct stk_common_byte_array eps_pdn_params;
	};
	/*
	 * At least one of the following two fields must be present in a
	 * response indicating modification of the call.
	 * In an EVELOPE message, only allowed for a call setup.
	 */
	struct stk_ccp ccp1;
	struct stk_subaddress subaddress;
	struct stk_location_info location;
	/* Only allowed when ccp1 is present */
	struct stk_ccp ccp2;
	char *alpha_id;
	/* Only allowed when both ccp1 and ccp2 are present */
	struct stk_bc_repeat bc_repeat;
};

struct stk_envelope_event_download {
	enum stk_event_type type;
	union {
		struct {
			unsigned char transaction_id;
			struct stk_address caller_address;
			struct stk_subaddress caller_subaddress;
		} mt_call;
		struct {
			unsigned char transaction_id;
		} call_connected;
		struct {
			struct stk_transaction_id transaction_ids;
			struct stk_cause cause;
		} call_disconnected;
		struct {
			enum stk_service_state state;
			/* Present when state indicated Normal Service */
			struct stk_location_info info;
		} location_status;
		struct stk_reader_status card_reader_status;
		char language_selection[3];
		struct {
			enum stk_browser_termination_cause cause;
		} browser_termination;
		struct {
			struct stk_channel channel;
			unsigned short channel_data_len;
		} data_available;
		struct {
			struct stk_channel channel;
			struct stk_bearer_description bearer_desc;
			struct stk_other_address address;
		} channel_status;
		struct stk_access_technologies access_technology_change;
		struct stk_display_parameters display_params_changed;
		struct {
			/*
			 * Note the service record subfield is not required,
			 * only the Technology id and Service id.
			 */
			struct stk_service_record service_record;
			struct stk_remote_entity_address remote_addr;
			struct stk_uicc_te_interface transport_level;
			/* Only present if transport_level present */
			struct stk_other_address transport_addr;
		} local_connection;
		enum stk_network_search_mode network_search_mode_change;
		struct stk_common_byte_array browsing_status;
		struct stk_frames_info frames_information_change;
		enum stk_i_wlan_access_status i_wlan_access_status;
		struct {
			struct stk_location_info location;
			struct stk_routing_area_info rai;
			struct stk_tracking_area_id tai;
			enum stk_access_technology_type access_tech;
			enum stk_update_attach_type update_attach;
			enum stk_rejection_cause_code cause;
		} network_rejection;
	};
};

struct stk_envelope_timer_expiration {
	unsigned char id;
	struct stk_timer_value value;
};

struct stk_envelope_ussd_data_download {
	struct stk_ussd_string string;
};

struct stk_envelope_mms_transfer_status {
	struct stk_file transfer_file;
	struct stk_mms_id id;
	struct stk_mms_transfer_status transfer_status;
};

struct stk_envelope_mms_notification_download {
	struct stk_common_byte_array msg;
	ofono_bool_t last;
};

struct stk_envelope_terminal_apps {
	struct stk_registry_application_data *list;
	int count;
	ofono_bool_t last;
};

struct stk_envelope {
	enum stk_envelope_type type;
	enum stk_device_identity_type src;
	enum stk_device_identity_type dst;
	union {
		struct stk_envelope_sms_pp_download sms_pp_download;
		struct stk_envelope_cbs_pp_download cbs_pp_download;
		struct stk_envelope_menu_selection menu_selection;
		struct stk_envelope_call_control call_control;
		struct stk_envelope_sms_mo_control sms_mo_control;
		struct stk_envelope_event_download event_download;
		struct stk_envelope_timer_expiration timer_expiration;
		struct stk_envelope_ussd_data_download ussd_data_download;
		struct stk_envelope_mms_transfer_status mms_status;
		struct stk_envelope_mms_notification_download mms_notification;
		struct stk_envelope_terminal_apps terminal_apps;
	};
};

struct stk_command *stk_command_new_from_pdu(const unsigned char *pdu,
						unsigned int len);
void stk_command_free(struct stk_command *command);

const unsigned char *stk_pdu_from_response(const struct stk_response *response,
						unsigned int *out_length);
const unsigned char *stk_pdu_from_envelope(const struct stk_envelope *envelope,
						unsigned int *out_length);
char *stk_text_to_html(const char *text,
				const unsigned short *attrs, int num_attrs);
char *stk_image_to_xpm(const unsigned char *img, unsigned int len,
			enum stk_img_scheme scheme, const unsigned char *clut,
			unsigned short clut_len);
