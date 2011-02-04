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
#include <ofono/ctm.h>

#include "gatchat.h"
#include "gatresult.h"

#include "ifxmodem.h"

static const char *none_prefix[] = { NULL };
static const char *xctms_prefix[] = { "+XCTMS:", NULL };

struct ctm_data {
	GAtChat *chat;
};

static void xctms_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ctm_query_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int value;
	ofono_bool_t enable;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+XCTMS:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &value) == FALSE)
		goto error;

	/* FULL TTY mode status only sent to oFono */
	enable = (value == 1) ? TRUE : FALSE;

	cb(&error, enable, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ifx_query_tty(struct ofono_ctm *ctm, ofono_ctm_query_cb_t cb,
				void *data)
{
	struct ctm_data *ctmd = ofono_ctm_get_data(ctm);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(ctmd->chat, "AT+XCTMS?", xctms_prefix,
				xctms_query_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void xctms_modify_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ctm_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	/* TODO: Audio path configuration */

	cb(&error, cbd->data);
}

static void ifx_set_tty(struct ofono_ctm *ctm, ofono_bool_t enable,
				ofono_ctm_set_cb_t cb, void *data)
{
	struct ctm_data *ctmd = ofono_ctm_get_data(ctm);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[20];

	/* Only FULL TTY mode enabled/disabled */
	snprintf(buf, sizeof(buf), "AT+XCTMS=%i", enable ? 1 : 0);

	if (g_at_chat_send(ctmd->chat, buf, none_prefix,
				xctms_modify_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void xctms_support_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_ctm *ctm = user_data;

	if (ok)
		ofono_ctm_register(ctm);
}

static int ifx_ctm_probe(struct ofono_ctm *ctm,
				unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct ctm_data *ctmd;

	ctmd = g_try_new0(struct ctm_data, 1);
	if (ctmd == NULL)
		return -ENOMEM;

	ctmd->chat = g_at_chat_clone(chat);

	ofono_ctm_set_data(ctm, ctmd);

	g_at_chat_send(ctmd->chat, "AT+XCTMS=?", xctms_prefix,
			xctms_support_cb, ctm, NULL);

	return 0;
}

static void ifx_ctm_remove(struct ofono_ctm *ctm)
{
	struct ctm_data *ctmd = ofono_ctm_get_data(ctm);

	ofono_ctm_set_data(ctm, NULL);

	g_at_chat_unref(ctmd->chat);
	g_free(ctmd);
}

static struct ofono_ctm_driver driver = {
	.name           = "ifxmodem",
	.probe          = ifx_ctm_probe,
	.remove         = ifx_ctm_remove,
	.query_tty      = ifx_query_tty,
	.set_tty        = ifx_set_tty,
};

void ifx_ctm_init(void)
{
	ofono_ctm_driver_register(&driver);
}

void ifx_ctm_exit(void)
{
	ofono_ctm_driver_unregister(&driver);
}
