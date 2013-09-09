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

/* DEACTIVATE_DATA_CALL request parameters */
#define DEACTIVATE_DATA_CALL_NUM_PARAMS 2

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
	parcel_w_string(rilp, (char *) tech_str);
	parcel_w_string(rilp, (char *) profile_str);
	parcel_w_string(rilp, (char *) req->apn);
	parcel_w_string(rilp, (char *) req->username);
	parcel_w_string(rilp, (char *) req->password);

	auth_str = g_strdup_printf("%d", req->auth_type);
	parcel_w_string(rilp, (char *) auth_str);
	parcel_w_string(rilp, (char *) protocol_str);

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
