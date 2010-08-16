/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010 ST-Ericsson AB.
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include <ofono/gprs.h>

#include <linux/types.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include "gatchat.h"
#include "gatresult.h"
#include "stemodem.h"
#include "caif_socket.h"
#include "if_caif.h"

#define MAX_CAIF_DEVICES 7
#define MAX_DNS 2
#define MAX_ELEM 20

#define AUTH_BUF_LENGTH (OFONO_GPRS_MAX_USERNAME_LENGTH + \
			OFONO_GPRS_MAX_PASSWORD_LENGTH + 128)

static const char *cgact_prefix[] = { "+CGACT:", NULL };
static const char *none_prefix[] = { NULL };

static GSList *g_caif_devices;

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
};

struct conn_info {
	unsigned int cid;
	unsigned int device;
	unsigned int channel_id;
	char interface[10];
};

struct eppsd_response {
	char *current;
	char ip_address[MAX_ELEM];
	char subnet_mask[MAX_ELEM];
	char mtu[MAX_ELEM];
	char default_gateway[MAX_ELEM];
	char dns_server1[MAX_ELEM];
	char dns_server2[MAX_ELEM];
	char p_cscf_server[MAX_ELEM];
};

static void start_element_handler(GMarkupParseContext *context,
		const gchar *element_name, const gchar **attribute_names,
		const gchar **attribute_values, gpointer user_data,
		GError **error)
{
	struct eppsd_response *rsp = user_data;
	rsp->current = NULL;

	if (!strcmp(element_name, "ip_address"))
		rsp->current = rsp->ip_address;
	else if (!strcmp(element_name, "subnet_mask"))
		rsp->current = rsp->subnet_mask;
	else if (!strcmp(element_name, "mtu"))
		rsp->current = rsp->mtu;
	else if (!strcmp(element_name, "default_gateway"))
		rsp->current = rsp->default_gateway;
	else if (!strcmp(element_name, "dns_server") &&
		rsp->dns_server1[0] == '\0')
		rsp->current = rsp->dns_server1;
	else if (!strcmp(element_name, "dns_server"))
		rsp->current = rsp->dns_server2;
	else if (!strcmp(element_name, "p_cscf_server"))
		rsp->current = rsp->p_cscf_server;
}

static void end_element_handler(GMarkupParseContext *context,
				const gchar *element_name, gpointer user_data,
				GError **error)
{
	struct eppsd_response *rsp = user_data;
	rsp->current = NULL;
}

static void text_handler(GMarkupParseContext *context,
				const gchar *text, gsize text_len,
				gpointer user_data, GError **error)
{
	struct eppsd_response *rsp = user_data;

	if (rsp->current) {
		strncpy(rsp->current, text, MAX_ELEM);
		rsp->current[MAX_ELEM] = 0;
	}
}

static void error_handler(GMarkupParseContext *context,
				GError *error, gpointer user_data)
{
	DBG("Error parsing xml response from eppsd: %s\n",
		error->message);
}

static GMarkupParser parser = {
	start_element_handler,
	end_element_handler,
	text_handler,
	NULL,
	error_handler
};

static gint conn_compare_by_cid(gconstpointer a, gconstpointer b)
{
	const struct conn_info *conn = a;
	unsigned int used = GPOINTER_TO_UINT(b);

	if (used != conn->cid)
		return 1;

	return 0;
}

static struct conn_info *conn_info_create(unsigned int device,
						unsigned int channel_id)
{
	struct conn_info *connection = g_try_new0(struct conn_info, 1);

	if (!connection)
		return NULL;

	connection->cid = 0;
	connection->device = device;
	connection->channel_id = channel_id;

	return connection;
}

/*
 * Creates a new IP interface for CAIF.
 */
static gboolean caif_if_create(const char *interface, unsigned int connid)
{
	return FALSE;
}

/*
 * Removes IP interface for CAIF.
 */
static gboolean caif_if_remove(const char *interface, unsigned int connid)
{
	return FALSE;
}

static void ste_eppsd_down_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;
	struct conn_info *conn;
	GSList *l;

	if (!ok)
		goto error;

	l = g_slist_find_custom(g_caif_devices,
				GUINT_TO_POINTER(gcd->active_context),
				conn_compare_by_cid);

	if (!l) {
		DBG("Did not find data (used caif device) for"
					"connection with cid; %d",
					gcd->active_context);
		goto error;
	}

	conn = l->data;

	if (!caif_if_remove(conn->interface, conn->channel_id)) {
		DBG("Failed to remove caif interface %s.",
				conn->interface);
	}

	conn->cid = 0;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ste_eppsd_up_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_up_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct conn_info *conn = NULL;
	GAtResultIter iter;
	GSList *l;
	int i;
	gsize length;
	char *res_string;
	const char *dns[MAX_DNS + 1];
	struct eppsd_response rsp;
	GMarkupParseContext *context = NULL;

	l = g_slist_find_custom(g_caif_devices,
				GUINT_TO_POINTER(gcd->active_context),
				conn_compare_by_cid);

	if (!l) {
		DBG("Did not find data (device and channel id)"
					"for connection with cid; %d",
					gcd->active_context);
		goto error;
	}

	conn = l->data;

	if (!ok)
		goto error;

	rsp.current = NULL;
	context = g_markup_parse_context_new(&parser, 0, &rsp, NULL);
	memset(&rsp, 0, sizeof(rsp));

	g_at_result_iter_init(&iter, result);

	for (i = 0; i < g_at_result_num_response_lines(result); i++) {
		g_at_result_iter_next(&iter, NULL);
		res_string = strdup(g_at_result_iter_raw_line(&iter));
		length = strlen(res_string);

		if (!g_markup_parse_context_parse(context, res_string,
							length, NULL))
			goto error;
	}

	if (!g_markup_parse_context_end_parse(context, NULL))
		goto error;

	g_markup_parse_context_free(context);

	dns[0] = rsp.dns_server1;
	dns[1] = rsp.dns_server2;
	dns[2] = NULL;

	sprintf(conn->interface, "caif%u", conn->device);

	if (!caif_if_create(conn->interface, conn->channel_id)) {
		ofono_error("Failed to create caif interface %s.",
				conn->interface);
		CALLBACK_WITH_SUCCESS(cb, NULL, FALSE, rsp.ip_address,
				rsp.subnet_mask, rsp.default_gateway,
				dns, cbd->data);
	} else {
		CALLBACK_WITH_SUCCESS(cb, conn->interface,
				FALSE, rsp.ip_address, rsp.subnet_mask,
				rsp.default_gateway, dns, cbd->data);
	}

	return;

error:
	DBG("ste_eppsd_up_cb error");

	if (context)
		g_markup_parse_context_free(context);

	if (conn)
		conn->cid = 0;

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, cbd->data);
}

static void ste_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_up_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *ncbd = NULL;
	char buf[128];
	struct conn_info *conn;
	GSList *l;

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, NULL, 0, NULL, NULL, NULL, NULL, cbd->data);
		return;
	}

	ncbd = g_memdup(cbd, sizeof(struct cb_data));

	l = g_slist_find_custom(g_caif_devices, GUINT_TO_POINTER(0),
				conn_compare_by_cid);

	if (!l) {
		DBG("at_cgdcont_cb, no more available devices");
		goto error;
	}

	conn = l->data;
	conn->cid = gcd->active_context;
	snprintf(buf, sizeof(buf), "AT*EPPSD=1,%u,%u",
			conn->channel_id, conn->cid);

	if (g_at_chat_send(gcd->chat, buf, NULL,
				ste_eppsd_up_cb, ncbd, g_free) > 0)
		return;

error:
	g_free(ncbd);

	gcd->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL,
				NULL, NULL, cbd->data);
}

static void ste_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_up_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[AUTH_BUF_LENGTH];
	int len;

	if (!cbd)
		goto error;

	gcd->active_context = ctx->cid;
	cbd->user = gc;

	len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"", ctx->cid);

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
				ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				ste_cgdcont_cb, cbd, g_free) == 0)
		goto error;

	/*
	 * Set username and password, this should be done after CGDCONT
	 * or an error can occur.  We don't bother with error checking
	 * here
	 * */
	snprintf(buf, sizeof(buf), "AT*EIAAUW=%d,1,\"%s\",\"%s\"",
			ctx->cid, ctx->username, ctx->password);

	g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL);

	return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, NULL, NULL, NULL, NULL, data);
}

static void ste_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int id,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct conn_info *conn;
	char buf[64];
	GSList *l;

	if (!cbd)
		goto error;

	gcd->active_context = id;
	cbd->user = gc;

	l = g_slist_find_custom(g_caif_devices, GUINT_TO_POINTER(id),
				conn_compare_by_cid);

	if (!l) {
		DBG("at_gprs_deactivate_primary, did not find"
			"data (channel id) for connection with cid; %d", id);
		goto error;
	}

	conn = l->data;

	snprintf(buf, sizeof(buf), "AT*EPPSD=0,%u,%u", conn->channel_id, id);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				ste_eppsd_down_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ste_cgact_read_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	gint cid, state;
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGACT:")) {

		if (!g_at_result_iter_next_number(&iter, &cid))
			continue;

		if ((unsigned int) cid != gcd->active_context)
			continue;

		if (!g_at_result_iter_next_number(&iter, &state))
			continue;

		if (state == 1)
			continue;

		ofono_gprs_context_deactivated(gc, gcd->active_context);
		gcd->active_context = 0;

		break;
	}
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	const char *event;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_has_prefix(event, "NW REACT ") ||
			g_str_has_prefix(event, "NW DEACT ") ||
			g_str_has_prefix(event, "ME DEACT ")) {
		/* Ask what primary contexts are active now */
		g_at_chat_send(gcd->chat, "AT+CGACT?", cgact_prefix,
				ste_cgact_read_cb, gc, NULL);
	}
}

static int ste_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;
	struct conn_info *ci;
	int i;

	gcd = g_new0(struct gprs_context_data, 1);
	gcd->chat = g_at_chat_clone(chat);

	g_at_chat_register(gcd->chat, "+CGEV:", cgev_notify, FALSE, gc, NULL);

	ofono_gprs_context_set_data(gc, gcd);

	for (i = 0; i < MAX_CAIF_DEVICES; i++) {
		ci = conn_info_create(i, i+1);
		if (ci)
			g_caif_devices = g_slist_append(g_caif_devices, ci);
	}

	return 0;
}

static void ste_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	g_slist_foreach(g_caif_devices, (GFunc) g_free, NULL);
	g_slist_free(g_caif_devices);
	g_caif_devices = NULL;

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "stemodem",
	.probe			= ste_gprs_context_probe,
	.remove			= ste_gprs_context_remove,
	.activate_primary	= ste_gprs_activate_primary,
	.deactivate_primary	= ste_gprs_deactivate_primary,
};

void ste_gprs_context_init()
{
	ofono_gprs_context_driver_register(&driver);
}

void ste_gprs_context_exit()
{
	ofono_gprs_context_driver_unregister(&driver);
}
