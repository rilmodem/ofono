/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/cbs.h>
#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"
#include "vendor.h"

static const char *none_prefix[] = { NULL };
static const char *cscb_prefix[] = { "+CSCB:", NULL };

struct cbs_data {
	GAtChat *chat;
	gboolean cscb_mode_1;
	unsigned int vendor;
};

static void at_cbm_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_cbs *cbs = user_data;
	const char *hexpdu;
	int pdulen;
	GAtResultIter iter;
	unsigned char pdu[88];
	long hexpdulen;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CBM:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &pdulen))
		return;

	if (pdulen != 88) {
		ofono_error("Got a CBM message with invalid PDU size!");
		return;
	}

	hexpdu = g_at_result_pdu(result);
	if (hexpdu == NULL) {
		ofono_error("Got a CBM, but no PDU.  Are we in text mode?");
		return;
	}

	DBG("Got new Cell Broadcast via CBM: %s, %d", hexpdu, pdulen);

	if (decode_hex_own_buf(hexpdu, -1, &hexpdulen, 0, pdu) == NULL) {
		ofono_error("Unable to hex-decode the PDU");
		return;
	}

	if (hexpdulen != pdulen) {
		ofono_error("hexpdu length not equal to reported pdu length");
		return;
	}

	ofono_cbs_notify(cbs, pdu, pdulen);
}

static void at_cscb_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_cbs_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_cbs_set_topics(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *user_data)
{
	struct cbs_data *data = ofono_cbs_get_data(cbs);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char *buf;
	unsigned int id;

	DBG("");

	/* For the Qualcomm based devices it is required to clear
	 * the list of topics first.  Otherwise setting the new
	 * topic ranges will fail.
	 *
	 * In addition only AT+CSCB=1 seems to work.  Providing
	 * a topic range for clearing makes AT+CSBC=0,... fail.
	 */
	switch (data->vendor) {
	case OFONO_VENDOR_GOBI:
	case OFONO_VENDOR_QUALCOMM_MSM:
		g_at_chat_send(data->chat, "AT+CSCB=1", none_prefix,
				NULL, NULL, NULL);
		break;
	default:
		break;
	}

	buf = g_strdup_printf("AT+CSCB=0,\"%s\"", topics);

	id = g_at_chat_send(data->chat, buf, none_prefix,
				at_cscb_set_cb, cbd, g_free);

	g_free(buf);

	if (id > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void at_cbs_clear_topics(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *user_data)
{
	struct cbs_data *data = ofono_cbs_get_data(cbs);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char buf[256];

	DBG("");

	if (data->cscb_mode_1)
		snprintf(buf, sizeof(buf), "AT+CSCB=1,\"0-65535\"");
	else
		snprintf(buf, sizeof(buf), "AT+CSCB=0,\"\"");

	if (g_at_chat_send(data->chat, buf, none_prefix,
				at_cscb_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void at_cbs_register(gboolean ok, GAtResult *result, gpointer user)
{
	struct ofono_cbs *cbs = user;
	struct cbs_data *data = ofono_cbs_get_data(cbs);

	/* This driver assumes that something else will properly setup
	 * CNMI notifications to deliver CBS broadcasts via +CBM.  We do
	 * not setup CNMI string ourselves here to avoid race conditions
	 * with the SMS driver which will also be setting the CNMI itself
	 *
	 * The default SMS driver will setup the CNMI for +CBM delivery
	 * appropriately for us
	 */
	g_at_chat_register(data->chat, "+CBM:", at_cbm_notify, TRUE, cbs, NULL);

	ofono_cbs_register(cbs);
}

static void at_cscb_support_cb(gboolean ok, GAtResult *result, gpointer user)
{
	struct ofono_cbs *cbs = user;
	struct cbs_data *data = ofono_cbs_get_data(cbs);
	gint range[2];
	GAtResultIter iter;
	char buf[256];

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCB:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto error;

	while (g_at_result_iter_next_range(&iter, &range[0], &range[1]))
		if (1 >= range[0] && 1 <= range[1])
			data->cscb_mode_1 = TRUE;

	g_at_result_iter_close_list(&iter);

	/* Assume that if CSCB mode 1 is supported, then we need to use
	 * it to remove topics, otherwise we need to set the entire list
	 * of new topics using CSCB mode 0.
	 */
	if (data->cscb_mode_1)
		snprintf(buf, sizeof(buf), "AT+CSCB=1,\"0-65535\"");
	else
		snprintf(buf, sizeof(buf), "AT+CSCB=0,\"\"");

	if (g_at_chat_send(data->chat, buf, none_prefix,
				at_cbs_register, cbs, NULL) > 0)
		return;

error:
	ofono_error("CSCB not supported");
	ofono_cbs_remove(cbs);
}

static int at_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
				void *user)
{
	GAtChat *chat = user;
	struct cbs_data *data;

	data = g_new0(struct cbs_data, 1);
	data->chat = g_at_chat_clone(chat);
	data->vendor = vendor;

	ofono_cbs_set_data(cbs, data);

	g_at_chat_send(data->chat, "AT+CSCB=?", cscb_prefix,
			at_cscb_support_cb, cbs, NULL);

	return 0;
}

static void at_cbs_remove(struct ofono_cbs *cbs)
{
	struct cbs_data *data = ofono_cbs_get_data(cbs);

	ofono_cbs_set_data(cbs, NULL);

	g_at_chat_unref(data->chat);
	g_free(data);
}

static struct ofono_cbs_driver driver = {
	.name = "atmodem",
	.probe = at_cbs_probe,
	.remove = at_cbs_remove,
	.set_topics = at_cbs_set_topics,
	.clear_topics = at_cbs_clear_topics,
};

void at_cbs_init(void)
{
	ofono_cbs_driver_register(&driver);
}

void at_cbs_exit(void)
{
	ofono_cbs_driver_unregister(&driver);
}
