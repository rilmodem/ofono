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

#ifndef __ISIMODEM_MTC_H
#define __ISIMODEM_MTC_H

#ifdef __cplusplus
extern "C" {
#endif

#define PN_MTC			0x15
#define MTC_TIMEOUT		5

enum mtc_isi_cause {
	MTC_OK = 0x00,
	MTC_FAIL = 0x01,
	MTC_NOT_ALLOWED = 0x02,
	MTC_STATE_TRANSITION_GOING_ON = 0x05,
	MTC_ALREADY_ACTIVE = 0x06,
	MTC_SERVICE_DISABLED = 0x10,
	MTC_NOT_READY_YET = 0x13,
	MTC_NOT_SUPPORTED = 0x14,
	MTC_TRANSITION_ONGOING = 0x16,
	MTC_RESET_REQUIRED = 0x17
};

enum mtc_isi_action {
	MTC_START = 0x03,
	MTC_READY = 0x04,
	MTC_NOS_READY = 0x0C,
	MTC_SOS_START = 0x11,
	MTC_SOS_READY = 0x12,
};

enum mtc_message_id {
	MTC_STATE_REQ = 0x01,
	MTC_STATE_QUERY_REQ = 0x02,
	MTC_POWER_OFF_REQ = 0x03,
	MTC_POWER_ON_REQ = 0x04,
	MTC_STARTUP_SYNQ_REQ = 0x0B,
	MTC_SHUTDOWN_SYNC_REQ = 0x12,
	MTC_STATE_RESP = 0x64,
	MTC_STATE_QUERY_RESP = 0x65,
	MTC_POWER_OFF_RESP = 0x66,
	MTC_POWER_ON_RESP = 0x67,
	MTC_STARTUP_SYNQ_RESP = 0x6E,
	MTC_SHUTDOWN_SYNC_RESP = 0x75,
	MTC_STATE_INFO_IND = 0xC0,
	MTC_COMMON_MESSAGE = 0xF0
};

enum mtc_modem_state {
	MTC_POWER_OFF = 0x00,
	MTC_NORMAL = 0x01,
	MTC_CHARGING = 0x02,
	MTC_ALARM = 0x03,
	MTC_TEST = 0x04,
	MTC_LOCAL = 0x05,
	MTC_WARRANTY = 0x06,
	MTC_RELIABILITY = 0x07,
	MTC_SELFTEST_FAIL = 0x08,
	MTC_SWDL = 0x09,
	MTC_RF_INACTIVE = 0x0A,
	MTC_ID_WRITE = 0x0B,
	MTC_DISCHARGING = 0x0C,
	MTC_DISK_WIPE = 0x0D,
	MTC_SW_RESET = 0x0E,
	MTC_CMT_ONLY_MODE = 0xFF,
	MTC_STATE_NONE = -1,	/* Used only internally */
};

#ifdef __cplusplus
};
#endif

#endif /* __ISIMODEM_MTC_H */
