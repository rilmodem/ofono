/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/cbs.h>
#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *none_prefix[] = { NULL };

static void at_cbm_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_cbs *cbs = user_data;
	const char *hexpdu;
	int pdulen;
	GAtResultIter iter;
	unsigned char pdu[88];
	long hexpdulen;

	dump_response("at_cbm_notify", TRUE, result);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CBM:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &pdulen))
		return;

	hexpdu = g_at_result_pdu(result);

	if (!hexpdu) {
		ofono_error("Got a CBM, but no PDU.  Are we in text mode?");
		return;
	}

	ofono_debug("Got new Cell Broadcast via CBM: %s, %d", hexpdu, pdulen);

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

	dump_response("cscb_set_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_cbs_set_topics(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *user_data)
{
	GAtChat *chat = ofono_cbs_get_data(cbs);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char *buf;
	unsigned int id;

	if (!cbd)
		goto error;

	buf = g_strdup_printf("AT+CSCB=0,\"%s\"", topics);

	id = g_at_chat_send(chat, buf, none_prefix,
				at_cscb_set_cb, cbd, g_free);

	g_free(buf);

	if (id > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void at_cbs_clear_topics(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *user_data)
{
	GAtChat *chat = ofono_cbs_get_data(cbs);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, "AT+CSCB=1,\"0-65535\"", none_prefix,
				at_cscb_set_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void at_cbs_register(gboolean ok, GAtResult *result, gpointer user)
{
	struct ofono_cbs *cbs = user;
	GAtChat *chat = ofono_cbs_get_data(cbs);

	/* This driver assumes that something else will properly setup
	 * CNMI notifications to deliver CBS broadcasts via +CBM.  We do
	 * not setup CNMI string ourselves here to avoid race conditions
	 * with the SMS driver which will also be setting the CNMI itself
	 *
	 * The default SMS driver will setup the CNMI for +CBM delivery
	 * appropriately for us
	 */
	g_at_chat_register(chat, "+CBM:", at_cbm_notify, TRUE, cbs, NULL);

	ofono_cbs_register(cbs);
}

static int at_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;

	ofono_cbs_set_data(cbs, chat);

	/* Start with CBS not accepting any channels.  The core will
	 * power on / set preferred channels when it is ready
	 */
	g_at_chat_send(chat, "AT+CSCB=1,\"0-65535\"", none_prefix,
				at_cbs_register, cbs, NULL);

	return 0;
}

static void at_cbs_remove(struct ofono_cbs *cbs)
{
}

static struct ofono_cbs_driver driver = {
	.name = "atmodem",
	.probe = at_cbs_probe,
	.remove = at_cbs_remove,
	.set_topics = at_cbs_set_topics,
	.clear_topics = at_cbs_clear_topics,
};

void at_cbs_init()
{
	ofono_cbs_driver_register(&driver);
}

void at_cbs_exit()
{
	ofono_cbs_driver_unregister(&driver);
}
