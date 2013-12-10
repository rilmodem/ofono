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
