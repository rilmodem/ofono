/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
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

#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ussd.h>
#include "util.h"

#include "gatchat.h"

#include "speedupmodem.h"

static const char *cusd_prefix[] = { "+CUSD:", NULL };
static const char *none_prefix[] = { NULL };

struct ussd_data {
	GAtChat *chat;
};

static void cusd_parse(GAtResult *result, struct ofono_ussd *ussd)
{
	GAtResultIter iter;
	int status, dcs;
	const char *content;
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

	msg_ptr = decode_hex_own_buf(content, -1, &msg_len, 0, msg);

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

static void speedup_ussd_request(struct ofono_ussd *ussd, int dcs,
				const unsigned char *pdu, int len,
				ofono_ussd_cb_t cb, void *user_data)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char buf[512], coded_buf[182];
	long written;

	cbd->user = ussd;

	unpack_7bit_own_buf(pdu, len, 0, TRUE, sizeof(coded_buf),
				&written, 0, (unsigned char *)coded_buf);
	if (written < 1)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CUSD=1,\"%.*s\",%d", (int) written,
			coded_buf, dcs);

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
	struct ofono_error error;

	/*
	 * All errors and notifications arrive unexpected and
	 * thus just reset the state here. This is safer than
	 * getting stuck in a dead-lock.
	 */
	error.type = OFONO_ERROR_TYPE_NO_ERROR;
	error.error = 0;

	cb(&error, cbd->data);
}

static void speedup_ussd_cancel(struct ofono_ussd *ussd,
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

static void cusd_register(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_ussd *ussd = user_data;
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

static int speedup_ussd_probe(struct ofono_ussd *ussd,
					unsigned int vendor, void *user)
{
	GAtChat *chat = user;
	struct ussd_data *data;

	data = g_try_new0(struct ussd_data, 1);
	if (data == NULL)
		return -ENOMEM;

	data->chat = g_at_chat_clone(chat);

	ofono_ussd_set_data(ussd, data);

	g_at_chat_send(data->chat, "AT+CUSD=1", none_prefix,
					cusd_register, ussd, NULL);

	return 0;
}

static void speedup_ussd_remove(struct ofono_ussd *ussd)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);

	ofono_ussd_set_data(ussd, NULL);

	g_at_chat_unref(data->chat);
	g_free(data);
}

static struct ofono_ussd_driver driver = {
	.name		= "speedupmodem",
	.probe		= speedup_ussd_probe,
	.remove		= speedup_ussd_remove,
	.request	= speedup_ussd_request,
	.cancel		= speedup_ussd_cancel,
};

void speedup_ussd_init(void)
{
	ofono_ussd_driver_register(&driver);
}

void speedup_ussd_exit(void)
{
	ofono_ussd_driver_unregister(&driver);
}
