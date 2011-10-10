/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __ISIMODEM_SS_H
#define __ISIMODEM_SS_H

#define PN_SS					0x06
#define SS_TIMEOUT				15
#define SS_MAX_USSD_LENGTH			160

enum ss_message_id {
	SS_SERVICE_REQ =			0x00,
	SS_SERVICE_COMPLETED_RESP =		0x01,
	SS_SERVICE_FAILED_RESP =		0x02,
	SS_SERVICE_NOT_SUPPORTED_RESP =		0x03,
	SS_GSM_USSD_SEND_REQ =			0x04,
	SS_GSM_USSD_SEND_RESP =			0x05,
	SS_GSM_USSD_RECEIVE_IND =		0x06,
	SS_STATUS_IND =				0x09,
	SS_SERVICE_COMPLETED_IND =		0x10,
};

enum ss_ussd_type {
	SS_GSM_USSD_MT_REPLY =			0x01,
	SS_GSM_USSD_COMMAND =			0x02,
	SS_GSM_USSD_REQUEST =			0x03,
	SS_GSM_USSD_NOTIFY =			0x04,
	SS_GSM_USSD_END =			0x05
};

enum ss_ussd_status {
	SS_GSM_STATUS_REQUEST_USSD_START =	0x02,
	SS_GSM_STATUS_REQUEST_USSD_STOP =	0x03,
	SS_GSM_STATUS_REQUEST_USSD_FAILED =	0x04
};

enum ss_operations {
	SS_ACTIVATION =				0x01,
	SS_DEACTIVATION =			0x02,
	SS_REGISTRATION =			0x03,
	SS_ERASURE =				0x04,
	SS_INTERROGATION =			0x05,
	SS_GSM_PASSWORD_REGISTRATION =		0x06
};

enum ss_basic_service_codes {
	SS_ALL_TELE_AND_BEARER =		0,
	SS_GSM_ALL_TELE =			10,
	SS_GSM_TELEPHONY =			11,
	SS_GSM_ALL_DATA_TELE =			12,
	SS_GSM_FACSIMILE =			13,
	SS_GSM_SMS =				16,
	SS_GSM_VOICE_GROUP =			17,
	SS_GSM_ALL_TELE_EXC_SMS =		19,
	SS_GSM_ALL_BEARER =			20,
	SS_GSM_ALL_ASYNC =			21,
	SS_GSM_ALL_SYNC =			22,
	SS_GSM_ALL_DATA_CIRCUIT_SYNC =		24,
	SS_GSM_ALL_DATA_CIRCUIT_ASYNC =		25,
	SS_GSM_ALL_DATA_PACKET_SYNC =		26,
	SS_GSM_ALL_PAD_ACCESS =			27
};

enum ss_codes {
	SS_GSM_ALL_FORWARDINGS =		002,
	SS_GSM_ALL_COND_FORWARDINGS =		004,
	SS_GSM_FORW_UNCONDITIONAL =		21,
	SS_GSM_BARR_ALL_OUT =			33,
	SS_GSM_OUTGOING_BARR_SERV =		333,
	SS_GSM_INCOMING_BARR_SERV =		353,
	SS_GSM_BARR_ALL_IN =			35,
	SS_GSM_CALL_WAITING =			43,
	SS_GSM_FORW_NO_REPLY =			61,
	SS_GSM_FORW_NO_REACH =			62,
	SS_GSM_FORW_BUSY =			67,
	SS_GSM_ALL_BARRINGS =			330,
	SS_GSM_BARR_OUT_INTER =			331,
	SS_GSM_BARR_OUT_INTER_EXC_HOME =	332,
	SS_GSM_BARR_ALL_IN_ROAM =		351,
	SS_GSM_CLIP =                           0x001E,
	SS_GSM_CLIR =                           0x001F,
	SS_GSM_COLP =                           0x004C,
	SS_GSM_COLR =                           0x004D,
	SS_GSM_CNAP =                           0x012C,
	SS_GSM_ECT =                            0x0060
};

enum ss_response_data {
	SS_SEND_ADDITIONAL_INFO =		0x01,
};

enum ss_subblock {
	SS_FORWARDING =				0x00,
	SS_STATUS_RESULT =			0x01,
	SS_GSM_PASSWORD =			0x03,
	SS_GSM_FORWARDING_INFO =		0x04,
	SS_GSM_FORWARDING_FEATURE =		0x05,
	SS_GSM_BARRING_INFO =			0x06,
	SS_GSM_BARRING_FEATURE =		0x07,
	SS_GSM_DATA =				0x08,
	SS_GSM_BSC_INFO =			0x09,
	SS_GSM_GENERIC_SERVICE_INFO =		0x0A,
	SS_GSM_PASSWORD_INFO =			0x0B,
	SS_GSM_CLIR_INFO =			0x0C,
	SS_GSM_INDICATE_PASSWORD_ERROR =	0x0D,
	SS_GSM_INDICATE_ERROR =			0x0E,
	SS_GSM_ADDITIONAL_INFO =		0x2F,
	SS_GSM_USSD_STRING =			0x32
};

enum ss_isi_cause {
	SS_GSM_ACTIVE =				0x01,
	SS_GSM_REGISTERED =			0x02,
	SS_GSM_PROVISIONED =			0x04,
	SS_GSM_QUIESCENT =			0x08,
};

enum ss_gsm_cli_restriction_option {
	SS_GSM_CLI_PERMANENT =			0x00,
	SS_GSM_DEFAULT_RESTRICTED =		0x01,
	SS_GSM_CLI_DEFAULT_ALLOWED =		0x02,
	SS_GSM_OVERRIDE_ENABLED =		0x03,
	SS_GSM_OVERRIDE_DISABLED =		0x04
};

enum ss_constants {
	SS_UNDEFINED_TIME =			0x00,
};

/* TS 27.007 Supplementary service notifications +CSSN */
enum ss_cssi {
	SS_MO_UNCONDITIONAL_FORWARDING =	0,
	SS_MO_CONDITIONAL_FORWARDING =		1,
	SS_MO_CALL_FORWARDED =			2,
	SS_MO_CALL_WAITING =			3,
	SS_MO_CUG_CALL =			4,
	SS_MO_OUTGOING_BARRING =		5,
	SS_MO_INCOMING_BARRING =		6,
	SS_MO_CLIR_SUPPRESSION_REJECTED =	7,
	SS_MO_CALL_DEFLECTED =			8,
};

enum ss_cssu {
	SS_MT_CALL_FORWARDED =			0,
	SS_MT_CUG_CALL =			1,
	SS_MT_VOICECALL_ON_HOLD =		2,
	SS_MT_VOICECALL_RETRIEVED =		3,
	SS_MT_MULTIPARTY_VOICECALL =		4,
	SS_MT_VOICECALL_HOLD_RELEASED =		5,
	SS_MT_FORWARD_CHECK_SS_MESSAGE =	6,
	SS_MT_VOICECALL_IN_TRANSFER =		7,
	SS_MT_VOICECALL_TRANSFERRED =		8,
	SS_MT_CALL_DEFLECTED =			9,
};

#endif /* __ISIMODEM_SS_H */
