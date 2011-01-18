/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011  Nokia Corporation. All rights reserved.
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
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#define CDMA_SMS_MAX_ADDR_FIELDS 256
#define CDMA_SMS_UD_LEN 512

/* 3GPP2 C.S0015-B v2.0, Table 3.4-1 */
enum cdma_sms_tp_msg_type {
	CDMA_SMS_TP_MSG_TYPE_P2P =	0,
	CDMA_SMS_TP_MSG_TYPE_BCAST =	1,
	CDMA_SMS_TP_MSG_TYPE_ACK =	2
};

/*
 * 3GPP2 X.S0004-550-E, Section 2.256
 * Only supported by 3GPP2 C.S0015-B v2.0 Section 3.4.3.1 listed.
 */
enum cdma_sms_teleservice_id {
	CDMA_SMS_TELESERVICE_ID_CMT91 =	4096,
	CDMA_SMS_TELESERVICE_ID_WPT =	4097,
	CDMA_SMS_TELESERVICE_ID_WMT =	4098,
	CDMA_SMS_TELESERVICE_ID_VMN =	4099,
	CDMA_SMS_TELESERVICE_ID_WAP =	4100,
	CDMA_SMS_TELESERVICE_ID_WEMT =	4101,
	CDMA_SMS_TELESERVICE_ID_SCPT =	4102,
	CDMA_SMS_TELESERVICE_ID_CATPT =	4103
};

/* 3GPP2 C.S0015-B v2.0 Section 3.4.3.3 */
enum cdma_sms_num_mode {
	CDMA_SMS_NUM_MODE_DIGIT =	0,
	CDMA_SMS_NUM_MODE_DATA_NW =	1
};

/* 3GPP2 C.S0005-E v2.0 Table 2.7.1.3.2.4-2 */
enum cdma_sms_digi_num_type {
	CDMA_SMS_DIGI_NUM_TYPE_UNKNOWN =	0,
	CDMA_SMS_DIGI_NUM_TYPE_INTERNATIONAL =	1,
	CDMA_SMS_DIGI_NUM_TYPE_NATIONAL =	2,
	CDMA_SMS_DIGI_NUM_TYPE_NETWORK =	3,
	CDMA_SMS_DIGI_NUM_TYPE_SUBSCRIBER =	4,
	CDMA_SMS_DIGI_NUM_TYPE_RESERVED1 =	5,
	CDMA_SMS_DIGI_NUM_TYPE_ABBREVIATED =	6,
	CDMA_SMS_DIGI_NUM_TYPE_RESERVED2 =	7
};

/* 3GPP2 C.S0015-B v2.0 Table 3.4.3.3-1 */
enum cdma_sms_data_nw_num_type {
	CDMA_SMS_DATA_NW_NUM_TYPE_UNKNOWN =			0,
	CDMA_SMS_DATA_NW_NUM_TYPE_INTERNET_PROTOCOL =		1,
	CDMA_SMS_DATA_NW_NUM_TYPE_INTERNET_EMAIL_ADDRESS =	2,
	/* All Other Values Reserved */
};

/* 3GPP2 C.S0005-E v2.0 Table 2.7.1.3.2.4-3 */
enum cdma_sms_numbering_plan {
	CDMA_SMS_NUMBERING_PLAN_UNKNOWN =	0,
	CDMA_SMS_NUMBERING_PLAN_ISDN =		1,
	CDMA_SMS_NUMBERING_PLAN_DATA =		3,
	CDMA_SMS_NUMBERING_PLAN_TELEX =		4,
	CDMA_SMS_NUMBERING_PLAN_PRIVATE =	9,
	CDMA_SMS_NUMBERING_PLAN_RESERVED =	15
};

/* 3GPP2 C.S0015-B v2.0 Table 4.5.1-1 */
enum cdma_sms_msg_type {
	CDMA_SMS_MSG_TYPE_RESERVED =		0,
	CDMA_SMS_MSG_TYPE_DELIVER =		1,
	CDMA_SMS_MSG_TYPE_SUBMIT =		2,
	CDMA_SMS_MSG_TYPE_CANCEL =		3,
	CDMA_SMS_MSG_TYPE_DELIVER_ACK =		4,
	CDMA_SMS_MSG_TYPE_USER_ACK =		5,
	CDMA_SMS_MSG_TYPE_READ_ACK =		6,
	CDMA_SMS_MSG_TYPE_DELIVER_REPORT =	7,
	CDMA_SMS_MSG_TYPE_SUBMIT_REPORT =	8,
};

/* C.R1001-G_v1.0 Table 9.1-1 */
enum cdma_sms_msg_encoding {
	CDMA_SMS_MSG_ENCODING_OCTET =			0,
	CDMA_SMS_MSG_ENCODING_EXTENDED_PROTOCOL_MSG =	1,
	CDMA_SMS_MSG_ENCODING_7BIT_ASCII =		2,
	CDMA_SMS_MSG_ENCODING_IA5 =			3,
	CDMA_SMS_MSG_ENCODING_UNICODE =			4,
	CDMA_SMS_MSG_ENCODING_SHIFT_JIS =		5,
	CDMA_SMS_MSG_ENCODING_KOREAN =			6,
	CDMA_SMS_MSG_ENCODING_LATIN_HEBREW =		7,
	CDMA_SMS_MSG_ENCODING_LATIN =			8,
	CDMA_SMS_MSG_ENCODING_GSM_7BIT =		9,
	CDMA_SMS_MSG_ENCODING_GSM_DATA_CODING =		10
};

/* 3GPP2 C.S0015-B v2.0 Table 3.4.3-1 */
enum cdma_sms_param_id {
	CDMA_SMS_PARAM_ID_TELESERVICE_IDENTIFIER  =	0x00,
	CDMA_SMS_PARAM_ID_SERVICE_CATEGORY =		0x01,
	CDMA_SMS_PARAM_ID_ORIGINATING_ADDRESS =		0x02,
	CDMA_SMS_PARAM_ID_ORIGINATING_SUBADDRESS =	0x03,
	CDMA_SMS_PARAM_ID_DESTINATION_ADDRESS =		0x04,
	CDMA_SMS_PARAM_ID_DESTINATION_SUBADDRESS =	0x05,
	CDMA_SMS_PARAM_ID_BEARER_REPLY_OPTION =		0x06,
	CDMA_SMS_PARAM_ID_CAUSE_CODE =			0x07,
	CDMA_SMS_PARAM_ID_BEARER_DATA =			0x08
};

/* 3GPP2 C.S0015-B v2.0 Table 4.5-1 */
enum cdma_sms_subparam_id {
	CDMA_SMS_SUBPARAM_ID_MESSAGE_ID =			0x00,
	CDMA_SMS_SUBPARAM_ID_USER_DATA =			0x01,
	CDMA_SMS_SUBPARAM_ID_USER_RESPONSE_CODE =		0x02,
	CDMA_SMS_SUBPARAM_ID_MC_TIME_STAMP =			0x03,
	CDMA_SMS_SUBPARAM_ID_VALIDITY_PERIOD_ABSOLUTE =		0x04,
	CDMA_SMS_SUBPARAM_ID_VALIDITY_PERIOD_RELATIVE =		0x05,
	CDMA_SMS_SUBPARAM_ID_DEFERRED_DELIVERY_TIME_ABSOLUTE =	0x06,
	CDMA_SMS_SUBPARAM_ID_DEFERRED_DELIVERY_TIME_RELATIVE =	0x07,
	CDMA_SMS_SUBPARAM_ID_PRIORITY_INDICATOR =		0x08,
	CDMA_SMS_SUBPARAM_ID_PRIVACY_INDICATOR =		0x09,
	CDMA_SMS_SUBPARAM_ID_REPLY_OPTION =			0x0A,
	CDMA_SMS_SUBPARAM_ID_NUMBER_OF_MESSAGES =		0x0B,
	CDMA_SMS_SUBPARAM_ID_ALERT_ON_MESSAGE_DELIVERY =	0x0C,
	CDMA_SMS_SUBPARAM_ID_LANGUAGE_INDICATOR =		0x0D,
	CDMA_SMS_SUBPARAM_ID_CALL_BACK_NUMBER =			0x0E,
	CDMA_SMS_SUBPARAM_ID_MESSAGE_DISPLAY_MODE =		0x0F,
	CDMA_SMS_SUBPARAM_ID_MULTIPLE_ENCODING_USER_DATA =	0x10,
	CDMA_SMS_SUBPARAM_ID_MESSAGE_DEPOSIT_INDEX =		0x11,
	CDMA_SMS_SUBPARAM_ID_SERVICE_CATEGORY_PROGRAM_DATA =	0x12,
	CDMA_SMS_SUBPARAM_ID_SERVICE_CATEGORY_PROGRAM_RESULT =	0x13,
	CDMA_SMS_SUBPARAM_ID_MESSAGE_STATUS =			0x14,
	CDMA_SMS_SUBPARAM_ID_TP_FAILURE_CAUSE =			0x15,
	CDMA_SMS_SUBPARAM_ID_ENHANCED_VMN =			0x16,
	CDMA_SMS_SUBPARAM_ID_ENHANCED_VMN_ACK =			0x17
};

/* 3GPP2 C.R1001-G Table 9.3.1-1 and 9.3.3-1 */
enum cdma_sms_service_cat {
	CDMA_SMS_SERVICE_CAT_EMERGENCY_BROADCAST =		0x0001,
	CDMA_SMS_SERVICE_CAT_ADMINISTRATIVE =			0x0002,
	CDMA_SMS_SERVICE_CAT_MAINTENANCE =			0x0003,
	CDMA_SMS_SERVICE_CAT_GEN_NEWS_LOCAL =			0x0004,
	CDMA_SMS_SERVICE_CAT_GEN_NEWS_REGIONAL =		0x0005,
	CDMA_SMS_SERVICE_CAT_GEN_NEWS_NATIONAL =		0x0006,
	CDMA_SMS_SERVICE_CAT_GEN_NEWS_INT =			0x0007,
	CDMA_SMS_SERVICE_CAT_FIN_NEWS_LOCAL =			0x0008,
	CDMA_SMS_SERVICE_CAT_FIN_NEWS_REGIONAL =		0x0009,
	CDMA_SMS_SERVICE_CAT_FIN_NEWS_NATIONAL =		0x000A,
	CDMA_SMS_SERVICE_CAT_FIN_NEWS_INT =			0x000B,
	CDMA_SMS_SERVICE_CAT_SPORTS_NEWS_LOCAL =		0x000C,
	CDMA_SMS_SERVICE_CAT_SPORTS_NEWS_REGIONAL =		0x000D,
	CDMA_SMS_SERVICE_CAT_SPORTS_NEWS_NATIONAL =		0x000E,
	CDMA_SMS_SERVICE_CAT_SPORTS_NEWS_INT =			0x000F,
	CDMA_SMS_SERVICE_CAT_ENT_NEWS_LOCAL =			0x0010,
	CDMA_SMS_SERVICE_CAT_ENT_NEWS_REGIONAL =		0x0011,
	CDMA_SMS_SERVICE_CAT_ENT_NEWS_NATIONAL =		0x0012,
	CDMA_SMS_SERVICE_CAT_ENT_NEWS_INT =			0x0013,
	CDMA_SMS_SERVICE_CAT_LOCAL_WEATHER =			0x0014,
	CDMA_SMS_SERVICE_CAT_TRAFFIC_REPORT =			0x0015,
	CDMA_SMS_SERVICE_CAT_FLIGHT_SCHED =			0x0016,
	CDMA_SMS_SERVICE_CAT_RESTAURANT =			0x0017,
	CDMA_SMS_SERVICE_CAT_LODGINGS =				0x0018,
	CDMA_SMS_SERVICE_CAT_RETAIL_DIR =			0x0019,
	CDMA_SMS_SERVICE_CAT_ADVERTISEMENTS =			0x001A,
	CDMA_SMS_SERVICE_CAT_STOCK_QUOTES =			0x001B,
	CDMA_SMS_SERVICE_CAT_EMPLOYMENT =			0x001C,
	CDMA_SMS_SERVICE_CAT_HOSPITAL =				0x001D,
	CDMA_SMS_SERVICE_CAT_TECH_NEWS =			0x001E,
	CDMA_SMS_SERVICE_CAT_MULTICATEGORY =			0x001F,
	CDMA_SMS_SERVICE_CAT_CAPT =				0x0020,
	CDMA_SMS_SERVICE_CAT_PRESIDENTIAL_ALERT =		0x1000,
	CDMA_SMS_SERVICE_CAT_EXTREME_THREAT =			0x1001,
	CDMA_SMS_SERVICE_CAT_SEVERE_THREAT =			0x1002,
	CDMA_SMS_SERVICE_CAT_AMBER =				0x1003,
	CDMA_SMS_SERVICE_CAT_CMAS_TEST =			0x1004
};

/* 3GPP2 C.S0015-B v2.0 Section 3.4.3.3 */
enum cdma_sms_digit_mode {
	CDMA_SMS_DIGIT_MODE_4BIT_DTMF =		0,
	CDMA_SMS_DIGIT_MODE_8BIT_ASCII =	1
};

/* 3GPP2 C.S0015-B v2.0 Section 3.4.3.3 */
struct cdma_sms_address {
	enum cdma_sms_digit_mode digit_mode;
	enum cdma_sms_num_mode number_mode;
	union {
		enum cdma_sms_digi_num_type digi_num_type;
		enum cdma_sms_data_nw_num_type data_nw_num_type;
	};
	enum cdma_sms_numbering_plan number_plan;
	guint8 num_fields;
	guint8 address[CDMA_SMS_MAX_ADDR_FIELDS];
};

/* 3GPP2 C.S0015-B v2.0 Section 3.4.3.6 */
struct cdma_sms_cause_code {
	guint8 reply_seq;
	guint8 error_class;
	guint8 cause_code;
};

/* 3GPP2 C.S0015-B v2.0 Section 4.5.1 */
struct cdma_sms_identifier {
	enum cdma_sms_msg_type msg_type;
	guint16 msg_id;
	gboolean header_ind;
};

/* 3GPP2 C.S0015-B v2.0 Section 4.5.2 */
struct cdma_sms_ud {
	enum cdma_sms_msg_encoding msg_encoding;
	guint8 num_fields;
	guint8 chari[CDMA_SMS_UD_LEN];
};

/*
 * 3GPP2 C.S0015-B v2.0 Table 4.3.4-1.
 * TODO: Not all subparameter records defined
 *       and supported yet.
 */
struct cdma_sms_wmt_deliver {
	struct cdma_sms_ud ud;
};

/* 3GPP2 C.S0015-B v2.0 Section 4.5 */
struct cdma_sms_bearer_data {
	guint32 subparam_bitmap;
	struct cdma_sms_identifier id;
	union {
		struct cdma_sms_wmt_deliver wmt_deliver;
	};
};

/*
 * 3GPP2 C.S0015-B v2.0 Table 3.4.2.1-1.
 * TODO: Not all parameter records defined
 *       and supported yet.
 */
struct cdma_sms_p2p_msg {
	guint32 param_bitmap;
	enum cdma_sms_teleservice_id teleservice_id;
	struct cdma_sms_address oaddr;
	struct cdma_sms_bearer_data bd;
};

/* 3GPP2 C.S0015-B v2.0 Table 3.4.2.2-1 */
struct cdma_sms_broadcast_msg {
	enum cdma_sms_service_cat service_category;
	struct cdma_sms_bearer_data bd;
};

/*
 * 3GPP2 C.S0015-B v2.0 Table 3.4.2.3-1
 * TODO: Not all parameter records defined
 *       and supported yet.
 */
struct cdma_sms_ack_msg {
	struct cdma_sms_address daddr;
	struct cdma_sms_cause_code cause_code;
};

/* 3GPP2 C.S0015-B v2.0 Section 3.4.1 */
struct cdma_sms {
	enum cdma_sms_tp_msg_type type;
	union {
		struct cdma_sms_p2p_msg p2p_msg;
		struct cdma_sms_broadcast_msg broadcast_msg;
		struct cdma_sms_ack_msg ack_msg;
	};
};

static inline gboolean check_bitmap(guint32 bitmap, guint32 pos)
{
	guint32 mask = 0x1 << pos;

	return bitmap & mask ? TRUE : FALSE;
}

gboolean cdma_sms_decode(const guint8 *pdu, guint8 len,
				struct cdma_sms *out);
char *cdma_sms_decode_text(const struct cdma_sms_ud *ud);
const char *cdma_sms_address_to_string(const struct cdma_sms_address *addr);
