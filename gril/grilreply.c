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

#include "common.h"
#include "util.h"
#include "grilreply.h"
#include "grilutil.h"

#define OPERATOR_NUM_PARAMS 3

/* SETUP_DATA_CALL_PARAMS reply params */
#define MIN_DATA_CALL_REPLY_SIZE 36

static const char *handle_tech(gint req, gchar *stech, gint *tech) {

	/*
	 * This code handles the mapping between the RIL_RadioTechnology
	 * based upon whether this a reply to a voice registration request
	 * ( see <Act> values - 27.007 Section 7.3 ), or a reply to a
	 * data registration request ( see <curr_bearer> values - 27.007
	 * Section 7.29 ).  The two sets of constants are similar, but
	 * sublty different.
	 */

	g_assert(tech);

	if (req == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
		if (stech) {
			switch(atoi(stech)) {
			case RADIO_TECH_UNKNOWN:
				*tech = -1;
				break;
			case RADIO_TECH_GSM:
			case RADIO_TECH_GPRS:
				*tech = ACCESS_TECHNOLOGY_GSM;
				break;
			case RADIO_TECH_EDGE:
				*tech = ACCESS_TECHNOLOGY_GSM_EGPRS;
				break;
			case RADIO_TECH_UMTS:
				*tech = ACCESS_TECHNOLOGY_UTRAN;
				break;
			case RADIO_TECH_HSDPA:
				*tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA;
				break;
			case RADIO_TECH_HSUPA:
				*tech = ACCESS_TECHNOLOGY_UTRAN_HSUPA;
				break;
			case RADIO_TECH_HSPAP:
			case RADIO_TECH_HSPA:
				/* HSPAP is HSPA+; which ofono doesn't define;
				 * so, if differentiating HSPA and HSPA+ is
				 * important, then ofono needs to be patched,
				 * and we probably also need to introduce a
				 * new indicator icon.
				 */

				*tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
				break;
			case RADIO_TECH_LTE:
				*tech = ACCESS_TECHNOLOGY_EUTRAN;
				break;
			default:
				*tech = -1;
			}
		} else
			*tech = -1;

		return registration_tech_to_string(*tech);
	} else {
		if (stech) {
			switch(atoi(stech)) {
			case RADIO_TECH_GSM:
			case RADIO_TECH_UNKNOWN:
				*tech = PACKET_BEARER_NONE;
				break;
			case RADIO_TECH_GPRS:
				*tech = PACKET_BEARER_GPRS;
				break;
			case RADIO_TECH_EDGE:
				*tech = PACKET_BEARER_EGPRS;
				break;
			case RADIO_TECH_UMTS:
				*tech = PACKET_BEARER_UMTS;
				break;
			case RADIO_TECH_HSDPA:
				*tech = PACKET_BEARER_HSDPA;
				break;
			case RADIO_TECH_HSUPA:
				*tech = PACKET_BEARER_HSUPA;
				break;
			case RADIO_TECH_HSPAP:
			case RADIO_TECH_HSPA:
				/* HSPAP is HSPA+; which ofono doesn't define;
				 * so, if differentiating HSPA and HSPA+ is
				 * important, then ofono needs to be patched,
				 * and we probably also need to introduce a
				 * new indicator icon.
				 */
				*tech = PACKET_BEARER_HSUPA_HSDPA;
				break;
			case RADIO_TECH_LTE:
				*tech = PACKET_BEARER_EPS;
				break;
			default:
				*tech = PACKET_BEARER_NONE;
			}
		} else
			*tech = PACKET_BEARER_NONE;

		return packet_bearer_to_string(*tech);
	}
}

static void ril_reply_free_operator(gpointer data)
{
	struct reply_operator *reply = data;

	if (reply) {
		g_free(reply->lalpha);
		g_free(reply->salpha);
		g_free(reply->numeric);
		g_free(reply->status);
		g_free(reply);
	}
}

void g_ril_reply_free_avail_ops(struct reply_avail_ops *reply)
{
	if (reply) {
		g_slist_free_full(reply->list, ril_reply_free_operator);
		g_free(reply);
	}
}

struct reply_avail_ops *g_ril_reply_parse_avail_ops(GRil *gril,
						struct ril_msg *message) {
	struct parcel rilp;
	struct reply_operator *operator;
	struct reply_avail_ops *reply;
	int num_ops, num_strings;
	unsigned int i;

	/*
	 * Minimum message length is 4:
	 * - array size
	 */
	if (message->buf_len < 4) {
		ofono_error("%s: invalid QUERY_AVAIL_NETWORKS reply: "
				"size too small (< 4): %d ",
				__FUNCTION__,
				(int) message->buf_len);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);
	g_ril_append_print_buf(gril, "{");

	/* Number of operators at the list (4 strings for every operator) */
	num_strings = parcel_r_int32(&rilp);
	if (num_strings % 4) {
		ofono_error("%s: invalid QUERY_AVAIL_NETWORKS reply: "
				"num_strings (%d) MOD 4 != 0",
				__FUNCTION__,
				num_strings);
		goto error;
	}

	num_ops = num_strings / 4;
	DBG("noperators = %d", num_ops);

	reply = g_try_new0(struct reply_avail_ops, 1);
	if (!reply) {
		ofono_error("%s: can't allocate reply struct", __FUNCTION__);
		goto error;
	}

	reply->num_ops = num_ops;
	for (i = 0; i < num_ops; i++) {
		operator = g_try_new0(struct reply_operator, 1);
		if (!operator) {
			ofono_error("%s: can't allocate reply struct",
					__FUNCTION__);
			goto error;
		}

		operator->lalpha = parcel_r_string(&rilp);
		operator->salpha = parcel_r_string(&rilp);
		operator->numeric = parcel_r_string(&rilp);
		operator->status = parcel_r_string(&rilp);

		if (!operator->lalpha && !operator->salpha) {
			ofono_error("%s: operator (%s) doesn't specify names",
					operator->numeric,
					__FUNCTION__);
			g_ril_reply_free_operator(operator);
			continue;
		}

		if (!operator->numeric) {
			ofono_error("%s: operator (%s/%s) "
					"doesn't specify numeric",
					operator->lalpha,
					operator->salpha,
					__FUNCTION__);
			g_ril_reply_free_operator(operator);
			continue;
		}

		reply->list = g_slist_append(reply->list, operator);

		g_ril_append_print_buf(gril,
					"%s [lalpha=%s, salpha=%s, "
					" numeric=%s status=%s]",
					print_buf,
					operator->lalpha,
					operator->salpha,
					operator->numeric,
					operator->status);
	}

	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	return reply;

error:
	if (reply)
		g_ril_reply_free_avail_ops(reply);

	return NULL;
}

void g_ril_reply_free_operator(struct reply_operator *reply)
{
	ril_reply_free_operator(reply);
}

struct reply_operator *g_ril_reply_parse_operator(GRil *gril,
						struct ril_msg *message)
{
	struct parcel rilp;
	int num_params;
	struct reply_operator *reply;

	/*
	 * Minimum message length is 16:
	 * - array size
	 * - 3 NULL strings
	 */
	if (message->buf_len < 16) {
		ofono_error("%s: invalid OPERATOR reply: "
				"size too small (< 16): %d ",
				__FUNCTION__,
				(int) message->buf_len);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	if ((num_params = parcel_r_int32(&rilp)) != OPERATOR_NUM_PARAMS) {
		ofono_error("%s: invalid OPERATOR reply: "
				"number of params is %d; should be 3.",
				__FUNCTION__,
				num_params);
		goto error;
	}

	reply =	g_new0(struct reply_operator, 1);

	reply->lalpha = parcel_r_string(&rilp);
	reply->salpha = parcel_r_string(&rilp);
	reply->numeric = parcel_r_string(&rilp);

	if (!reply->lalpha && !reply->salpha) {
		ofono_error("%s: invalid OPERATOR reply: "
				" no names returned.",
				__FUNCTION__);

		goto error;
	}

	if (!reply->numeric) {
		ofono_error("%s: invalid OPERATOR reply: "
				" no numeric returned.",
				__FUNCTION__);
		goto error;
	}

	g_ril_append_print_buf(gril,
				"(lalpha=%s, salpha=%s, numeric=%s)",
				reply->lalpha, reply->salpha, reply->numeric);

	g_ril_print_response(gril, message);

	return reply;

error:
	if (reply)
		g_ril_reply_free_operator(reply);

	return NULL;
}

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
		ofono_error("%s: SETUP_DATA_CALL reply too small: %d",
				__FUNCTION__,
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
		ofono_error("%s: too many calls: %d", __FUNCTION__, num);
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
				__FUNCTION__,
				type);
		OFONO_EINVAL(error);
		goto error;
	}

	reply->protocol = (guint) protocol;

	if (reply->ifname == NULL || strlen(reply->ifname) == 0) {
		ofono_error("%s: No interface specified: %s",
				__FUNCTION__,
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
		ofono_error("%s no IP address: %s", __FUNCTION__, raw_ip_addrs);

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
		ofono_error("%s: no gateways: %s", __FUNCTION__, raw_gws);
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
		ofono_error("%s: no DNS: %s", __FUNCTION__, dnses);

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

struct reply_reg_state *g_ril_reply_parse_reg_state(GRil *gril,
						struct ril_msg *message)

{
	struct parcel rilp;
	int tmp;
	char *sstatus = NULL, *slac = NULL, *sci = NULL;
	char *stech = NULL, *sreason = NULL, *smax = NULL;
	const char *tech_str;
	struct reply_reg_state *reply;

	DBG("");

	/*
	 * If no array length is present, reply is invalid.
	 */
	if (message->buf_len < 4) {
		ofono_error("%s: invalid %s reply: "
				"size too small (< 4): %d ",
				__FUNCTION__,
				ril_request_id_to_string(message->req),
				(int) message->buf_len);
		return NULL;
	}

	g_ril_init_parcel(message, &rilp);

	reply =	g_new0(struct reply_reg_state, 1);

	/* Size of response string array
	 *
	 * Should be:
	 *   >= 4 for VOICE_REG reply
	 *   >= 5 for DATA_REG reply
	 */
	if ((tmp = parcel_r_int32(&rilp)) < 4) {
		ofono_error("%s: invalid %s; response array is too small: %d",
				__FUNCTION__,
				ril_request_id_to_string(message->req),
				tmp);
		goto error;
	}

	sstatus = parcel_r_string(&rilp);
	slac = parcel_r_string(&rilp);
	sci = parcel_r_string(&rilp);
	stech = parcel_r_string(&rilp);

	tmp -= 4;

	/* FIXME: need to review VOICE_REGISTRATION response
	 * as it returns ~15 parameters ( vs. 6 for DATA ).
	 *
	 * The first four parameters are the same for both
	 * responses ( although status includes values for
	 * emergency calls for VOICE response ).
	 *
	 * Parameters 5 & 6 have different meanings for
	 * voice & data response.
	 */
	if (tmp--) {
		sreason = parcel_r_string(&rilp);        /* TODO: different use for CDMA */

		if (tmp--) {
			smax = parcel_r_string(&rilp);           /* TODO: different use for CDMA */

			if (smax)
				reply->max_cids = atoi(smax);
		}
	}


	if (!sstatus) {
		ofono_error("%s: no status included in %s reply",
				__FUNCTION__,
				ril_request_id_to_string(message->req));
		goto error;
	}

	reply->status = atoi(sstatus);


	if (slac)
		reply->lac = strtol(slac, NULL, 16);
	else
		reply->lac = -1;

	if (sci)
		reply->ci = strtol(sci, NULL, 16);
	else
		reply->ci = -1;

	tech_str = handle_tech(message->req, stech, &reply->tech);

	g_ril_append_print_buf(gril,
				"{%s,%s,%s,%s,%s,%s}",
				registration_status_to_string(reply->status),
				slac,
				sci,
				tech_str,
				sreason,
				smax);

	g_ril_print_response(gril, message);

	/* Free our parcel handlers */
	g_free(sstatus);
	g_free(slac);
	g_free(sci);
	g_free(stech);
	g_free(sreason);
	g_free(smax);

	return reply;

error:
	g_free(reply);
	return NULL;
}

void g_ril_reply_free_sim_io(struct reply_sim_io *reply)
{
	if (reply) {
		g_free(reply->hex_response);
		g_free(reply);
	}
}

struct reply_sim_io *g_ril_reply_parse_sim_io(GRil *gril,
						struct ril_msg *message)
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
		return NULL;
	}

	reply =	g_new0(struct reply_sim_io, 1);

	g_ril_init_parcel(message, &rilp);
	reply->sw1 = parcel_r_int32(&rilp);
	reply->sw2 = parcel_r_int32(&rilp);

	response = parcel_r_string(&rilp);
	if (response == NULL)
		goto error;

	reply->hex_response =
		decode_hex((const char *) response, strlen(response),
				(long *) &reply->hex_len, -1);
	if (reply->hex_response == NULL)
		goto error;

	g_ril_append_print_buf(gril,
				"(sw1=0x%.2X,sw2=0x%.2X,%s)",
				reply->sw1,
				reply->sw2,
				response);
	g_ril_print_response(gril, message);

	g_free(response);

	return reply;

error:
	g_free(reply);

	return NULL;
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
						struct ril_msg *message)
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

	return status;

error:
	g_ril_reply_free_sim_status(status);

	return NULL;
}
