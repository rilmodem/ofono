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
};

/* TS 102.223 Section 9.4 */
enum stk_command_type {
	STK_COMMAND_TYPE_REFRESH =			0x01,
	STK_COMMAND_TYPE_POLL_INTERVAL =		0x02,
	STK_COMMAND_TYPE_POLLING_OFF =			0x03,
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
	STK_COMMAND_TYPE_PROVIDE_LOCAL_INFORMATION =	0x26,
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
	STK_COMMAND_TYPE_GET_SERVICE_INFORMATION =	0x46,
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
	STK_DATA_OBJECT_TYPE_PROVISIONING_REFERENCE_FILE =	0x33,
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
	STK_DATA_OBJECT_TYPE_REMOTE_ENTITY_ADDRESS =		0x49,
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
	STK_DATA_OBJECT_TYPE_MMS_REFERENCE =			0x6A,
	STK_DATA_OBJECT_TYPE_MMS_ID =				0x6B,
	STK_DATA_OBJECT_TYPE_MMS_TRANSFER_STATUS =		0x6C,
	STK_DATA_OBJECT_TYPE_MEID =				0x6D,
	STK_DATA_OBJECT_TYPE_CONTENT_ID =			0x6E,
	STK_DATA_OBJECT_TYPE_MMS_NOTIFICATION =			0x6F,
	STK_DATA_OBJECT_TYPE_LAST_ENVELOPE =			0x70,
	STK_DATA_OBJECT_TYPE_REGISTRY_APPLICATION_DATA =	0x71,
	STK_DATA_OBJECT_TYPE_ROUTING_AREA_INFO =		0x73,
	STK_DATA_OBJECT_TYPE_UPDATE_ATTACH_TYPE =		0x74,
	STK_DATA_OBJECT_TYPE_NMEA_SENTENCE =			0x78,
	STK_DATA_OBJECT_TYPE_PLMN_LIST =			0x79,
	STK_DATA_OBJECT_TYPE_BROADCAST_NETWORK_INFO =		0x7A,
	STK_DATA_OBJECT_TYPE_ACTIVATE_DESCRIPTOR =		0x7B,
};

enum stk_device_identity_type {
	STK_DEVICE_IDENTITY_TYPE_KEYPAD =	0x01,
	STK_DEVICE_IDENTITY_TYPE_DISPLAY =	0x02,
	STK_DEVICE_IDENTITY_TYPE_EARPIECE =	0x03,
	STK_DEVICE_IDENTITY_TYPE_UICC =		0x81,
	STK_DEVICE_IDENTITY_TYPE_TERMINAL =	0x82,
	STK_DEVICE_IDENTITY_TYPE_NETWORK =	0x83,
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

	/* 0x20 to 0x2F are used to indicate that SIM should retry */
	STK_RESULT_TYPE_TERMINAL_BUSY =			0x20,
	STK_RESULT_TYPE_NETWORK_UNAVAILABLE =		0x21,
	STK_RESULT_TYPE_USER_REJECT =			0x22,
	STK_RESULT_TYPE_USER_CANCEL =			0x23,
	STK_RESULT_TYPE_TIMER_CONFLICT =		0x24,
	STK_RESULT_TYPE_CALL_CONTROL_TEMPORARY = 	0x25,
	STK_RESULT_TYPE_BROWSER_TEMPORARY =		0x26,
	STK_RESULT_TYPE_MMS_TEMPORARY =			0x27,

	/* 0x30 to 0x3F are used to indicate permanent problems */
	STK_RESULT_TYPE_NOT_CAPABLE =			0x30,
	STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD =	0x31,
	STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD =		0x32,
	STK_RESULT_TYPE_COMMAND_ID_UNKNOWN =		0x33,
	STK_RESULT_TYPE_MINIMUM_NOT_MET =		0x36,
	STK_RESULT_TYPE_CALL_CONTROL_PERMANENT =	0x39,
	STK_RESULT_TYPE_BIP_ERROR =			0x3A,
	STK_RESULT_TYPE_ACCESS_TECHNOLOGY_ERROR =	0x3B,
	STK_RESULT_TYPE_FRAMES_ERROR =			0x3C,
	STK_RESULT_TYPE_MMS_ERROR =			0x3D,
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
 * information as defined for this purpose in TS 24.008 [9]. All information
 * defined in TS 24.008, except the information element identifier, shall be
 * stored in the USIM. The length of this subaddress data can be up to 22
 * bytes."
 */
struct stk_subaddress {
	unsigned char len;
	unsigned char subaddr[23];
};

/*
 * Defined in TS 102.223 Section 8.4
 *
 * According to 24.008 Section 10.5.4.5 "The bearer capability is a type 4
 * information element with a minimum length of 3 octets and a maximum length
 * of 16 octets."
 *
 * According to TS 31.102 Section 4.2.38 the CCP length is 15 bytes.
 *
 * The CCP structure is not decoded, but stored as is from the CTLV
 */
struct stk_ccp {
	unsigned char len;
	unsigned char ccp[16];
};

/*
 * Icon ID denotes a file on the SIM filesystem.  Since EF cannot have record
 * ids of 0, we use icon_id with 0 to denote empty icon_identifier objects
 */
struct stk_icon_identifier {
	unsigned char qualifier;
	unsigned char id;
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

/* Define the struct of single file in TS102.223 Section 8.18.
 * According to TS 11.11 Section 6.2, each file id has two bytes, and the
 * maximum Dedicated File level is 2. So the maximum size of file is 8, which
 * contains two bytes of Master File, 2 bytes of 1st level Dedicated File,
 * 2 bytes of 2nd level Dedicated File and 2 bytes of Elementary File.
 */
struct stk_file {
	unsigned char file[8];
	unsigned int len;
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

struct stk_command_display_text {
	char *text;
	struct stk_icon_identifier icon_id;
	ofono_bool_t immediate_response;
	struct stk_duration duration;
	struct stk_text_attribute text_attribute;
	unsigned char frame_id; /* Values 0x10 to 0xFF reserved */
};

struct stk_command_get_input {
	char *text;
	struct stk_response_length response_length;
	char *default_text;
	struct stk_icon_identifier icon_id;
	struct stk_text_attribute text_attribute;
	unsigned char frame_id; /* Values 0x10 to 0xFF reserved */
};

struct stk_command_send_sms {
	char *alpha_id;
	struct stk_address address;
	struct sms gsm_sms;
	struct stk_icon_identifier icon_id;
	struct stk_text_attribute text_attribute;
	unsigned char frame_id; /* Values 0x10 to 0xFF reserved */
};

struct stk_command {
	unsigned char number;
	unsigned char type;
	unsigned char qualifier;
	enum stk_device_identity_type src;
	enum stk_device_identity_type dst;

	union {
		struct stk_command_display_text display_text;
		struct stk_command_display_text get_inkey;
		struct stk_command_get_input get_input;
		struct stk_command_send_sms send_sms;
	};

	void (*destructor)(struct stk_command *command);
};

struct stk_command *stk_command_new_from_pdu(const unsigned char *pdu,
						unsigned int len);
void stk_command_free(struct stk_command *command);
