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
#include "vendor.h"

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *cusd_prefix[] = { "+CUSD:", NULL };
static const char *none_prefix[] = { NULL };
static const char *cscs_prefix[] = { "+CSCS:", NULL };

struct ussd_data {
	GAtChat *chat;
	unsigned int vendor;
	enum at_util_charset charset;
};

static void read_charset_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ussd_data *data = user_data;

	if (!ok)
		return;

	at_util_parse_cscs_query(result, &data->charset);
}

static void cusd_parse(GAtResult *result, struct ofono_ussd *ussd)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);
	GAtResultIter iter;
	int status;
	int dcs;
	const char *content = NULL;
	enum sms_charset charset;
	unsigned char msg[160];
	const unsigned char *msg_ptr = NULL;
	unsigned char *converted = NULL;
	long written;
	long msg_len = 0;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CUSD:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	if (!g_at_result_iter_next_string(&iter, &content))
		goto out;

	if (!g_at_result_iter_next_number(&iter, &dcs))
		dcs = 0;

	if (!cbs_dcs_decode(dcs, NULL, NULL, &charset, NULL, NULL, NULL)) {
		ofono_error("Unsupported USSD data coding scheme (%02x)", dcs);
		status = 4; /* Not supported */
		goto out;
	}

	switch (charset) {
	case SMS_CHARSET_7BIT:
		if (data->charset == AT_UTIL_CHARSET_GSM)
			msg_ptr = pack_7bit_own_buf((const guint8 *) content,
					strlen(content), 0, TRUE, &msg_len, 0, msg);
		else if (data->charset == AT_UTIL_CHARSET_UTF8)
			ussd_encode(content, &msg_len, msg);
		else if (data->charset == AT_UTIL_CHARSET_UCS2) {
			msg_ptr = decode_hex_own_buf(content, -1, &msg_len, 0, msg);

			converted = convert_ucs2_to_gsm(msg_ptr, msg_len, NULL,
							&written, 0);
			if (!converted) {
				msg_ptr = NULL;
				msg_len = 0;
				goto out;
			}

			msg_ptr = pack_7bit_own_buf(converted,
						written, 0, TRUE,
						&msg_len, 0, msg);

			g_free(converted);
		}
		break;
	case SMS_CHARSET_8BIT:
		msg_ptr = decode_hex_own_buf(content, -1, &msg_len, 0, msg);
		break;
	case SMS_CHARSET_UCS2:
		msg_ptr = decode_hex_own_buf(content, -1, &msg_len, 0, msg);
		break;
	}

out:
	ofono_ussd_notify(ussd, status, dcs, msg_ptr, msg_len);
}

static void cusd_request_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ussd_cb_t cb = cbd->cb;
	struct ofono_ussd *ussd = cbd->user;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);

	cusd_parse(result, ussd);
}

static void at_ussd_request(struct ofono_ussd *ussd, int dcs,
				const unsigned char *pdu, int len,
				ofono_ussd_cb_t cb, void *user_data)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char buf[256];
	unsigned char unpacked_buf[182];
	char coded_buf[160];
	char *converted;
	long written;
	enum sms_charset charset;

	if (!cbd)
		goto error;

	cbd->user = ussd;

	if (!cbs_dcs_decode(dcs, NULL, NULL, &charset,
					NULL, NULL, NULL))
		goto error;

	if (charset == SMS_CHARSET_7BIT) {
		unpack_7bit_own_buf(pdu, len, 0, TRUE, sizeof(unpacked_buf),
					&written, 0, unpacked_buf);

		if (written < 1)
			goto error;

		snprintf(buf, sizeof(buf), "AT+CUSD=1,\"%.*s\",%d",
				(int) written, unpacked_buf, dcs);
	} else {
		converted = encode_hex_own_buf(pdu, len, 0, coded_buf);
		if (!converted)
			goto error;

		snprintf(buf, sizeof(buf), "AT+CUSD=1,\"%.*s\",%d",
				strlen(converted), converted, dcs);
	}

	if (data->vendor == OFONO_VENDOR_QUALCOMM_MSM) {
		/* Ensure that the modem is using GSM character set. It
		 * seems it defaults to IRA and then umlauts are not
		 * properly encoded. The modem returns some weird from
		 * of Latin-1, but it is not really Latin-1 either. */
		g_at_chat_send(data->chat, "AT+CSCS=\"GSM\"", none_prefix,
							NULL, NULL, NULL);
	}

	if (g_at_chat_send(data->chat, buf, cusd_prefix,
				cusd_request_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void cusd_cancel_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ussd_cb_t cb = cbd->cb;
	struct ussd_data *data = cbd->user;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (data->vendor == OFONO_VENDOR_QUALCOMM_MSM) {
		/* All errors and notifications arrive unexpected and
		 * thus just reset the state here. This is safer than
		 * getting stuck in a dead-lock. */
		error.type = OFONO_ERROR_TYPE_NO_ERROR;
		error.error = 0;
	}

	cb(&error, cbd->data);
}

static void at_ussd_cancel(struct ofono_ussd *ussd,
				ofono_ussd_cb_t cb, void *user_data)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	if (!cbd)
		goto error;

	cbd->user = data;

	if (g_at_chat_send(data->chat, "AT+CUSD=2", none_prefix,
				cusd_cancel_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void cusd_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_ussd *ussd = user_data;

	cusd_parse(result, ussd);
}

static void at_ussd_register(gboolean ok, GAtResult *result, gpointer user)
{
	struct ofono_ussd *ussd = user;
	struct ussd_data *data = ofono_ussd_get_data(ussd);

	if (!ok) {
		ofono_error("Could not enable CUSD notifications");
		return;
	}

	g_at_chat_register(data->chat, "+CUSD:", cusd_notify,
						FALSE, ussd, NULL);

	ofono_ussd_register(ussd);
}

static int at_ussd_probe(struct ofono_ussd *ussd, unsigned int vendor,
				void *user)
{
	GAtChat *chat = user;
	struct ussd_data *data;

	data = g_new0(struct ussd_data, 1);
	data->chat = g_at_chat_clone(chat);
	data->vendor = vendor;

	ofono_ussd_set_data(ussd, data);

	g_at_chat_send(chat, "AT+CSCS?", cscs_prefix, read_charset_cb, data,
			NULL);

	g_at_chat_send(chat, "AT+CUSD=1", NULL, at_ussd_register, ussd, NULL);

	return 0;
}

static void at_ussd_remove(struct ofono_ussd *ussd)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);

	ofono_ussd_set_data(ussd, NULL);

	g_at_chat_unref(data->chat);
	g_free(data);
}

static struct ofono_ussd_driver driver = {
	.name		= "atmodem",
	.probe		= at_ussd_probe,
	.remove		= at_ussd_remove,
	.request	= at_ussd_request,
	.cancel		= at_ussd_cancel
};

void at_ussd_init()
{
	ofono_ussd_driver_register(&driver);
}

void at_ussd_exit()
{
	ofono_ussd_driver_unregister(&driver);
}
