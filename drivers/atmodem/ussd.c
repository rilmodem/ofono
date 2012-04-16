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

static const unsigned char *ucs2_gsm_to_packed(const char *content,
						long *msg_len,
						unsigned char *msg)
{
	unsigned char *decoded;
	long len;
	unsigned char *gsm;
	long written;
	unsigned char *packed;
	unsigned char buf[182 * 2]; /* 182 USSD chars * 2 (UCS2) */

	if (strlen(content) > sizeof(buf) * 2) /* Hex, 2 chars / byte */
		return NULL;

	decoded = decode_hex_own_buf(content, -1, &len, 0, buf);

	if (decoded == NULL)
		return NULL;

	gsm = convert_ucs2_to_gsm(decoded, len, NULL, &written, 0);

	if (gsm == NULL)
		return NULL;

	if (written > 182) {
		g_free(gsm);
		return NULL;
	}

	packed = pack_7bit_own_buf(gsm, written, 0, TRUE, msg_len, 0, msg);
	g_free(gsm);

	return packed;
}

static void cusd_parse(GAtResult *result, struct ofono_ussd *ussd)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);
	GAtResultIter iter;
	int status;
	const char *content;
	int dcs;
	enum sms_charset charset;
	unsigned char msg[160];
	const unsigned char *msg_ptr = NULL;
	long msg_len;

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

	DBG("response charset %d modem charset %d", charset, data->charset);

	switch (charset) {
	case SMS_CHARSET_7BIT:
		switch (data->charset) {
		case AT_UTIL_CHARSET_GSM:
			msg_ptr = pack_7bit_own_buf((const guint8 *) content,
							-1, 0, TRUE, &msg_len,
							0, msg);
			break;

		case AT_UTIL_CHARSET_UTF8:
			if (ussd_encode(content, &msg_len, msg) == TRUE)
				msg_ptr = msg;

			break;

		case AT_UTIL_CHARSET_UCS2:
			msg_ptr = ucs2_gsm_to_packed(content, &msg_len, msg);
			break;

		default:
			msg_ptr = NULL;
		}
		break;

	case SMS_CHARSET_8BIT:
	case SMS_CHARSET_UCS2:
		msg_ptr = decode_hex_own_buf(content, -1, &msg_len, 0, msg);
		break;
	}

	DBG("msg ptr %p msg len %ld", msg_ptr, msg_len);

out:
	ofono_ussd_notify(ussd, status, dcs, msg_ptr, msg_ptr ? msg_len : 0);
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
	char buf[512];
	enum sms_charset charset;

	cbd->user = ussd;

	if (!cbs_dcs_decode(dcs, NULL, NULL, &charset,
					NULL, NULL, NULL))
		goto error;

	if (charset == SMS_CHARSET_7BIT) {
		unsigned char unpacked_buf[182];
		long written;

		unpack_7bit_own_buf(pdu, len, 0, TRUE, sizeof(unpacked_buf),
					&written, 0, unpacked_buf);

		if (written < 1)
			goto error;

		snprintf(buf, sizeof(buf), "AT+CUSD=1,\"%.*s\",%d",
				(int) written, unpacked_buf, dcs);
	} else {
		char coded_buf[321];
		char *converted = encode_hex_own_buf(pdu, len, 0, coded_buf);

		if (converted == NULL)
			goto error;

		snprintf(buf, sizeof(buf), "AT+CUSD=1,\"%s\",%d",
				converted, dcs);
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

	switch (data->vendor) {
	case OFONO_VENDOR_GOBI:
	case OFONO_VENDOR_QUALCOMM_MSM:
		/* All errors and notifications arrive unexpected and
		 * thus just reset the state here. This is safer than
		 * getting stuck in a dead-lock. */
		error.type = OFONO_ERROR_TYPE_NO_ERROR;
		error.error = 0;
		break;
	default:
		break;
	}

	cb(&error, cbd->data);
}

static void at_ussd_cancel(struct ofono_ussd *ussd,
				ofono_ussd_cb_t cb, void *user_data)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	cbd->user = data;

	if (g_at_chat_send(data->chat, "AT+CUSD=2", none_prefix,
				cusd_cancel_cb, cbd, g_free) > 0)
		return;

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
		ofono_ussd_remove(ussd);
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

	g_at_chat_send(data->chat, "AT+CSCS?", cscs_prefix,
			read_charset_cb, data, NULL);

	g_at_chat_send(data->chat, "AT+CUSD=1", NULL,
			at_ussd_register, ussd, NULL);

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

void at_ussd_init(void)
{
	ofono_ussd_driver_register(&driver);
}

void at_ussd_exit(void)
{
	ofono_ussd_driver_unregister(&driver);
}
