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
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ussd.h>
#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *none_prefix[] = { NULL };

static void cusd_request_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ussd_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("cusd_request_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_ussd_request(struct ofono_ussd *ussd, const char *str,
				ofono_ussd_cb_t cb, void *data)
{
	GAtChat *chat = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, data);
	unsigned char *converted = NULL;
	int dcs;
	int max_len;
	long written;
	char buf[256];

	if (!cbd)
		goto error;

	converted = convert_utf8_to_gsm(str, strlen(str), NULL, &written, 0);

	/* TODO: Be able to convert to UCS2, although the standard does not
	 * indicate that this is actually possible
	 */
	if (!converted)
		goto error;
	else {
		dcs = 15;
		max_len = 182;
	}

	if (written > max_len)
		goto error;

	sprintf(buf, "AT+CUSD=1,\"%*s\",%d", (int) written, converted, dcs);

	g_free(converted);
	converted = NULL;

	if (g_at_chat_send(chat, buf, none_prefix,
				cusd_request_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	if (converted)
		g_free(converted);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void cusd_cancel_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ussd_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("cusd_cancel_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_ussd_cancel(struct ofono_ussd *ussd,
				ofono_ussd_cb_t cb, void *data)
{
	GAtChat *chat = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, "AT+CUSD=2", none_prefix,
				cusd_cancel_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean at_ussd_register(gpointer user)
{
	struct ofono_ussd *ussd = user;

	ofono_ussd_register(ussd);

	return FALSE;
}

static int at_ussd_probe(struct ofono_ussd *ussd, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;

	ofono_ussd_set_data(ussd, chat);
	g_idle_add(at_ussd_register, ussd);

	return 0;
}

static void at_ussd_remove(struct ofono_ussd *ussd)
{
}

static struct ofono_ussd_driver driver = {
	.name = "atmodem",
	.probe = at_ussd_probe,
	.remove = at_ussd_remove,
	.request = at_ussd_request,
	.cancel = at_ussd_cancel
};

void at_ussd_init()
{
	ofono_ussd_driver_register(&driver);
}

void at_ussd_exit()
{
	ofono_ussd_driver_unregister(&driver);
}
