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
#include <ofono/ussd.h>
#include "util.h"
#include "smsutil.h"

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

struct cusd_req {
	ofono_ussd_cb_t cb;
	void *data;
	struct ofono_ussd *ussd;
};

static const char *cusd_prefix[] = { "+CUSD:", NULL };
static const char *none_prefix[] = { NULL };

static void cusd_parse(GAtResult *result, struct ofono_ussd *ussd)
{
	GAtResultIter iter;
	int status;
	int dcs;
	const char *content;
	char *converted = NULL;
	gboolean udhi;
	enum sms_charset charset;
	gboolean compressed;
	gboolean iso639;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CUSD:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	if (!g_at_result_iter_next_string(&iter, &content))
		goto out;

	if (!g_at_result_iter_next_number(&iter, &dcs))
		goto out;

	if (!cbs_dcs_decode(dcs, &udhi, NULL, &charset,
				&compressed, NULL, &iso639))
		goto out;

	if (udhi || compressed || iso639)
		goto out;

	if (charset == SMS_CHARSET_7BIT)
		converted = convert_gsm_to_utf8((const guint8 *) content,
						strlen(content), NULL, NULL, 0);

	else if (charset == SMS_CHARSET_8BIT) {
		/* TODO: Figure out what to do with 8 bit data */
		ofono_error("8-bit coded USSD response received");
		status = 4; /* Not supported */
	} else {
		/* No other encoding is mentioned in TS27007 7.15 */
		ofono_error("Unsupported USSD data coding scheme (%02x)", dcs);
		status = 4; /* Not supported */
	}

out:
	ofono_ussd_notify(ussd, status, converted);

	g_free(converted);
}

static void cusd_request_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cusd_req *cbd = user_data;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cbd->cb(&error, cbd->data);

	cusd_parse(result, cbd->ussd);
}

static void at_ussd_request(struct ofono_ussd *ussd, const char *str,
				ofono_ussd_cb_t cb, void *data)
{
	GAtChat *chat = ofono_ussd_get_data(ussd);
	struct cusd_req *cbd = g_try_new0(struct cusd_req, 1);
	unsigned char *converted = NULL;
	int dcs;
	int max_len;
	long written;
	char buf[256];

	if (!cbd)
		goto error;

	cbd->cb = cb;
	cbd->data = data;
	cbd->ussd = ussd;

	converted = convert_utf8_to_gsm(str, strlen(str), NULL, &written, 0);

	if (!converted)
		goto error;
	else {
		dcs = 15;
		max_len = 182;
	}

	if (written > max_len)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CUSD=1,\"%.*s\",%d",
			(int) written, converted, dcs);

	g_free(converted);
	converted = NULL;

	if (g_at_chat_send(chat, buf, cusd_prefix,
				cusd_request_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);
	g_free(converted);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void cusd_cancel_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ussd_cb_t cb = cbd->cb;
	struct ofono_error error;

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
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void cusd_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_ussd *ussd = user_data;

	cusd_parse(result, ussd);
}

static void at_ussd_register(gboolean ok, GAtResult *result, gpointer user)
{
	struct ofono_ussd *ussd = user;
	GAtChat *chat = ofono_ussd_get_data(ussd);

	if (!ok) {
		ofono_error("Could not enable CUSD notifications");
		return;
	}

	g_at_chat_register(chat, "+CUSD:", cusd_notify, FALSE, ussd, NULL);

	ofono_ussd_register(ussd);
}

static int at_ussd_probe(struct ofono_ussd *ussd, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;

	chat = g_at_chat_clone(chat);
	ofono_ussd_set_data(ussd, chat);

	g_at_chat_send(chat, "AT+CUSD=1", NULL, at_ussd_register, ussd, NULL);

	return 0;
}

static void at_ussd_remove(struct ofono_ussd *ussd)
{
	GAtChat *chat = ofono_ussd_get_data(ussd);

	g_at_chat_unref(chat);
	ofono_ussd_set_data(ussd, NULL);
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
