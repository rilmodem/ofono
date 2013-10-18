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

#include "util.h"
#include "grilreply.h"
#include "grilutil.h"

/* SETUP_DATA_CALL_PARAMS reply params */
#define MIN_DATA_CALL_REPLY_SIZE 36

/* TODO: move this to grilutil.c */
void g_ril_reply_free_setup_data_call(struct reply_setup_data_call *reply)
{
	if (reply) {
		g_free(reply->ifname);
		g_strfreev(reply->dns_addresses);
		g_strfreev(reply->gateways);
		g_strfreev(reply->ip_addrs);
		g_free(reply);
	}
}

struct reply_setup_data_call *g_ril_reply_parse_data_call(GRil *gril,
							struct ril_msg *message,
							struct ofono_error *error)
{
	struct parcel rilp;
	int num = 0;
	int protocol;
	char *type = NULL, *raw_ip_addrs = NULL;
	char *dnses = NULL, *raw_gws = NULL;

	struct reply_setup_data_call *reply =
		g_new0(struct reply_setup_data_call, 1);

	OFONO_NO_ERROR(error);

	reply->cid = -1;

       /* TODO:
	 * Cleanup duplicate code between this function and
	 * ril_util_parse_data_call_list().
	 */

	/* valid size: 36 (34 if HCRADIO defined) */
	if (message->buf_len < MIN_DATA_CALL_REPLY_SIZE) {
		/* TODO: make a macro for error logging */
		ofono_error("%s: reply too small: %d",
				__func__,
				(int) message->buf_len);
		OFONO_EINVAL(error);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	/*
	 * ril.h documents the reply to a RIL_REQUEST_SETUP_DATA_CALL
	 * as being a RIL_Data_Call_Response_v6 struct, however in
	 * reality, the response actually includes the version of the
	 * struct, followed by an array of calls, so the array size
	 * also has to be read after the version.
	 *
	 * TODO: What if there's more than 1 call in the list??
	 */

	/*
	 * TODO: consider using 'unused' variable; however if we
	 * do this, the alternative is a few more append_print_buf
	 * calls ( which become no-ops if tracing isn't enabled.
	 */
	reply->version = parcel_r_int32(&rilp);
	num = parcel_r_int32(&rilp);
	if (num != 1) {
		ofono_error("%s: too many calls: %d", __func__,	num);
		OFONO_EINVAL(error);
		goto error;
	}

	reply->status = parcel_r_int32(&rilp);
	reply->retry_time = parcel_r_int32(&rilp);
	reply->cid = parcel_r_int32(&rilp);
	reply->active = parcel_r_int32(&rilp);
	type = parcel_r_string(&rilp);
	reply->ifname = parcel_r_string(&rilp);
	raw_ip_addrs = parcel_r_string(&rilp);
	dnses = parcel_r_string(&rilp);
	raw_gws = parcel_r_string(&rilp);

	g_ril_append_print_buf(gril,
				"{version=%d,num=%d [status=%d,retry=%d,"
				"cid=%d,active=%d,type=%s,ifname=%s,address=%s"
				",dns=%s,gateways=%s]}",
				reply->version,
				num,
				reply->status,
				reply->retry_time,
				reply->cid,
				reply->active,
				type,
				reply->ifname,
				raw_ip_addrs,
				dnses,
				raw_gws);

	g_ril_print_response(gril, message);

	protocol = ril_protocol_string_to_ofono_protocol(type);
	if (protocol < 0) {
		ofono_error("%s: Invalid type(protocol) specified: %s",
				__func__,
				type);
		OFONO_EINVAL(error);
		goto error;
	}

	reply->protocol = (guint) protocol;

	if (reply->ifname == NULL || strlen(reply->ifname) == 0) {
		ofono_error("%s: No interface specified: %s",
				__func__,
				reply->ifname);

		OFONO_EINVAL(error);
		goto error;

	}

	/* TODO:
	 * RILD can return multiple addresses; oFono only supports
	 * setting a single IPv4 address.  At this time, we only
	 * use the first address.  It's possible that a RIL may
	 * just specify the end-points of the point-to-point
	 * connection, in which case this code will need to
	 * changed to handle such a device.
	 *
	 * For now split into a maximum of three, and only use
	 * the first address for the remaining operations.
	 */
	if (raw_ip_addrs)
		reply->ip_addrs = g_strsplit(raw_ip_addrs, " ", 3);
	else
		reply->ip_addrs = NULL;

	/* TODO: I'm not sure it's possible to specify a zero-length
	 * in a parcel in a parcel.  If *not*, then this can be
	 * simplified.
	 */
	if (reply->ip_addrs == NULL || (sizeof(reply->ip_addrs) == 0)) {
		ofono_error("%s no IP address: %s", __func__, raw_ip_addrs);

		OFONO_EINVAL(error);
		goto error;
	}

	/*
	 * RILD can return multiple addresses; oFono only supports
	 * setting a single IPv4 gateway.
	 */
	if (raw_gws)
		reply->gateways = g_strsplit(raw_gws, " ", 3);
	else
		reply->gateways = NULL;

	if (reply->gateways == NULL || (sizeof(reply->gateways) == 0)) {
		ofono_error("%s: no gateways: %s", __func__, raw_gws);
		OFONO_EINVAL(error);
		goto error;
	}

	/* Split DNS addresses */
	if (dnses)
		reply->dns_addresses = g_strsplit(dnses, " ", 3);
	else
		reply->dns_addresses = NULL;

	if (reply->dns_addresses == NULL ||
		(sizeof(reply->dns_addresses) == 0)) {
		ofono_error("%s: no DNS: %s", __func__, dnses);

		OFONO_EINVAL(error);
		goto error;
	}

error:
	g_free(type);
	g_free(raw_ip_addrs);
	g_free(dnses);
	g_free(raw_gws);

	return reply;
}

void g_ril_reply_free_sim_io(struct reply_sim_io *reply)
{
	if (reply) {
		g_free(reply->hex_response);
		g_free(reply);
	}
}

struct reply_sim_io *g_ril_reply_parse_sim_io(GRil *gril,
						struct ril_msg *message,
						struct ofono_error *error)
{
	struct parcel rilp;
	char *response = NULL;
	struct reply_sim_io *reply;

	/*
	 * Minimum length of SIM_IO_Response is 12:
	 * sw1 (int32)
	 * sw2 (int32)
	 * simResponse (string)
	 */
	if (message->buf_len < 12) {
		ofono_error("Invalid SIM IO reply: size too small (< 12): %d ",
			    (int) message->buf_len);
		OFONO_EINVAL(error);
		return NULL;
	}

	reply =	g_new0(struct reply_sim_io, 1);

	g_ril_init_parcel(message, &rilp);
	reply->sw1 = parcel_r_int32(&rilp);
	reply->sw2 = parcel_r_int32(&rilp);

	response = parcel_r_string(&rilp);
	if (response) {
		reply->hex_response =
			decode_hex((const char *) response,
					strlen(response),
					(long *) &reply->hex_len, -1);
	}

	g_ril_append_print_buf(gril,
				"(sw1=0x%.2X,sw2=0x%.2X,%s)",
				reply->sw1,
				reply->sw2,
				response);
	g_ril_print_response(gril, message);

	g_free(response);

	OFONO_NO_ERROR(error);

	return reply;
}

gchar *g_ril_reply_parse_imsi(GRil *gril, struct ril_msg *message)
{
	struct parcel rilp;
	gchar *imsi;

	g_ril_init_parcel(message, &rilp);

	/* 15 is the max length of IMSI
	 * add 4 bytes for string length */
	/* FIXME: g_assert(message->buf_len <= 19); */
	imsi = parcel_r_string(&rilp);

	g_ril_append_print_buf(gril, "{%s}", imsi);
	g_ril_print_response(gril, message);

	return imsi;
}

void g_ril_reply_free_sim_status(struct reply_sim_status *status)
{
	if (status) {
		guint i;

		for (i = 0; i < status->num_apps; i++) {
			if (status->apps[i] != NULL) {
				g_free(status->apps[i]->aid_str);
				g_free(status->apps[i]->app_str);
				g_free(status->apps[i]);
			}
		}

		g_free(status);
	}
}

struct reply_sim_status *g_ril_reply_parse_sim_status(GRil *gril,
						struct ril_msg *message,
						struct ofono_error *error)
{
	struct parcel rilp;
	int i;
	struct reply_sim_status *status;

	g_ril_append_print_buf(gril, "[%04d]< %s",
			message->serial_no,
			ril_request_id_to_string(message->req));

	g_ril_init_parcel(message, &rilp);

	/*
	 * FIXME: Need to come up with a common scheme for verifying the
	 * size of RIL message and properly reacting to bad messages.
	 * This could be a runtime assertion, disconnect, drop/ignore
	 * the message, ...
	 *
	 * 20 is the min length of RIL_CardStatus_v6 as the AppState
	 * array can be 0-length.
	 */
	if (message->buf_len < 20) {
		ofono_error("Size of SIM_STATUS reply too small: %d bytes",
			    (int) message->buf_len);
		OFONO_EINVAL(error);
		return NULL;
	}

	status = g_new0(struct reply_sim_status, 1);

	status->card_state = parcel_r_int32(&rilp);

	/*
	 * NOTE:
	 *
	 * The global pin_status is used for multi-application
	 * UICC cards.  For example, there are SIM cards that
	 * can be used in both GSM and CDMA phones.  Instead
	 * of managed PINs for both applications, a global PIN
	 * is set instead.  It's not clear at this point if
	 * such SIM cards are supported by ofono or RILD.
	 */

	status->pin_state = parcel_r_int32(&rilp);
	status->gsm_umts_index = parcel_r_int32(&rilp);
	status->cdma_index = parcel_r_int32(&rilp);
	status->ims_index = parcel_r_int32(&rilp);
	status->num_apps = parcel_r_int32(&rilp);

	/* TODO:
	 * How do we handle long (>80 chars) ril_append_print_buf strings?
	 * Using line wrapping ( via '\' ) introduces spaces in the output.
	 * Do we just make a style-guide exception for PrintBuf operations?
	 */
	g_ril_append_print_buf(gril,
				"(card_state=%d,universal_pin_state=%d,"
				"gsm_umts_index=%d,cdma_index=%d,"
				"ims_index=%d, ",
				status->card_state,
				status->pin_state,
				status->gsm_umts_index,
				status->cdma_index,
				status->ims_index);

	if (status->card_state != RIL_CARDSTATE_PRESENT)
		goto done;

	if (status->num_apps > MAX_UICC_APPS) {
		ofono_error("SIM error; too many apps: %d", status->num_apps);
		status->num_apps = MAX_UICC_APPS;
	}

	for (i = 0; i < status->num_apps; i++) {
		struct reply_sim_app *app;
		DBG("processing app[%d]", i);
		status->apps[i] = g_try_new0(struct reply_sim_app, 1);
		app = status->apps[i];
		if (app == NULL) {
			ofono_error("Can't allocate app_data");
			goto error;
		}

		app->app_type = parcel_r_int32(&rilp);
		app->app_state = parcel_r_int32(&rilp);
		app->perso_substate = parcel_r_int32(&rilp);

		/* TODO: we need a way to instruct parcel to skip
		 * a string, without allocating memory...
		 */
		app->aid_str = parcel_r_string(&rilp); /* application ID (AID) */
		app->app_str = parcel_r_string(&rilp); /* application label */

		app->pin_replaced = parcel_r_int32(&rilp);
		app->pin1_state = parcel_r_int32(&rilp);
		app->pin2_state = parcel_r_int32(&rilp);

		g_ril_append_print_buf(gril,
					"%s[app_type=%d,app_state=%d,"
					"perso_substate=%d,aid_ptr=%s,"
					"app_label_ptr=%s,pin1_replaced=%d,"
					"pin1=%d,pin2=%d],",
					print_buf,
					app->app_type,
					app->app_state,
					app->perso_substate,
					app->aid_str,
					app->app_str,
					app->pin_replaced,
					app->pin1_state,
					app->pin2_state);
	}

done:
	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	OFONO_NO_ERROR(error);

	return status;

error:
	g_ril_reply_free_sim_status(status);
	OFONO_EINVAL(error);

	return NULL;
}
