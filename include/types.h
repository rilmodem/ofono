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

#ifndef __OFONO_TYPES_H
#define __OFONO_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef	FALSE
#define	FALSE	(0)
#endif

#ifndef	TRUE
#define	TRUE	(!FALSE)
#endif

typedef int		ofono_bool_t;

/* MCC is always three digits. MNC is either two or three digits */
#define OFONO_MAX_MCC_LENGTH 3
#define OFONO_MAX_MNC_LENGTH 3

typedef void (*ofono_destroy_func)(void *data);

/* 27.007 Section 6.2 */
enum ofono_clir_option {
	OFONO_CLIR_OPTION_DEFAULT = 0,
	OFONO_CLIR_OPTION_INVOCATION,
	OFONO_CLIR_OPTION_SUPPRESSION
};

enum ofono_error_type {
	OFONO_ERROR_TYPE_NO_ERROR = 0,
	OFONO_ERROR_TYPE_CME,
	OFONO_ERROR_TYPE_CMS,
	OFONO_ERROR_TYPE_CEER,
	OFONO_ERROR_TYPE_SIM,
	OFONO_ERROR_TYPE_FAILURE
};

enum ofono_disconnect_reason {
	OFONO_DISCONNECT_REASON_UNKNOWN = 0,
	OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
	OFONO_DISCONNECT_REASON_REMOTE_HANGUP,
	OFONO_DISCONNECT_REASON_ERROR,
};

struct ofono_error {
	enum ofono_error_type type;
	int error;
};

#define OFONO_MAX_PHONE_NUMBER_LENGTH 80
#define OFONO_MAX_CALLER_NAME_LENGTH 80

struct ofono_phone_number {
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 1];
	int type;
};

/* Length of NUM_FIELDS in 3GPP2 C.S0005-E v2.0 */
#define OFONO_CDMA_MAX_PHONE_NUMBER_LENGTH 256

struct ofono_cdma_phone_number {
	/* char maps to max size of CHARi (8 bit) in 3GPP2 C.S0005-E v2.0 */
	char number[OFONO_CDMA_MAX_PHONE_NUMBER_LENGTH];
};

struct ofono_call {
	unsigned int id;
	int type;
	int direction;
	int status;
	struct ofono_phone_number phone_number;
	struct ofono_phone_number called_number;
	char name[OFONO_MAX_CALLER_NAME_LENGTH + 1];
	int clip_validity;
	int cnap_validity;
};

struct ofono_network_time {
	int sec;	/* Seconds [0..59], -1 if unavailable */
	int min;	/* Minutes [0..59], -1 if unavailable */
	int hour;	/* Hours [0..23], -1 if unavailable */
	int mday;	/* Day of month [1..31], -1 if unavailable */
	int mon;	/* Month [1..12], -1 if unavailable */
	int year;	/* Current year, -1 if unavailable */
	int dst;	/* Current adjustment, in hours */
	int utcoff;	/* Offset from UTC in seconds */
};

#define OFONO_SHA1_UUID_LEN 20

struct ofono_uuid {
	unsigned char uuid[OFONO_SHA1_UUID_LEN];
};

/* HFP AG supported features bitmap. Bluetooth HFP 1.5 spec page 77 */
enum hfp_ag_feature {
	HFP_AG_FEATURE_3WAY =			0x1,
	HFP_AG_FEATURE_ECNR =			0x2,
	HFP_AG_FEATURE_VOICE_RECOG =		0x4,
	HFP_AG_FEATURE_IN_BAND_RING_TONE =	0x8,
	HFP_AG_FEATURE_ATTACH_VOICE_TAG =	0x10,
	HFP_AG_FEATURE_REJECT_CALL =		0x20,
	HFP_AG_FEATURE_ENHANCED_CALL_STATUS =	0x40,
	HFP_AG_FEATURE_ENHANCED_CALL_CONTROL =	0x80,
	HFP_AG_FEATURE_EXTENDED_RES_CODE =	0x100
};

/* HFP HF supported features bitmap. Bluetooth HFP 1.5 spec page 77 */
enum hfp_hf_feature {
	HFP_HF_FEATURE_ECNR =			0x1,
	HFP_HF_FEATURE_3WAY =			0x2,
	HFP_HF_FEATURE_CLIP =			0x4,
	HFP_HF_FEATURE_VOICE_RECOGNITION =	0x8,
	HFP_HF_FEATURE_REMOTE_VOLUME_CONTROL =	0x10,
	HFP_HF_FEATURE_ENHANCED_CALL_STATUS =	0x20,
	HFP_HF_FEATURE_ENHANCED_CALL_CONTROL =	0x40
};

const char *ofono_uuid_to_str(const struct ofono_uuid *uuid);
void ofono_call_init(struct ofono_call *call);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_TYPES_H */
