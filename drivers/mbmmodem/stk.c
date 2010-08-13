/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#include "mbmmodem.h"

struct stk_data {
	GAtChat *chat;
};

static const char *stke_prefix[] = { "%STKE:", NULL };
static const char *none_prefix[] = { NULL };

static void mbm_stke_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_envelope_cb_t cb = cbd->cb;
	GAtResultIter iter;
	struct ofono_error error;
	const guint8 *pdu = NULL;
	gint len = 0;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, 0, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*STKE:") == TRUE)
		if (g_at_result_iter_next_hexstring(&iter, &pdu, &len) == FALSE)
			goto error;

	cb(&error, pdu, len, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static void mbm_stk_envelope(struct ofono_stk *stk, int length,
				const unsigned char *command,
				ofono_stk_envelope_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 64 + length * 2);
	int len, ret;

	DBG("");

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT*STKE=\"");
	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *command++);
	len += sprintf(buf + len, "\"");

	DBG("%s", buf);

	ret = g_at_chat_send(sd->chat, buf, stke_prefix,
				mbm_stke_cb, cbd, g_free);

	DBG("ret %d", ret);

	g_free(buf);
	buf = NULL;

	if (ret > 0)
		return;

error:
	g_free(buf);
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void mbm_stkr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void mbm_stk_terminal_response(struct ofono_stk *stk, int length,
					const unsigned char *command,
					ofono_stk_generic_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 64 + length * 2);
	int len, ret;

	DBG("");

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT*STKR=\"");
	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *command++);
	len += sprintf(buf + len, "\"");

	ret = g_at_chat_send(sd->chat, buf, none_prefix,
				mbm_stkr_cb, cbd, g_free);

	g_free(buf);
	buf = NULL;

	if (ret > 0)
		return;

error:
	g_free(buf);
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void stki_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GAtResultIter iter;
	const guint8 *pdu;
	gint len;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "*STKI:"))
		return;

	if (!g_at_result_iter_next_hexstring(&iter, &pdu, &len))
		return;

	ofono_stk_proactive_command_notify(stk, len, pdu);
}

static void stkn_notify(GAtResult *result, gpointer user_data)
{
	DBG("");

	/* Proactive command has been handled by the modem.  Should
	 * the core be notified?  For now we just ignore it because
	 * we must not respond to the command.
	 */
}

static void stkend_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	DBG("");

	ofono_stk_proactive_session_end_notify(stk);
}

static void mbm_stkc_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct stk_data *sd = ofono_stk_get_data(stk);

	DBG("");

	if (!ok)
		return;

	g_at_chat_register(sd->chat, "*STKI:", stki_notify, FALSE, stk, NULL);
	g_at_chat_register(sd->chat, "*STKN:", stkn_notify, FALSE, stk, NULL);
	g_at_chat_register(sd->chat, "*STKEND",
					stkend_notify, FALSE, stk, NULL);

	ofono_stk_register(stk);
}

static int mbm_stk_probe(struct ofono_stk *stk, unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct stk_data *sd;

	DBG("");

	sd = g_new0(struct stk_data, 1);
	sd->chat = g_at_chat_clone(chat);

	ofono_stk_set_data(stk, sd);

	/* Perform PROFILE DOWNLOAD and enable *STKI / *STKN */
	g_at_chat_send(sd->chat, "AT*STKC=1,\"19E1FFFF0000FF7FFF03FEFF\"",
			none_prefix, mbm_stkc_cb, stk, NULL);

	return 0;
}

static void mbm_stk_remove(struct ofono_stk *stk)
{
	struct stk_data *sd = ofono_stk_get_data(stk);

	DBG("");

	ofono_stk_set_data(stk, NULL);

	g_at_chat_unref(sd->chat);
	g_free(sd);
}

static struct ofono_stk_driver driver = {
	.name			= "mbmmodem",
	.probe			= mbm_stk_probe,
	.remove			= mbm_stk_remove,
	.envelope		= mbm_stk_envelope,
	.terminal_response	= mbm_stk_terminal_response,
};

void mbm_stk_init()
{
	ofono_stk_driver_register(&driver);
}

void mbm_stk_exit()
{
	ofono_stk_driver_unregister(&driver);
}
