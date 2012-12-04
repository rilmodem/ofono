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
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/stk.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"
#include "stk.h"
#include "vendor.h"

struct stk_data {
	GAtChat *chat;
	unsigned int vendor;
};

static const char *none_prefix[] = { NULL };
static const char *cusate_prefix[] = { "+CUSATER:", NULL };

static void at_cusate_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_envelope_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	const guint8 *response = NULL;
	gint len = 0;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok == FALSE)
		goto done;

	/*
	 * According to 27.007, Section 12.2.5 the envelope response is
	 * returned in +CUSATER intermediate response
	 */
	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CUSATER:"))
		goto done;

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len))
		goto done;

done:
	cb(&error, response, len, cbd->data);
}

static void at_stk_envelope(struct ofono_stk *stk, int length,
				const unsigned char *command,
				ofono_stk_envelope_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = alloca(64 + length * 2);
	int len;

	len = sprintf(buf, "AT+CUSATE=");

	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *command++);

	if (g_at_chat_send(sd->chat, buf, cusate_prefix,
				at_cusate_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void at_cusatt_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void at_stk_terminal_response(struct ofono_stk *stk, int length,
					const unsigned char *value,
					ofono_stk_generic_cb_t cb,
					void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = alloca(64 + length * 2);
	int len;

	len = sprintf(buf, "AT+CUSATT=");

	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *value++);

	if (g_at_chat_send(sd->chat, buf, none_prefix,
				at_cusatt_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void phonesim_cusatp_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GAtResultIter iter;
	const guint8 *response;
	gint len;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CUSATP:"))
		return;

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len))
		return;

	ofono_stk_proactive_command_notify(stk, len, response);
}

static void phonesim_hcmd_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GAtResultIter iter;
	const guint8 *response;
	gint len;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "*HCMD:"))
		return;

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len))
		return;

	ofono_stk_proactive_command_handled_notify(stk, len, response);
}

static void phonesim_cusatend_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	ofono_stk_proactive_session_end_notify(stk);
}

static gboolean at_stk_register(gpointer user)
{
	struct ofono_stk *stk = user;
	struct stk_data *sd = ofono_stk_get_data(stk);

	g_at_chat_register(sd->chat, "+CUSATP:", phonesim_cusatp_notify,
						FALSE, stk, NULL);

	g_at_chat_register(sd->chat, "+CUSATEND", phonesim_cusatend_notify,
						FALSE, stk, NULL);

	if (sd->vendor == OFONO_VENDOR_PHONESIM)
		g_at_chat_register(sd->chat, "*HCMD:", phonesim_hcmd_notify,
						FALSE, stk, NULL);

	ofono_stk_register(stk);

	return FALSE;
}

static int at_stk_probe(struct ofono_stk *stk, unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct stk_data *sd;

	sd = g_new0(struct stk_data, 1);
	sd->chat = g_at_chat_clone(chat);
	sd->vendor = vendor;

	ofono_stk_set_data(stk, sd);
	g_idle_add(at_stk_register, stk);

	return 0;
}

static void at_stk_remove(struct ofono_stk *stk)
{
	struct stk_data *sd = ofono_stk_get_data(stk);

	g_idle_remove_by_data(stk);
	ofono_stk_set_data(stk, NULL);

	g_at_chat_unref(sd->chat);
	g_free(sd);
}

static struct ofono_stk_driver driver = {
	.name			= "atmodem",
	.probe			= at_stk_probe,
	.remove			= at_stk_remove,
	.envelope		= at_stk_envelope,
	.terminal_response	= at_stk_terminal_response,
};

void at_stk_init(void)
{
	ofono_stk_driver_register(&driver);
}

void at_stk_exit(void)
{
	ofono_stk_driver_unregister(&driver);
}
