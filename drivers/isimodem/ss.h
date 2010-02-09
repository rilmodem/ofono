/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Alexander Kanavin <alexander.kanavin@nokia.com>
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

#ifndef __ISIMODEM_SS_H
#define __ISIMODEM_SS_H

#define PN_SS			0x06
#define SS_TIMEOUT		15
#define SS_MAX_USSD_LENGTH	160

enum ss_message_id {
	SS_SERVICE_REQ = 0x00,
	SS_SERVICE_COMPLETED_RESP = 0x01,
	SS_SERVICE_FAILED_RESP = 0x02,
	SS_GSM_USSD_SEND_REQ = 0x04,
	SS_GSM_USSD_SEND_RESP = 0x05,
	SS_GSM_USSD_RECEIVE_IND = 0x06,
	SS_STATUS_IND = 0x09,
	SS_COMMON_MESSAGE = 0xF0
};

enum ss_ussd_type {
	SS_GSM_USSD_MT_REPLY = 0x01,
	SS_GSM_USSD_COMMAND = 0x02,
	SS_GSM_USSD_REQUEST = 0x03,
	SS_GSM_USSD_NOTIFY = 0x04,
	SS_GSM_USSD_END = 0x05
};

enum ss_ussd_status {
	SS_GSM_STATUS_REQUEST_USSD_START = 0x02,
	SS_GSM_STATUS_REQUEST_USSD_STOP = 0x03,
	SS_GSM_STATUS_REQUEST_USSD_FAILED = 0x04
};

enum ss_operations {
	SS_ACTIVATION = 0x01,
	SS_DEACTIVATION = 0x02,
	SS_REGISTRATION = 0x03,
	SS_ERASURE = 0x04,
	SS_INTERROGATION = 0x05,
	SS_GSM_PASSWORD_REGISTRATION = 0x06
};

enum ss_basic_service_codes {
	SS_ALL_TELE_AND_BEARER = 0x00,
	SS_GSM_ALL_TELE = 0x0A,
	SS_GSM_TELEPHONY = 0x0B,
	SS_GSM_ALL_DATA_TELE = 0x0C,
	SS_GSM_FACSIMILE = 0x0D,
	SS_GSM_SMS = 0x10,
	SS_GSM_VOICE_GROUP = 0x11,
	SS_GSM_ALL_TELE_EXC_SMS = 0x13,
	SS_GSM_ALL_BEARER = 0x14,
	SS_GSM_ALL_ASYNC = 0x15,
	SS_GSM_ALL_SYNC = 0x16,
	SS_GSM_ALL_DATA_CIRCUIT_SYNC = 0x18,
	SS_GSM_ALL_DATA_CIRCUIT_ASYNC = 0x19,
	SS_GSM_ALL_DATA_PACKET_SYNC = 0x1A,
	SS_GSM_ALL_PAD_ACCESS = 0x1B
};

enum ss_codes {
	SS_GSM_ALL_FORWARDINGS = 0x02,
	SS_GSM_ALL_COND_FORWARDINGS = 0x04,
	SS_GSM_FORW_UNCONDITIONAL = 0x15,
	SS_GSM_BARR_ALL_OUT = 0x21,
	SS_GSM_BARR_ALL_IN = 0x23,
	SS_GSM_CALL_WAITING = 0x2B,
	SS_GSM_FORW_NO_REPLY = 0x3D,
	SS_GSM_FORW_NO_REACH = 0x3E,
	SS_GSM_FORW_BUSY = 0x43,
	SS_GSM_ALL_BARRINGS = 0x014A,
	SS_GSM_BARR_OUT_INTER = 0x014B,
	SS_GSM_BARR_OUT_INTER_EXC_HOME = 0x014C,
	SS_GSM_BARR_ALL_IN_ROAM = 0x015F
};

enum ss_response_data {
	SS_SEND_ADDITIONAL_INFO = 0x01
};

enum ss_subblock {
	SS_FORWARDING = 0x00,
	SS_STATUS_RESULT = 0x01,
	SS_GSM_PASSWORD = 0x03,
	SS_GSM_FORWARDING_INFO = 0x04,
	SS_GSM_FORWARDING_FEATURE = 0x05,
	SS_GSM_DATA = 0x08,
	SS_GSM_BSC_INFO = 0x09,
	SS_GSM_PASSWORD_INFO = 0x0B,
	SS_GSM_INDICATE_PASSWORD_ERROR = 0x0D,
	SS_GSM_INDICATE_ERROR = 0x0E,
	SS_GSM_ADDITIONAL_INFO = 0x2F,
	SS_GSM_USSD_STRING = 0x32
};

enum ss_isi_cause {
	SS_GSM_ACTIVE = 0x01,
	SS_GSM_REGISTERED = 0x02,
	SS_GSM_PROVISIONED = 0x04,
	SS_GSM_QUIESCENT = 0x08
};

#endif /* __ISIMODEM_SS_H */
