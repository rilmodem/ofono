/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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

#include <glib.h>
#include <gril.h>
#include <string.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/log.h>
#include <ofono/types.h>

#include "common.h"
#include "rilutil.h"
#include "simutil.h"
#include "util.h"
#include "ril_constants.h"

struct ril_util_sim_state_query {
	GRil *ril;
	guint cpin_poll_source;
	guint cpin_poll_count;
	guint interval;
	guint num_times;
	ril_util_sim_inserted_cb_t cb;
	void *userdata;
	GDestroyNotify destroy;
};

static gboolean cpin_check(gpointer userdata);

void decode_ril_error(struct ofono_error *error, const char *final)
{
	if (!strcmp(final, "OK")) {
		error->type = OFONO_ERROR_TYPE_NO_ERROR;
		error->error = 0;
	} else {
		error->type = OFONO_ERROR_TYPE_FAILURE;
		error->error = 0;
	}
}

gint ril_util_call_compare_by_status(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	int status = GPOINTER_TO_INT(b);

	if (status != call->status)
		return 1;

	return 0;
}

gint ril_util_call_compare_by_phone_number(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	const struct ofono_phone_number *pb = b;

	return memcmp(&call->phone_number, pb,
				sizeof(struct ofono_phone_number));
}

gint ril_util_call_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	if (id < call->id)
		return -1;

	if (id > call->id)
		return 1;

	return 0;
}

gint ril_util_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

static gboolean cpin_check(gpointer userdata)
{
	struct ril_util_sim_state_query *req = userdata;

	req->cpin_poll_source = 0;

	return FALSE;
}

gchar *ril_util_get_netmask(const gchar *address)
{
	char *result;

	if (g_str_has_suffix(address, "/30")) {
		result = PREFIX_30_NETMASK;
	} else if (g_str_has_suffix(address, "/29")) {
		result = PREFIX_29_NETMASK;
	} else if (g_str_has_suffix(address, "/28")) {
		result = PREFIX_28_NETMASK;
	} else if (g_str_has_suffix(address, "/27")) {
		result = PREFIX_27_NETMASK;
	} else if (g_str_has_suffix(address, "/26")) {
		result = PREFIX_26_NETMASK;
	} else if (g_str_has_suffix(address, "/25")) {
		result = PREFIX_25_NETMASK;
	} else if (g_str_has_suffix(address, "/24")) {
		result = PREFIX_24_NETMASK;
	} else {
		/*
		 * This handles the case where the
		 * Samsung RILD returns an address without
		 * a prefix, however it explicitly sets a
		 * /24 netmask ( which isn't returned as
		 * an attribute of the DATA_CALL.
		 *
		 * TODO/OEM: this might need to be quirked
		 * for specific devices.
		 */
		result = PREFIX_24_NETMASK;
	}

	DBG("address: %s netmask: %s", address, result);

	return result;
}

/* TODO: this function can go away, once all the code has been
 * re-factored to use grilreply.c */
void ril_util_init_parcel(struct ril_msg *message, struct parcel *rilp)
{
	/* Set up Parcel struct for proper parsing */
	rilp->data = message->buf;
	rilp->size = message->buf_len;
	rilp->capacity = message->buf_len;
	rilp->offset = 0;
}

struct ril_util_sim_state_query *ril_util_sim_state_query_new(GRil *ril,
						guint interval, guint num_times,
						ril_util_sim_inserted_cb_t cb,
						void *userdata,
						GDestroyNotify destroy)
{
	struct ril_util_sim_state_query *req;

	req = g_new0(struct ril_util_sim_state_query, 1);

	req->ril = ril;
	req->interval = interval;
	req->num_times = num_times;
	req->cb = cb;
	req->userdata = userdata;
	req->destroy = destroy;

	cpin_check(req);

	return req;
}

void ril_util_sim_state_query_free(struct ril_util_sim_state_query *req)
{
	if (req == NULL)
		return;

	if (req->cpin_poll_source > 0)
		g_source_remove(req->cpin_poll_source);

	if (req->destroy)
		req->destroy(req->userdata);

	g_free(req);
}

GSList *ril_util_parse_clcc(GRil *gril, struct ril_msg *message)
{
	struct ofono_call *call;
	struct parcel rilp;
	GSList *l = NULL;
	int num, i;
	gchar *number, *name;

	ril_util_init_parcel(message, &rilp);

	g_ril_append_print_buf(gril, "{");

	/* Number of RIL_Call structs */
	num = parcel_r_int32(&rilp);
	for (i = 0; i < num; i++) {
		call = g_try_new(struct ofono_call, 1);
		if (call == NULL)
			break;

		ofono_call_init(call);
		call->status = parcel_r_int32(&rilp);
		call->id = parcel_r_int32(&rilp);
		call->phone_number.type = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp); /* isMpty */
		parcel_r_int32(&rilp); /* isMT */
		parcel_r_int32(&rilp); /* als */
		call->type = parcel_r_int32(&rilp); /* isVoice */
		parcel_r_int32(&rilp); /* isVoicePrivacy */
		number = parcel_r_string(&rilp);
		if (number) {
			strncpy(call->phone_number.number, number,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
			g_free(number);
		}
		parcel_r_int32(&rilp); /* numberPresentation */
		name = parcel_r_string(&rilp);
		if (name) {
			strncpy(call->name, name,
				OFONO_MAX_CALLER_NAME_LENGTH);
			g_free(name);
		}
		parcel_r_int32(&rilp); /* namePresentation */
		parcel_r_int32(&rilp); /* uusInfo */

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		/* TODO: figure out how to line-wrap properly
		 * without introducing spaces in string.
		 */
		g_ril_append_print_buf(gril,
					"%s [id=%d,status=%d,type=%d,number=%s,name=%s]",
					print_buf,
					call->id, call->status, call->type,
					call->phone_number.number, call->name);

		l = g_slist_insert_sorted(l, call, ril_util_call_compare);
	}

	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	return l;
}

char *ril_util_parse_sim_io_rsp(GRil *gril,
				struct ril_msg *message,
				int *sw1, int *sw2,
				int *hex_len)
{
	struct parcel rilp;
	char *response = NULL;
	char *hex_response = NULL;

	/* Minimum length of SIM_IO_Response is 12:
	 * sw1 (int32)
	 * sw2 (int32)
	 * simResponse (string)
	 */
	if (message->buf_len < 12) {
		ofono_error("Invalid SIM IO reply: size too small (< 12): %d ",
			    (int) message->buf_len);
		return FALSE;
	}

	ril_util_init_parcel(message, &rilp);
	*sw1 = parcel_r_int32(&rilp);
	*sw2 = parcel_r_int32(&rilp);

	response = parcel_r_string(&rilp);
	if (response) {
		hex_response = (char *) decode_hex((const char *) response,
							strlen(response),
							(long *) hex_len, -1);
	}

	g_ril_append_print_buf(gril,
				"(sw1=0x%.2X,sw2=0x%.2X,%s)",
				*sw1,
				*sw2,
				response);
	g_ril_print_response(gril, message);

	g_free(response);
	return hex_response;
}

gboolean ril_util_parse_sim_status(GRil *gril,
					struct ril_msg *message,
					struct sim_status *status,
					struct sim_app **apps)
{
	struct parcel rilp;
	gboolean result = FALSE;
	unsigned int i;

	g_ril_append_print_buf(gril, "[%04d]< %s",
			message->serial_no,
			ril_request_id_to_string(message->req));

	ril_util_init_parcel(message, &rilp);

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
		return FALSE;
	}

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
				"(card_state=%d,universal_pin_state=%d,gsm_umts_index=%d,cdma_index=%d,ims_index=%d, ",
				status->card_state,
				status->pin_state,
				status->gsm_umts_index,
				status->cdma_index,
				status->ims_index);

	if (status->card_state == RIL_CARDSTATE_PRESENT)
		result = TRUE;
	else
		goto done;

	if (status->num_apps > MAX_UICC_APPS) {
		ofono_error("SIM error; too many apps: %d", status->num_apps);
		status->num_apps = MAX_UICC_APPS;
	}

	for (i = 0; i < status->num_apps; i++) {
		DBG("processing app[%d]", i);
		apps[i] = g_try_new0(struct sim_app, 1);
		if (apps[i] == NULL) {
			ofono_error("Can't allocate app_data");
			goto error;
		}

		apps[i]->app_type = parcel_r_int32(&rilp);
		apps[i]->app_state = parcel_r_int32(&rilp);
		apps[i]->perso_substate = parcel_r_int32(&rilp);

		/* TODO: we need a way to instruct parcel to skip
		 * a string, without allocating memory...
		 */
		apps[i]->aid_str = parcel_r_string(&rilp); /* application ID (AID) */
		apps[i]->app_str = parcel_r_string(&rilp); /* application label */

		apps[i]->pin_replaced = parcel_r_int32(&rilp);
		apps[i]->pin1_state = parcel_r_int32(&rilp);
		apps[i]->pin2_state = parcel_r_int32(&rilp);

		g_ril_append_print_buf(gril,
					"%s[app_type=%d,app_state=%d,perso_substate=%d,aid_ptr=%s,app_label_ptr=%s,pin1_replaced=%d,pin1=%d,pin2=%d],",
					print_buf,
					apps[i]->app_type,
					apps[i]->app_state,
					apps[i]->perso_substate,
					apps[i]->aid_str,
					apps[i]->app_str,
					apps[i]->pin_replaced,
					apps[i]->pin1_state,
					apps[i]->pin2_state);
	}

done:
	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	return result;

error:
	if (apps)
		ril_util_free_sim_apps(apps, status->num_apps);

	return FALSE;
}

gboolean ril_util_parse_reg(GRil *gril,
				struct ril_msg *message, int *status,
				int *lac, int *ci, int *tech, int *max_calls)
{
	struct parcel rilp;
	int tmp;
	gchar *sstatus = NULL, *slac = NULL, *sci = NULL;
	gchar *stech = NULL, *sreason = NULL, *smax = NULL;

	ril_util_init_parcel(message, &rilp);

	/* FIXME: need minimum message size check FIRST!!! */

	/* Size of response string array
	 *
	 * Should be:
	 *   >= 4 for VOICE_REG reply
	 *   >= 5 for DATA_REG reply
	 */
	if ((tmp = parcel_r_int32(&rilp)) < 4) {
		DBG("Size of response array is too small: %d", tmp);
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

			if (smax && max_calls)
				*max_calls = atoi(smax);
		}
	}

	if (status) {
		if (!sstatus) {
			DBG("No sstatus value returned!");
			goto error;
		}

		*status = atoi(sstatus);
	}

	if (lac) {
		if (slac)
			*lac = strtol(slac, NULL, 16);
		else
			*lac = -1;
	}

	if (ci) {
		if (sci)
			*ci = strtol(sci, NULL, 16);
		else
			*ci = -1;
	}


	if (tech) {
		if (stech) {
			switch(atoi(stech)) {
			case RADIO_TECH_UNKNOWN:
				*tech = -1;
				break;
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
			case RADIO_TECH_HSPA:
				*tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
				break;
			default:
				*tech = -1;
			}
		} else
			*tech = -1;
	}

	g_ril_append_print_buf(gril,
				"{%s,%s,%s,%s,%s,%s}",
				registration_status_to_string(*status),
				slac,
				sci,
				registration_tech_to_string(*tech),
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

	return TRUE;

error:
	return FALSE;
}

gint ril_util_parse_sms_response(GRil *gril, struct ril_msg *message)
{
	struct parcel rilp;
	int error, mr;
	char *ack_pdu;

	/* Set up Parcel struct for proper parsing */
	ril_util_init_parcel(message, &rilp);

	/* TP-Message-Reference for GSM/
	 * BearerData MessageId for CDMA
	 */
	mr = parcel_r_int32(&rilp);
	ack_pdu = parcel_r_string(&rilp);
	error = parcel_r_int32(&rilp);


	g_ril_append_print_buf(gril, "{%d,%s,%d}",
				mr, ack_pdu, error);
	g_ril_print_response(gril, message);

	g_free(ack_pdu);

	return mr;
}

gint ril_util_get_signal(GRil *gril, struct ril_msg *message)
{
	struct parcel rilp;
	int gw_signal, cdma_dbm, evdo_dbm, lte_signal;

	/* Set up Parcel struct for proper parsing */
	ril_util_init_parcel(message, &rilp);

	/* RIL_SignalStrength_v6 */
	/* GW_SignalStrength */
	gw_signal = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* bitErrorRate */

	/* CDMA_SignalStrength */
	cdma_dbm = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* ecio */

	/* EVDO_SignalStrength */
	evdo_dbm = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* ecio */
	parcel_r_int32(&rilp); /* signalNoiseRatio */

	/* LTE_SignalStrength */
	lte_signal = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* rsrp */
	parcel_r_int32(&rilp); /* rsrq */
	parcel_r_int32(&rilp); /* rssnr */
	parcel_r_int32(&rilp); /* cqi */

	g_ril_append_print_buf(gril, "(gw: %d, cdma: %d, evdo: %d, lte: %d)",
				gw_signal, cdma_dbm, evdo_dbm, lte_signal);

	if (message->unsolicited)
		g_ril_print_unsol(gril, message);
	else
		g_ril_print_response(gril, message);

	/* Return the first valid one */
	if ((gw_signal != 99) && (gw_signal != -1))
		return (gw_signal * 100) / 31;
	if ((lte_signal != 99) && (lte_signal != -1))
		return (lte_signal * 100) / 31;

	/* In case of dbm, return the value directly */
	if (cdma_dbm != -1) {
		if (cdma_dbm > 100)
			cdma_dbm = 100;
		return cdma_dbm;
	}
	if (evdo_dbm != -1) {
		if (evdo_dbm > 100)
			evdo_dbm = 100;
		return evdo_dbm;
	}

	return -1;
}

void ril_util_free_sim_apps(struct sim_app **apps, guint num_apps) {
	guint i;

	for (i = 0; i < num_apps; i++) {
		g_free(apps[i]->aid_str);
		g_free(apps[i]->app_str);
		g_free(apps[i]);
	}
}
