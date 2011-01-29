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
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/stk.h>

#include "gatchat.h"
#include "gatresult.h"

#include "calypsomodem.h"

struct stk_data {
	GAtChat *chat;
};

static const char *none_prefix[] = { NULL };
static const char *sate_prefix[] = { "%SATE:", NULL };

static void sate_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_envelope_cb_t cb = cbd->cb;
	GAtResultIter iter;
	struct ofono_error error;
	const guint8 *pdu = NULL;
	gint len = 0;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));

	/*
	 * Ignore errors "SIM memory failure" and "Unknown error", seem
	 * to be generated for no reason.
	 */
	if (!ok && error.type == OFONO_ERROR_TYPE_CMS && error.error == 320) {
		ok = TRUE;
		error.type = OFONO_ERROR_TYPE_NO_ERROR;
	}
	if (!ok && error.type == OFONO_ERROR_TYPE_CME && error.error == 100) {
		ok = TRUE;
		error.type = OFONO_ERROR_TYPE_NO_ERROR;
	}

	if (!ok)
		goto done;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "%SATE:") == FALSE)
		goto done;

	/* Response data is optional */
	g_at_result_iter_next_hexstring(&iter, &pdu, &len);

	DBG("len %d", len);

done:
	cb(&error, pdu, len, cbd->data);
}

static void calypso_stk_envelope(struct ofono_stk *stk, int length,
					const unsigned char *command,
					ofono_stk_envelope_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 64 + length * 2);
	int len;

	DBG("");

	if (buf == NULL)
		goto error;

	len = sprintf(buf, "AT%%SATE=\"");
	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *command++);
	len += sprintf(buf + len, "\"");

	DBG("%s", buf);

	if (g_at_chat_send(sd->chat, buf, sate_prefix,
					sate_cb, cbd, g_free) > 0) {
		g_free(buf);
		return;
	}

error:
	g_free(buf);
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void satr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void calypso_stk_terminal_response(struct ofono_stk *stk, int length,
						const unsigned char *command,
						ofono_stk_generic_cb_t cb,
						void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 64 + length * 2);
	int len;

	DBG("");

	if (buf == NULL)
		goto error;

	len = sprintf(buf, "AT%%SATR=\"");
	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *command++);
	len += sprintf(buf + len, "\"");

	DBG("%s", buf);

	if (g_at_chat_send(sd->chat, buf, none_prefix,
					satr_cb, cbd, g_free) > 0) {
		g_free(buf);
		return;
	}

error:
	g_free(buf);
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void sati_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GAtResultIter iter;
	const guint8 *pdu;
	gint len;
	gboolean ret;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%SATI:"))
		return;

	ret = g_at_result_iter_next_hexstring(&iter, &pdu, &len);
	if (!ret || len == 0) {
		/*
		 * An empty notification is a End Session notification on
		 * the part of the UICC.
		 */
		ofono_stk_proactive_session_end_notify(stk);

		return;
	}

	ofono_stk_proactive_command_notify(stk, len, pdu);
}

static void sata_notify(GAtResult *result, gpointer user_data)
{
	DBG("");

	/* TODO: Pending call alert */
}

static void satn_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GAtResultIter iter;
	const guint8 *pdu;
	gint len;

	DBG("");

	/* Proactive command has been handled by the modem. */
	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "%SATN:") == FALSE)
		return;

	if (g_at_result_iter_next_hexstring(&iter, &pdu, &len) == FALSE)
		return;

	if (len == 0)
		return;

	ofono_stk_proactive_command_handled_notify(stk, len, pdu);
}

static void calypso_stk_register(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct stk_data *sd = ofono_stk_get_data(stk);

	DBG("");

	if (!ok)
		return;

	g_at_chat_register(sd->chat, "%SATI:", sati_notify, FALSE, stk, NULL);
	g_at_chat_register(sd->chat, "%SATA:", sata_notify, FALSE, stk, NULL);
	g_at_chat_register(sd->chat, "%SATN:", satn_notify, FALSE, stk, NULL);

	ofono_stk_register(stk);
}

static int calypso_stk_probe(struct ofono_stk *stk,
				unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct stk_data *sd;

	DBG("");

	sd = g_try_new0(struct stk_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->chat = g_at_chat_clone(chat);

	ofono_stk_set_data(stk, sd);

	/*
	 * Provide terminal profile data needed for the download and
	 * enable %SATI / %SATN.  The actual PROFILE DOWNLOAD will
	 * happen during AT+CFUN=1 later.
	 */
	g_at_chat_send(sd->chat, "AT%SATC=1,\"19E1FFFF0000FF7FFF03FEFF\"",
			none_prefix, NULL, stk, NULL);

	/* Enable Call Control / SMS Control */
	g_at_chat_send(sd->chat, "AT%SATCC=1",
			none_prefix, calypso_stk_register, stk, NULL);

	return 0;
}

static void calypso_stk_remove(struct ofono_stk *stk)
{
	struct stk_data *sd = ofono_stk_get_data(stk);

	DBG("");

	ofono_stk_set_data(stk, NULL);

	g_at_chat_unref(sd->chat);
	g_free(sd);
}

static struct ofono_stk_driver driver = {
	.name			= "calypsomodem",
	.probe			= calypso_stk_probe,
	.remove			= calypso_stk_remove,
	.envelope		= calypso_stk_envelope,
	.terminal_response	= calypso_stk_terminal_response,
};

void calypso_stk_init(void)
{
	ofono_stk_driver_register(&driver);
}

void calypso_stk_exit(void)
{
	ofono_stk_driver_unregister(&driver);
}
