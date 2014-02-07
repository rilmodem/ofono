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

#include "common.h"
#include "grilunsol.h"

/* Minimum size is two int32s version/number of calls */
#define MIN_DATA_CALL_LIST_SIZE 8

/*
 * Minimum NITZ is: 'yy/mm/dd,hh:mm:ss'
 * TZ '(+/-)tz,dt' are optional
 */
#define MIN_NITZ_SIZE 17

static gint data_call_compare(gconstpointer a, gconstpointer b)
{
	const struct data_call *ca = a;
	const struct data_call *cb = b;

	if (ca->cid < cb->cid)
		return -1;

	if (ca->cid > cb->cid)
		return 1;

	return 0;
}

static void free_data_call(gpointer data, gpointer user_data)
{
	struct data_call *call = data;

	if (call) {
		g_free(call->type);
		g_free(call->ifname);
		g_free(call->addresses);
		g_free(call->dnses);
		g_free(call->gateways);
		g_free(call);
	}
}

void g_ril_unsol_free_data_call_list(struct unsol_data_call_list *unsol)
{
	if (unsol) {
		g_slist_foreach(unsol->call_list, (GFunc) free_data_call, NULL);
		g_slist_free(unsol->call_list);
		g_free(unsol);
	}
}

struct unsol_data_call_list *g_ril_unsol_parse_data_call_list(GRil *gril,
					const struct ril_msg *message,
					struct ofono_error *error)
{
	struct data_call *call;
	struct parcel rilp;
	struct unsol_data_call_list *reply =
		g_new0(struct unsol_data_call_list, 1);
	unsigned int i;

	DBG("");

	OFONO_NO_ERROR(error);

	if (message->buf_len < MIN_DATA_CALL_LIST_SIZE) {
		ofono_error("%s: message too small: %d",
				__func__,
				(int) message->buf_len);
		OFONO_EINVAL(error);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	/*
	 * ril.h documents the reply to a RIL_REQUEST_DATA_CALL_LIST
	 * as being an array of  RIL_Data_Call_Response_v6 structs,
	 * however in reality, the response also includes a version
	 * to start.
	 */
	reply->version = parcel_r_int32(&rilp);
	reply->num = parcel_r_int32(&rilp);

	g_ril_append_print_buf(gril,
				"(version=%d,num=%d",
				reply->version,
				reply->num);

	for (i = 0; i < reply->num; i++) {
		call = g_new0(struct data_call, 1);

		call->status = parcel_r_int32(&rilp);
		call->retry = parcel_r_int32(&rilp);
		call->cid = parcel_r_int32(&rilp);
		call->active = parcel_r_int32(&rilp);

		call->type = parcel_r_string(&rilp);
		call->ifname = parcel_r_string(&rilp);
		call->addresses = parcel_r_string(&rilp);
		call->dnses = parcel_r_string(&rilp);
		call->gateways = parcel_r_string(&rilp);

		g_ril_append_print_buf(gril,
					"%s [status=%d,retry=%d,cid=%d,"
					"active=%d,type=%s,ifname=%s,"
					"address=%s,dns=%s,gateways=%s]",
					print_buf,
					call->status,
					call->retry,
					call->cid,
					call->active,
					call->type,
					call->ifname,
					call->addresses,
					call->dnses,
					call->gateways);

		reply->call_list =
			g_slist_insert_sorted(reply->call_list,
						call,
						data_call_compare);
	}

	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_unsol(gril, message);

error:
	return reply;
}

char *g_ril_unsol_parse_nitz(GRil *gril, const struct ril_msg *message)
{
	struct parcel rilp;
	gchar *nitz = NULL;

	DBG("");

	if (message->buf_len < MIN_NITZ_SIZE) {
		ofono_error("%s: NITZ too small: %d",
				__func__,
				(int) message->buf_len);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	nitz = parcel_r_string(&rilp);

	g_ril_append_print_buf(gril, "(%s)", nitz);
	g_ril_print_unsol(gril, message);

error:
	return nitz;
}

void g_ril_unsol_free_sms_data(struct unsol_sms_data *unsol)
{
	if (unsol != NULL) {
		g_free(unsol->data);
		g_free(unsol);
	}
}

struct unsol_sms_data *g_ril_unsol_parse_new_sms(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	char *ril_pdu;
	size_t ril_pdu_len;
	struct unsol_sms_data *sms_data;

	sms_data = g_new0(struct unsol_sms_data, 1);
	if (sms_data == NULL) {
		ofono_error("%s out of memory", __func__);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	ril_pdu = parcel_r_string(&rilp);
	if (ril_pdu == NULL) {
		ofono_error("%s Unable to parse notification", __func__);
		goto error;
	}

	ril_pdu_len = strlen(ril_pdu);

	sms_data->data = decode_hex(ril_pdu, ril_pdu_len,
					&sms_data->length, -1);
	if (sms_data->data == NULL) {
		ofono_error("%s Unable to decode notification", __func__);
		goto error_dec;
	}

	g_ril_append_print_buf(gril, "{%s}", ril_pdu);
	g_ril_print_unsol(gril, message);

	g_free(ril_pdu);

	return sms_data;

error_dec:
	g_free(ril_pdu);
error:
	g_ril_unsol_free_sms_data(sms_data);
	return NULL;
}

int g_ril_unsol_parse_radio_state_changed(GRil *gril, const struct ril_msg *message)
{
	struct parcel rilp;
	int radio_state;

	g_ril_init_parcel(message, &rilp);

	radio_state = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel received", __func__);
		radio_state = -1;
	}

	g_ril_append_print_buf(gril, "(state: %s)",
				ril_radio_state_to_string(radio_state));

	g_ril_print_unsol(gril, message);

	return radio_state;
}

int g_ril_unsol_parse_signal_strength(GRil *gril, const struct ril_msg *message)
{
	struct parcel rilp;
	int gw_signal, cdma_dbm, evdo_dbm, lte_signal;

	g_ril_init_parcel(message, &rilp);

	/* RIL_SignalStrength_v5 */
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

	/* Present only for RIL_SignalStrength_v6 or newer */
	if (parcel_data_avail(&rilp) > 0) {
		/* LTE_SignalStrength */
		lte_signal = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp); /* rsrp */
		parcel_r_int32(&rilp); /* rsrq */
		parcel_r_int32(&rilp); /* rssnr */
		parcel_r_int32(&rilp); /* cqi */
	} else {
		lte_signal = -1;
	}

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

void g_ril_unsol_free_supp_svc_notif(struct unsol_supp_svc_notif *unsol)
{
	g_free(unsol);
}

struct unsol_supp_svc_notif *g_ril_unsol_parse_supp_svc_notif(GRil *gril,
						struct ril_msg *message)
{
	struct parcel rilp;
	char *tmp_number;
	int type;
	struct unsol_supp_svc_notif *unsol =
		g_new0(struct unsol_supp_svc_notif, 1);

	g_ril_init_parcel(message, &rilp);

	unsol->notif_type = parcel_r_int32(&rilp);
	unsol->code = parcel_r_int32(&rilp);
	unsol->index = parcel_r_int32(&rilp);
	type = parcel_r_int32(&rilp);
	tmp_number = parcel_r_string(&rilp);

	if (tmp_number != NULL) {
		strncpy(unsol->number.number, tmp_number,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
		unsol->number.type = type;
		g_free(tmp_number);
	}

	g_ril_append_print_buf(gril, "{%d,%d,%d,%d,%s}",
				unsol->notif_type, unsol->code, unsol->index,
				type, tmp_number);
	g_ril_print_unsol(gril, message);

	return unsol;
}

void g_ril_unsol_free_ussd(struct unsol_ussd *unsol)
{
	if (unsol != NULL) {
		g_free(unsol->message);
		g_free(unsol);
	}
}

struct unsol_ussd *g_ril_unsol_parse_ussd(GRil *gril, struct ril_msg *message)
{
	struct parcel rilp;
	struct unsol_ussd *ussd;
	char *typestr = NULL;
	int numstr;

	ussd = g_try_malloc0(sizeof(*ussd));
	if (ussd == NULL) {
		ofono_error("%s out of memory", __func__);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	numstr = parcel_r_int32(&rilp);
	if (numstr < 1) {
		ofono_error("%s malformed parcel", __func__);
		goto error;
	}

	typestr = parcel_r_string(&rilp);
	if (typestr == NULL || *typestr == '\0') {
		ofono_error("%s wrong type", __func__);
		goto error;
	}

	ussd->type = *typestr - '0';

	g_free(typestr);

	if (numstr > 1)
		ussd->message = parcel_r_string(&rilp);

	g_ril_append_print_buf(gril, "{%d,%s}", ussd->type, ussd->message);

	g_ril_print_unsol(gril, message);

	return ussd;

error:
	g_free(typestr);
	g_free(ussd);

	return NULL;
}
