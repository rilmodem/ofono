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
