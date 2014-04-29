/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
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
#include <errno.h>
#include <string.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "grilrequest.h"
#include "simutil.h"
#include "util.h"
#include "common.h"

/* DEACTIVATE_DATA_CALL request parameters */
#define DEACTIVATE_DATA_CALL_NUM_PARAMS 2

/* POWER request parameters */
#define POWER_PARAMS 1

/* SETUP_DATA_CALL_PARAMS request parameters */
#define SETUP_DATA_CALL_PARAMS 7
#define DATA_PROFILE_DEFAULT_STR "0"
#define DATA_PROFILE_TETHERED_STR "1"
#define DATA_PROFILE_IMS_STR "2"
#define DATA_PROFILE_FOTA_STR "3"
#define DATA_PROFILE_CBS_STR "4"
#define DATA_PROFILE_OEM_BASE_STR "1000"

/* SETUP_DATA_CALL_PARAMS reply parameters */
#define MIN_DATA_CALL_REPLY_SIZE 36

/* Commands defined for TS 27.007 +CRSM */
#define CMD_READ_BINARY   176 /* 0xB0   */
#define CMD_READ_RECORD   178 /* 0xB2   */
#define CMD_GET_RESPONSE  192 /* 0xC0   */
#define CMD_UPDATE_BINARY 214 /* 0xD6   */
#define CMD_UPDATE_RECORD 220 /* 0xDC   */
#define CMD_STATUS        242 /* 0xF2   */
#define CMD_RETRIEVE_DATA 203 /* 0xCB   */
#define CMD_SET_DATA      219 /* 0xDB   */

/* FID/path of SIM/USIM root directory */
#define ROOTMF ((char[]) {'\x3F', '\x00'})
#define ROOTMF_SZ sizeof(ROOTMF)

/* RIL_Request* parameter counts */
#define GET_IMSI_NUM_PARAMS 1
#define ENTER_SIM_PIN_PARAMS 2
#define SET_FACILITY_LOCK_PARAMS 5
#define ENTER_SIM_PUK_PARAMS 3
#define CHANGE_SIM_PIN_PARAMS 3

/* RIL_FACILITY_LOCK parameters */
#define RIL_FACILITY_UNLOCK "0"
#define RIL_FACILITY_LOCK "1"

/*
 * TODO:
 *
 * A potential future change here is to create a driver
 * abstraction for each request/reply/event method, and a
 * corresponding method to allow new per-message implementations
 * to be registered.  This would allow PES to easily add code
 * to quirk a particular RIL implementation.
 *
 * struct g_ril_messages_driver {
 *	const char *name;
 * };
 *
 */

static gboolean set_path(GRil *ril, guint app_type,
				struct parcel *rilp,
				const int fileid, const guchar *path,
				const guint path_len)
{
	unsigned char db_path[6] = { 0x00 };
	unsigned char *comm_path = db_path;
	char *hex_path = NULL;
	int len = 0;

	if (path_len > 0 && path_len < 7) {
		memcpy(db_path, path, path_len);
		len = path_len;
	} else if (app_type == RIL_APPTYPE_USIM) {
		len = sim_ef_db_get_path_3g(fileid, db_path);
	} else if (app_type == RIL_APPTYPE_SIM) {
		len = sim_ef_db_get_path_2g(fileid, db_path);
	} else {
		ofono_error("Unsupported app_type: 0%x", app_type);
		return FALSE;
	}

	/*
	 * db_path contains the ID of the MF, but MediaTek modems return an
	 * error if we do not remove it. Other devices work the other way
	 * around: they need the MF in the path. In fact MTK behaviour seem to
	 * be the right one: to have the MF in the file is forbidden following
	 * ETSI TS 102 221, section 8.4.2 (we are accessing the card in mode
	 * "select by path from MF", see 3gpp 27.007, +CRSM).
	 */
	if (g_ril_vendor(ril) == OFONO_RIL_VENDOR_MTK && len >= (int) ROOTMF_SZ
			&& memcmp(db_path, ROOTMF, ROOTMF_SZ) == 0) {
		comm_path = db_path + ROOTMF_SZ;
		len -= ROOTMF_SZ;
	}

	if (len > 0) {
		hex_path = encode_hex(comm_path, len, 0);
		parcel_w_string(rilp, hex_path);

		g_ril_append_print_buf(ril,
					"%spath=%s,",
					print_buf,
					hex_path);

		g_free(hex_path);
	} else {
		/*
		 * The only known case of this is EFPHASE_FILED (0x6FAE).
		 * The ef_db table ( see /src/simutil.c ) entry for
		 * EFPHASE contains a value of 0x0000 for it's
		 * 'parent3g' member.  This causes a NULL path to
		 * be returned.
		 * (EF_PHASE does not exist for USIM)
		 */
		parcel_w_string(rilp, NULL);

		g_ril_append_print_buf(ril,
					"%spath=(null),",
					print_buf);
	}

	return TRUE;
}

gboolean g_ril_request_deactivate_data_call(GRil *gril,
				const struct req_deactivate_data_call *req,
				struct parcel *rilp,
				struct ofono_error *error)
{
	gchar *cid_str = NULL;
	gchar *reason_str = NULL;

	if (req->reason != RIL_DEACTIVATE_DATA_CALL_NO_REASON &&
		req->reason != RIL_DEACTIVATE_DATA_CALL_RADIO_SHUTDOWN) {
		goto error;
	}

	parcel_init(rilp);
	parcel_w_int32(rilp, DEACTIVATE_DATA_CALL_NUM_PARAMS);

	cid_str = g_strdup_printf("%d", req->cid);
	parcel_w_string(rilp, cid_str);

	/*
	 * TODO: airplane-mode; change reason to '1',
	 * which means "radio power off".
	 */
	reason_str = g_strdup_printf("%d", req->reason);
	parcel_w_string(rilp, reason_str);

	g_ril_append_print_buf(gril, "(%s,%s)", cid_str, reason_str);

	g_free(cid_str);
	g_free(reason_str);

	OFONO_NO_ERROR(error);
	return TRUE;

error:
	OFONO_EINVAL(error);
	return FALSE;
}

void g_ril_request_power(GRil *gril,
				const gboolean power,
				struct parcel *rilp)
{
	DBG("");

	parcel_init(rilp);
	parcel_w_int32(rilp, POWER_PARAMS);
	parcel_w_int32(rilp, (int32_t) power);

	g_ril_append_print_buf(gril, "(%d)", power);
}

void g_ril_request_set_net_select_manual(GRil *gril,
					const char *mccmnc,
					struct parcel *rilp)
{
	DBG("");

	parcel_init(rilp);
	parcel_w_string(rilp, mccmnc);

	g_ril_append_print_buf(gril, "(%s)", mccmnc);
}

gboolean g_ril_request_setup_data_call(GRil *gril,
					const struct req_setup_data_call *req,
					struct parcel *rilp,
					struct ofono_error *error)
{
	const gchar *protocol_str;
	gchar *tech_str;
	gchar *auth_str;
	gchar *profile_str;
	size_t apn_len;
	int num_param = SETUP_DATA_CALL_PARAMS;
	const char *request_cid_pr = "";

	DBG("");

	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK)
		num_param = SETUP_DATA_CALL_PARAMS + 1;

	/*
	 * Radio technology to use: 0-CDMA, 1-GSM/UMTS, 2...
	 * values > 2 are (RADIO_TECH + 2)
	 */
	if (req->tech < 1 || req->tech > (RADIO_TECH_GSM + 2)) {
		ofono_error("%s: Invalid tech value: %d",
				__func__,
				req->tech);
		goto error;
	}

	/*
	 * TODO(OEM): This code doesn't currently support
	 * OEM data profiles.  If a use case exist, then
	 * this code will need to be modified.
	 */
	switch (req->data_profile) {
	case RIL_DATA_PROFILE_DEFAULT:
		profile_str = DATA_PROFILE_DEFAULT_STR;
		break;
	case RIL_DATA_PROFILE_TETHERED:
		profile_str = DATA_PROFILE_TETHERED_STR;
		break;
	case RIL_DATA_PROFILE_IMS:
		profile_str = DATA_PROFILE_IMS_STR;
		break;
	case RIL_DATA_PROFILE_FOTA:
		profile_str = DATA_PROFILE_FOTA_STR;
		break;
	case RIL_DATA_PROFILE_CBS:
		profile_str = DATA_PROFILE_CBS_STR;
		break;
	default:
		ofono_error("%s, invalid data_profile value: %d",
				__func__,
				req->data_profile);
		goto error;
	}

	if (req->apn == NULL)
		goto error;

	apn_len = strlen(req->apn);
	if (apn_len == 0 || apn_len > 100) {
		ofono_error("%s: invalid apn length: %d",
				__func__,
				(int) apn_len);
		goto error;
	}

	if (req->auth_type > RIL_AUTH_BOTH) {
		ofono_error("%s: Invalid auth type: %d",
				__func__,
				req->auth_type);
		goto error;
	}

	protocol_str = ril_ofono_protocol_to_ril_string(req->protocol);
	if (protocol_str == NULL) {
		ofono_error("%s: Invalid protocol: %d",
				__func__,
				req->protocol);
		goto error;
	}

	parcel_init(rilp);

	parcel_w_int32(rilp, num_param);

	tech_str = g_strdup_printf("%d", req->tech);
	parcel_w_string(rilp, tech_str);
	parcel_w_string(rilp, profile_str);
	parcel_w_string(rilp, req->apn);
	parcel_w_string(rilp, req->username);
	parcel_w_string(rilp, req->password);

	auth_str = g_strdup_printf("%d", req->auth_type);
	parcel_w_string(rilp, auth_str);
	parcel_w_string(rilp, protocol_str);

	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK) {
		/* MTK request_cid parameter */
		parcel_w_string(rilp, "1");
		request_cid_pr = ",1";
	}

	g_ril_append_print_buf(gril,
				"(%s,%s,%s,%s,%s,%s,%s%s)",
				tech_str,
				profile_str,
				req->apn,
				req->username,
				req->password,
				auth_str,
				protocol_str,
				request_cid_pr);

	g_free(tech_str);
	g_free(auth_str);

	OFONO_NO_ERROR(error);
	return TRUE;

error:
        OFONO_EINVAL(error);
	return FALSE;
}

gboolean g_ril_request_sim_read_info(GRil *gril,
					const struct req_sim_read_info *req,
					struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, CMD_GET_RESPONSE);
	parcel_w_int32(rilp, req->fileid);

	g_ril_append_print_buf(gril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_GET_RESPONSE,
				req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	parcel_w_int32(rilp, 0);           /* P1 */
	parcel_w_int32(rilp, 0);           /* P2 */

	/*
	 * TODO: review parameters values used by Android.
	 * The values of P1-P3 in this code were based on
	 * values used by the atmodem driver impl.
	 *
	 * NOTE:
	 * GET_RESPONSE_EF_SIZE_BYTES == 15; !255
	 */
	parcel_w_int32(rilp, 15);         /* P3 - max length */
	parcel_w_string(rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(rilp, req->aid_str); /* AID (Application ID) */

	return TRUE;

error:
	return FALSE;
}

gboolean g_ril_request_sim_read_binary(GRil *gril,
					const struct req_sim_read_binary *req,
					struct parcel *rilp)
{
	g_ril_append_print_buf(gril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_READ_BINARY,
				req->fileid);

	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_READ_BINARY);
	parcel_w_int32(rilp, req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	parcel_w_int32(rilp, (req->start >> 8));   /* P1 */
	parcel_w_int32(rilp, (req->start & 0xff)); /* P2 */
	parcel_w_int32(rilp, req->length);         /* P3 */
	parcel_w_string(rilp, NULL);          /* data; only req'd for writes */
	parcel_w_string(rilp, NULL);          /* pin2; only req'd for writes */
	parcel_w_string(rilp, req->aid_str);

	return TRUE;

error:
	return FALSE;
}

gboolean g_ril_request_sim_read_record(GRil *gril,
					const struct req_sim_read_record *req,
					struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_READ_RECORD);
	parcel_w_int32(rilp, req->fileid);

	g_ril_append_print_buf(gril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_READ_RECORD,
				req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	parcel_w_int32(rilp, req->record);      /* P1 */
	parcel_w_int32(rilp, 4);           /* P2 */
	parcel_w_int32(rilp, req->length);      /* P3 */
	parcel_w_string(rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(rilp, req->aid_str); /* AID (Application ID) */

	return TRUE;

error:
	return FALSE;
}

gboolean g_ril_request_sim_write_binary(GRil *gril,
					const struct req_sim_write_binary *req,
					struct parcel *rilp)
{
	char *hex_data;
	int p1, p2;

	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_UPDATE_BINARY);
	parcel_w_int32(rilp, req->fileid);

	g_ril_append_print_buf(gril, "(cmd=0x%02X,efid=0x%04X,",
				CMD_UPDATE_BINARY, req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	p1 = req->start >> 8;
	p2 = req->start & 0xff;
	hex_data = encode_hex(req->data, req->length, 0);

	parcel_w_int32(rilp, p1);		/* P1 */
	parcel_w_int32(rilp, p2);		/* P2 */
	parcel_w_int32(rilp, req->length);	/* P3 (Lc) */
	parcel_w_string(rilp, hex_data);	/* data */
	parcel_w_string(rilp, NULL);		/* pin2; only for FDN/BDN */
	parcel_w_string(rilp, req->aid_str);	/* AID (Application ID) */

	g_ril_append_print_buf(gril,
				"%s%d,%d,%d,%s,pin2=(null),aid=%s)",
				print_buf,
				p1,
				p2,
				req->length,
				hex_data,
				req->aid_str);

	g_free(hex_data);

	return TRUE;

error:
	return FALSE;
}

static int get_sim_record_access_p2(enum req_record_access_mode mode)
{
	switch (mode) {
	case GRIL_REC_ACCESS_MODE_CURRENT:
		return 4;
	case GRIL_REC_ACCESS_MODE_ABSOLUTE:
		return 4;
	case GRIL_REC_ACCESS_MODE_NEXT:
		return 2;
	case GRIL_REC_ACCESS_MODE_PREVIOUS:
		return 3;
	}

	return -1;
}

gboolean g_ril_request_sim_write_record(GRil *gril,
					const struct req_sim_write_record *req,
					struct parcel *rilp)
{
	char *hex_data;
	int p2;

	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_UPDATE_RECORD);
	parcel_w_int32(rilp, req->fileid);

	g_ril_append_print_buf(gril, "(cmd=0x%02X,efid=0x%04X,",
				CMD_UPDATE_RECORD, req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	p2 = get_sim_record_access_p2(req->mode);
	hex_data = encode_hex(req->data, req->length, 0);

	parcel_w_int32(rilp, req->record);	/* P1 */
	parcel_w_int32(rilp, p2);		/* P2 (access mode) */
	parcel_w_int32(rilp, req->length);	/* P3 (Lc) */
	parcel_w_string(rilp, hex_data);	/* data */
	parcel_w_string(rilp, NULL);		/* pin2; only for FDN/BDN */
	parcel_w_string(rilp, req->aid_str);	/* AID (Application ID) */

	g_ril_append_print_buf(gril,
				"%s%d,%d,%d,%s,pin2=(null),aid=%s)",
				print_buf,
				req->record,
				p2,
				req->length,
				hex_data,
				req->aid_str);

	g_free(hex_data);

	return TRUE;

error:
	return FALSE;
}

void g_ril_request_read_imsi(GRil *gril,
				const gchar *aid_str,
				struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, GET_IMSI_NUM_PARAMS);
	parcel_w_string(rilp, aid_str);

	g_ril_append_print_buf(gril, "(%d,%s)", GET_IMSI_NUM_PARAMS, aid_str);
}

void g_ril_request_pin_send(GRil *gril,
				const char *passwd,
				const gchar *aid_str,
				struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, ENTER_SIM_PIN_PARAMS);
	parcel_w_string(rilp, passwd);
	parcel_w_string(rilp, aid_str);

	g_ril_append_print_buf(gril, "(%s,aid=%s)", passwd, aid_str);
}

gboolean g_ril_request_pin_change_state(GRil *gril,
					const struct req_pin_change_state *req,
					struct parcel *rilp)
{
	const char *lock_type;

	/*
	 * TODO: clean up the use of string literals &
	 * the multiple g_ril_append_print_buf() calls
	 * by using a table lookup as does the core sim code
	 */
	switch (req->passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		g_ril_append_print_buf(gril, "(SC,");
		lock_type = "SC";
		break;
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
		g_ril_append_print_buf(gril, "(PS,");
		lock_type = "PS";
		break;
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
		g_ril_append_print_buf(gril, "(PF,");
		lock_type = "PF";
		break;
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		g_ril_append_print_buf(gril, "(P2,");
		lock_type = "P2";
		break;
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		g_ril_append_print_buf(gril, "(PN,");
		lock_type = "PN";
		break;
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
		g_ril_append_print_buf(gril, "(PU,");
		lock_type = "PU";
		break;
	case OFONO_SIM_PASSWORD_PHSP_PIN:
		g_ril_append_print_buf(gril, "(PP,");
		lock_type = "PP";
		break;
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		g_ril_append_print_buf(gril, "(PC,");
		lock_type = "PC";
		break;
	default:
		ofono_error("%s: Invalid password type: %d",
				__func__,
				req->passwd_type);
		goto error;
	}

	parcel_init(rilp);
	parcel_w_int32(rilp, SET_FACILITY_LOCK_PARAMS);

	parcel_w_string(rilp, lock_type);

	if (req->enable)
		parcel_w_string(rilp, RIL_FACILITY_LOCK);
	else
		parcel_w_string(rilp, RIL_FACILITY_UNLOCK);

	parcel_w_string(rilp, req->passwd);

	/* TODO: make this a constant... */
	parcel_w_string(rilp, "0");		/* class */

	parcel_w_string(rilp, req->aid_str);

	g_ril_append_print_buf(gril, "(%s,%d,%s,0,aid=%s)",
				print_buf,
				req->enable,
				req->passwd,
				req->aid_str);

	return TRUE;

error:
	return FALSE;
}

void g_ril_request_pin_send_puk(GRil *gril,
				const char *puk,
				const char *passwd,
				const gchar *aid_str,
				struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, ENTER_SIM_PUK_PARAMS);
	parcel_w_string(rilp, puk);
	parcel_w_string(rilp, passwd);
	parcel_w_string(rilp, aid_str);

	g_ril_append_print_buf(gril, "(puk=%s,pin=%s,aid=%s)",
				puk, passwd, aid_str);
}

void g_ril_request_change_passwd(GRil *gril,
					const char *old_passwd,
					const char *new_passwd,
					const gchar *aid_str,
					struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, CHANGE_SIM_PIN_PARAMS);
	parcel_w_string(rilp, old_passwd);
	parcel_w_string(rilp, new_passwd);
	parcel_w_string(rilp, aid_str);

	g_ril_append_print_buf(gril, "(old=%s,new=%s,aid=%s)",
				old_passwd, new_passwd, aid_str);
}

void g_ril_request_sms_cmgs(GRil *gril,
				const struct req_sms_cmgs *req,
				struct parcel *rilp)
{
	int smsc_len;
	char *tpdu;

	parcel_init(rilp);
	parcel_w_int32(rilp, 2);	/* Number of strings */

	/*
	 * SMSC address:
	 *
	 * smsc_len == 1, then zero-length SMSC was spec'd
	 * RILD expects a NULL string in this case instead
	 * of a zero-length string.
	 */
	smsc_len = req->pdu_len - req->tpdu_len;
	/* TODO: encode SMSC & write to parcel */
	if (smsc_len > 1)
		ofono_error("SMSC address specified (smsc_len %d); "
				"NOT-IMPLEMENTED", smsc_len);

	parcel_w_string(rilp, NULL); /* SMSC address; NULL == default */

	/*
	 * TPDU:
	 *
	 * 'pdu' is a raw hexadecimal string
	 *  encode_hex() turns it into an ASCII/hex UTF8 buffer
	 *  parcel_w_string() encodes utf8 -> utf16
	 */
	tpdu = encode_hex(req->pdu + smsc_len, req->tpdu_len, 0);
	parcel_w_string(rilp, tpdu);

	g_ril_append_print_buf(gril, "(%s)", tpdu);

	g_free(tpdu);
}

void g_ril_request_sms_acknowledge(GRil *gril,
					struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 2); /* Number of int32 values in array */
	parcel_w_int32(rilp, 1); /* Successful receipt */
	parcel_w_int32(rilp, 0); /* error code */

	g_ril_append_print_buf(gril, "(1,0)");
}

void g_ril_request_set_smsc_address(GRil *gril,
					const struct ofono_phone_number *sca,
					struct parcel *rilp)
{
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 4];

	if (sca->type == OFONO_NUMBER_TYPE_INTERNATIONAL)
		snprintf(number, sizeof(number), "\"+%s\"", sca->number);
	else
		snprintf(number, sizeof(number), "\"%s\"", sca->number);

	parcel_init(rilp);
	parcel_w_string(rilp, number);

	g_ril_append_print_buf(gril, "(%s)", number);
}

void g_ril_request_dial(GRil *gril,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir,
			struct parcel *rilp)
{
	parcel_init(rilp);

	/* Number to dial */
	parcel_w_string(rilp, phone_number_to_string(ph));
	/* CLIR mode */
	parcel_w_int32(rilp, clir);
	/* USS, empty string */
	/* TODO: Deal with USS properly */
	parcel_w_int32(rilp, 0);
	parcel_w_int32(rilp, 0);

	g_ril_append_print_buf(gril, "(%s,%d,0,0)",
				phone_number_to_string(ph),
				clir);
}

void g_ril_request_hangup(GRil *gril,
				unsigned call_id,
				struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 1); /* Always 1 - AT+CHLD=1x */
	parcel_w_int32(rilp, call_id);

	g_ril_append_print_buf(gril, "(%u)", call_id);
}

void g_ril_request_dtmf(GRil *gril,
			char dtmf_char,
			struct parcel *rilp)
{
	char ril_dtmf[2];

	parcel_init(rilp);
	/* Ril wants just one character, but we need to send as string */
	ril_dtmf[0] = dtmf_char;
	ril_dtmf[1] = '\0';
	parcel_w_string(rilp, ril_dtmf);

	g_ril_append_print_buf(gril, "(%s)", ril_dtmf);
}

void g_ril_request_separate_conn(GRil *gril,
					int call_id,
					struct parcel *rilp)
{
	parcel_init(rilp);

	/* Payload is an array that holds just one element */
	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, call_id);

	g_ril_append_print_buf(gril, "(%d)", call_id);
}

void g_ril_request_set_supp_svc_notif(GRil *gril,
					struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 1); /* size of array */
	parcel_w_int32(rilp, 1); /* notifications enabled */

	g_ril_append_print_buf(gril, "(1)");
}

void g_ril_request_set_mute(GRil *gril, int muted, struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, muted);

	g_ril_append_print_buf(gril, "(%d)", muted);
}

void g_ril_request_send_ussd(GRil *gril,
				const char *ussd,
				struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_string(rilp, ussd);

	g_ril_append_print_buf(gril, "(%s)", ussd);
}

void g_ril_request_set_call_waiting(GRil *gril,
					int enabled, int serviceclass,
					struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, 2);	/* Number of params */
	parcel_w_int32(rilp, enabled);	/* on/off */

	/*
	 * Modem seems to respond with error to all queries
	 * or settings made with bearer class
	 * BEARER_CLASS_DEFAULT. Design decision: If given
	 * class is BEARER_CLASS_DEFAULT let's map it to
	 * SERVICE_CLASS_VOICE effectively making it the
	 * default bearer.
	 */
	if (serviceclass == BEARER_CLASS_DEFAULT)
		serviceclass = BEARER_CLASS_VOICE;

	parcel_w_int32(rilp, serviceclass);	/* Service class */

	g_ril_append_print_buf(gril, "(%d, 0x%x)", enabled, serviceclass);
}

void g_ril_request_query_call_waiting(GRil *gril,
					int serviceclass,
					struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, 1);	/* Number of params */
	/*
	 * RILD expects service class to be 0 as certain carriers can reject the
	 * query with specific service class
	 */
	parcel_w_int32(rilp, 0);

	g_ril_append_print_buf(gril, "(0)");
}

void g_ril_request_set_clir(GRil *gril,
				int mode,
				struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, 1);	/* Number of params */
	parcel_w_int32(rilp, mode);

	g_ril_append_print_buf(gril, "(%d)", mode);
}

void g_ril_request_call_fwd(GRil *gril,	const struct req_call_fwd *req,
				struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, req->action);
	parcel_w_int32(rilp, req->type);
	parcel_w_int32(rilp, req->cls);

	g_ril_append_print_buf(gril, "(type: %d cls: %d ", req->type, req->cls);

	if (req->number != NULL) {
		parcel_w_int32(rilp, req->number->type);
		parcel_w_string(rilp, (char *) req->number->number);

		g_ril_append_print_buf(gril, "%s number type: %d number: "
					"%s time: %d) ", print_buf,
					req->number->type, req->number->number,
					req->time);
	} else {
		/*
		 * The following values have no real meaning for
		 * activation/deactivation/erasure actions, but
		 * apparently rild expects them, so fields need to
		 * be filled. Otherwise there is no response.
		 */

		parcel_w_int32(rilp, 0x81);		/* TOA unknown */
		parcel_w_string(rilp, "1234567890");
		g_ril_append_print_buf(gril, "%s number type: %d number: "
					"%s time: %d) ", print_buf,
					0x81, "1234567890",
					req->time);

	}

	parcel_w_int32(rilp, req->time);
}

void g_ril_request_set_preferred_network_type(GRil *gril, int net_type,
						struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, 1);	/* Number of params */
	parcel_w_int32(rilp, net_type);

	g_ril_append_print_buf(gril, "(%d)", net_type);
}

void g_ril_request_query_facility_lock(GRil *gril, const char *facility,
					const char *password, int services,
					struct parcel *rilp)
{
	char svcs_str[4];

	parcel_init(rilp);

	parcel_w_int32(rilp, 4);	/* # of strings */
	parcel_w_string(rilp, facility);
	parcel_w_string(rilp, password);
	snprintf(svcs_str, sizeof(svcs_str), "%d", services);
	parcel_w_string(rilp, svcs_str);
	parcel_w_string(rilp, NULL);	/* AID (for FDN, not yet supported) */

	g_ril_append_print_buf(gril, "(%s,%s,%s,(null))",
				facility, password, svcs_str);
}

void g_ril_request_set_facility_lock(GRil *gril, const char *facility,
					int enable, const char *passwd,
					int services, struct parcel *rilp)
{
	char svcs_str[4];
	const char *enable_str;

	parcel_init(rilp);

	parcel_w_int32(rilp, 5);	/* # of strings */
	parcel_w_string(rilp, facility);
	if (enable)
		enable_str = "1";
	else
		enable_str = "0";
	parcel_w_string(rilp, enable_str);
	parcel_w_string(rilp, passwd);
	snprintf(svcs_str, sizeof(svcs_str), "%d", services);
	parcel_w_string(rilp, svcs_str);
	parcel_w_string(rilp, NULL);	/* AID (for FDN, not yet supported) */

	g_ril_append_print_buf(gril, "(%s,%s,%s,%s,(null))",
				facility, enable_str, passwd, svcs_str);
}

void g_ril_request_change_barring_password(GRil *gril, const char *facility,
						const char *old_passwd,
						const char *new_passwd,
						struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, 3);	/* # of strings */
	parcel_w_string(rilp, facility);
	parcel_w_string(rilp, old_passwd);
	parcel_w_string(rilp, new_passwd);

	g_ril_append_print_buf(gril, "(%s,%s,%s)",
				facility, old_passwd, new_passwd);
}
