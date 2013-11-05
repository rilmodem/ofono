/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013  Canonical Ltd.
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
#define ROOTMF "3F00"

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
	guchar db_path[6] = { 0x00 };
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

	if (len > 0) {
		hex_path = encode_hex(db_path, len, 0);
		parcel_w_string(rilp, hex_path);

		g_ril_append_print_buf(ril,
					"%spath=%s,",
					print_buf,
					hex_path);

		g_free(hex_path);
	} else if (fileid == SIM_EF_ICCID_FILEID || fileid == SIM_EFPL_FILEID) {
		/*
		 * Special catch-all for EF_ICCID (unique card ID)
		 * and EF_PL files which exist in the root directory.
		 * As the sim_info_cb function may not have yet
		 * recorded the app_type for the SIM, and the path
		 * for both files is the same for 2g|3g, just hard-code.
		 *
		 * See 'struct ef_db' in:
		 * ../../src/simutil.c for more details.
		 */
		parcel_w_string(rilp, ROOTMF);

		g_ril_append_print_buf(ril,
					"%spath=%s,",
					print_buf,
					ROOTMF);
	} else {
		/*
		 * The only known case of this is EFPHASE_FILED (0x6FAE).
		 * The ef_db table ( see /src/simutil.c ) entry for
		 * EFPHASE contains a value of 0x0000 for it's
		 * 'parent3g' member.  This causes a NULL path to
		 * be returned.
		 */
		parcel_w_string(rilp, NULL);
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

	DBG("");

	if (req->tech < RADIO_TECH_GPRS || req->tech > RADIO_TECH_GSM) {
		ofono_error("%s: Invalid tech value: %d", __func__, req->tech);
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

	parcel_w_int32(rilp, SETUP_DATA_CALL_PARAMS);

	tech_str = g_strdup_printf("%d", req->tech);
	parcel_w_string(rilp, tech_str);
	parcel_w_string(rilp, profile_str);
	parcel_w_string(rilp, req->apn);
	parcel_w_string(rilp, req->username);
	parcel_w_string(rilp, req->password);

	auth_str = g_strdup_printf("%d", req->auth_type);
	parcel_w_string(rilp, auth_str);
	parcel_w_string(rilp, protocol_str);

	g_ril_append_print_buf(gril,
				"(%s,%s,%s,%s,%s,%s,%s)",
				tech_str,
				profile_str,
				req->apn,
				req->username,
				req->password,
				auth_str,
				protocol_str);

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
				CMD_GET_RESPONSE,
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
				__func__, req->passwd_type);
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
