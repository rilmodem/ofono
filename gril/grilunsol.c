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
	const struct ril_data_call *ca = a;
	const struct ril_data_call *cb = b;

	if (ca->cid < cb->cid)
		return -1;

	if (ca->cid > cb->cid)
		return 1;

	return 0;
}

static void free_data_call(gpointer data, gpointer user_data)
{
	struct ril_data_call *call = data;

	if (call) {
		g_free(call->ifname);
		g_free(call->ip_addr);
		g_free(call->dns_addrs);
		g_free(call->gateways);
		g_free(call);
	}
}

void g_ril_unsol_free_data_call_list(struct ril_data_call_list *call_list)
{
	if (call_list) {
		g_slist_foreach(call_list->calls, (GFunc) free_data_call, NULL);
		g_slist_free(call_list->calls);
		g_free(call_list);
	}
}

static gboolean handle_settings(struct ril_data_call *call, char *type,
				char *ifname, char *raw_ip_addrs,
				char *raw_dns, char *raw_gws)
{
	gboolean result = FALSE;
	int protocol;
	char **dns_addrs = NULL, **gateways = NULL;
	char **ip_addrs = NULL, **split_ip_addr = NULL;

	protocol = ril_protocol_string_to_ofono_protocol(type);
	if (protocol < 0) {
		ofono_error("%s: invalid type(protocol) specified: %s",
				__func__, type);
		goto done;
	}

	if (ifname == NULL || strlen(ifname) == 0) {
		ofono_error("%s: no interface specified: %s",
				__func__, ifname);
		goto done;
	}

	/* Split DNS addresses */
	if (raw_dns)
		dns_addrs = g_strsplit(raw_dns, " ", 3);

	if (dns_addrs == NULL || g_strv_length(dns_addrs) == 0) {
		ofono_error("%s: no DNS: %s", __func__, raw_dns);
		goto done;
	}

	/*
	 * RILD can return multiple addresses; oFono only supports
	 * setting a single IPv4 gateway.
	 */
	if (raw_gws)
		gateways = g_strsplit(raw_gws, " ", 3);

	if (gateways == NULL || g_strv_length(gateways) == 0) {
		ofono_error("%s: no gateways: %s", __func__, raw_gws);
		goto done;
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
		ip_addrs = g_strsplit(raw_ip_addrs, " ", 3);

	if (ip_addrs == NULL || g_strv_length(ip_addrs) == 0) {
		ofono_error("%s: no IP address: %s", __func__, raw_ip_addrs);
		goto done;
	}

	DBG("num ip addrs is: %d", g_strv_length(ip_addrs));

	if (g_strv_length(ip_addrs) > 1)
		ofono_warn("%s: more than one IP addr returned: %s", __func__,
				raw_ip_addrs);
	/*
	 * Note - the address may optionally include a prefix size
	 * ( Eg. "/30" ).  As this confuses NetworkManager, we
	 * explicitly strip any prefix after calculating the netmask.
	 */
	split_ip_addr = g_strsplit(ip_addrs[0], "/", 2);

	if (split_ip_addr == NULL || g_strv_length(split_ip_addr) == 0) {
		ofono_error("%s: invalid IP address field returned: %s",
				__func__, ip_addrs[0]);
		goto done;
	}

	call->protocol = protocol;
	call->ifname = g_strdup(ifname);
	call->ip_addr = g_strdup(split_ip_addr[0]);
	call->dns_addrs = g_strdupv(dns_addrs);
	call->gateways = g_strdupv(gateways);

	result = TRUE;

done:
	if (dns_addrs)
		g_strfreev(dns_addrs);

	if (gateways)
		g_strfreev(gateways);

	if (ip_addrs)
		g_strfreev(ip_addrs);

	if (split_ip_addr)
		g_strfreev(split_ip_addr);


	return result;
}

/*
 * This function handles RIL_UNSOL_DATA_CALL_LIST_CHANGED messages,
 * as well as RIL_REQUEST_DATA_CALL_LIST/SETUP_DATA_CALL replies, as
 * all have the same payload.
 */
struct ril_data_call_list *g_ril_unsol_parse_data_call_list(GRil *gril,
						const struct ril_msg *message)
{
	struct ril_data_call *call;
	struct parcel rilp;
	struct ril_data_call_list *reply = NULL;
	unsigned int active, cid, i, num_calls, retry, status;
	char *type = NULL, *ifname = NULL, *raw_addrs = NULL;
	char *raw_dns = NULL, *raw_gws = NULL;

	DBG("");

	/* Can happen for RIL_REQUEST_DATA_CALL_LIST replies */
	if (message->buf_len < MIN_DATA_CALL_LIST_SIZE) {
		if (message->req == RIL_REQUEST_SETUP_DATA_CALL) {
			ofono_error("%s: message too small: %d",
					__func__,
					(int) message->buf_len);
			goto error;
		} else {
			g_ril_append_print_buf(gril, "{");
			goto done;
		}
	}

	reply = g_try_new0(struct ril_data_call_list, 1);
	if (reply == NULL) {
		ofono_error("%s: out of memory", __func__);
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
	num_calls = parcel_r_int32(&rilp);

	g_ril_append_print_buf(gril,
				"{version=%d,num=%d",
				reply->version,
				num_calls);

	for (i = 0; i < num_calls; i++) {
		status = parcel_r_int32(&rilp);
		retry = parcel_r_int32(&rilp);          /* ignore */
		cid = parcel_r_int32(&rilp);
		active = parcel_r_int32(&rilp);
		type = parcel_r_string(&rilp);
		ifname = parcel_r_string(&rilp);
		raw_addrs = parcel_r_string(&rilp);
		raw_dns = parcel_r_string(&rilp);
		raw_gws = parcel_r_string(&rilp);

		/* malformed check */
		if (rilp.malformed) {
			ofono_error("%s: malformed parcel received", __func__);
			goto error;
		}

		g_ril_append_print_buf(gril,
					"%s [status=%d,retry=%d,cid=%d,"
					"active=%d,type=%s,ifname=%s,"
					"address=%s,dns=%s,gateways=%s]",
					print_buf,
					status,
					retry,
					cid,
					active,
					type,
					ifname,
					raw_addrs,
					raw_dns,
					raw_gws);

		call = g_try_new0(struct ril_data_call, 1);
		if (call == NULL) {
			ofono_error("%s: out of memory", __func__);
			goto error;
		}

		call->status = status;
		call->cid = cid;
		call->active = active;

		if (message->req == RIL_REQUEST_SETUP_DATA_CALL &&
			status == PDP_FAIL_NONE &&
			handle_settings(call, type, ifname, raw_addrs,
					raw_dns, raw_gws) == FALSE)
			goto error;

		g_free(type);
		g_free(ifname);
		g_free(raw_addrs);
		g_free(raw_dns);
		g_free(raw_gws);

		reply->calls =
			g_slist_insert_sorted(reply->calls, call,
						data_call_compare);
	}

done:
	g_ril_append_print_buf(gril, "%s}", print_buf);

	if (message->unsolicited)
		g_ril_print_unsol(gril, message);
	else
		g_ril_print_response(gril, message);

	return reply;

error:
	g_free(type);
	g_free(ifname);
	g_free(raw_addrs);
	g_free(raw_dns);
	g_free(raw_gws);
	g_ril_unsol_free_data_call_list(reply);

	return NULL;
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

inline static int is_valid_strength(int signal)
{
	if (signal != 99 && signal != -1)
		return 1;
	else
		return 0;
}

int g_ril_unsol_parse_signal_strength(GRil *gril, const struct ril_msg *message,
					int ril_tech)
{
	struct parcel rilp;
	int gw_signal, cdma_dbm, evdo_dbm, lte_signal, signal;

	g_ril_init_parcel(message, &rilp);

	/* RIL_SignalStrength_v5 */
	/* GW_SignalStrength */
	gw_signal = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* bitErrorRate */

	/*
	 * CDMA/EVDO values are not processed as CDMA is not supported
	 */

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

	g_ril_append_print_buf(gril, "{gw: %d, cdma: %d, evdo: %d, lte: %d}",
				gw_signal, cdma_dbm, evdo_dbm, lte_signal);

	if (message->unsolicited)
		g_ril_print_unsol(gril, message);
	else
		g_ril_print_response(gril, message);

	/* Return the first valid one */
	if (is_valid_strength(gw_signal) && is_valid_strength(lte_signal))
		if (ril_tech == RADIO_TECH_LTE)
			signal = lte_signal;
		else
			signal = gw_signal;
	else if (is_valid_strength(gw_signal))
		signal = gw_signal;
	else if (is_valid_strength(lte_signal))
		signal = lte_signal;
	else
		return -1;

	return (signal * 100) / 31;
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
