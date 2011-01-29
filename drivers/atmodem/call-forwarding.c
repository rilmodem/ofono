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
#include <ofono/call-forwarding.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *none_prefix[] = { NULL };
static const char *ccfc_prefix[] = { "+CCFC:", NULL };

static void ccfc_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_forwarding_query_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int num = 0;
	struct ofono_call_forwarding_condition *list = NULL;
	int i;
	int maxlen;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto out;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CCFC:"))
		num += 1;

	/* Specification is really unclear about this
	 * generate status=0 for all classes just in case
	 */
	if (num == 0) {
		list = g_new0(struct ofono_call_forwarding_condition, 1);
		num = 1;

		list->status = 0;
		list->cls = GPOINTER_TO_INT(cbd->user);

		goto out;
	}

	list = g_new(struct ofono_call_forwarding_condition, num);

	g_at_result_iter_init(&iter, result);

	maxlen = OFONO_MAX_PHONE_NUMBER_LENGTH;

	for (num = 0; g_at_result_iter_next(&iter, "+CCFC:"); num++) {
		const char *str;

		g_at_result_iter_next_number(&iter, &(list[num].status));
		g_at_result_iter_next_number(&iter, &(list[num].cls));

		list[num].phone_number.number[0] = '\0';
		list[num].phone_number.type = 129;
		list[num].time = 20;

		if (!g_at_result_iter_next_string(&iter, &str))
			continue;

		strncpy(list[num].phone_number.number, str, maxlen);
		list[num].phone_number.number[maxlen] = '\0';

		g_at_result_iter_next_number(&iter,
						&(list[num].phone_number.type));

		if (!g_at_result_iter_skip_next(&iter))
			continue;

		if (!g_at_result_iter_skip_next(&iter))
			continue;

		g_at_result_iter_next_number(&iter, &(list[num].time));
	}

	for (i = 0; i < num; i++)
		DBG("ccfc_cb: %d, %d, %s(%d) - %d sec",
			list[i].status, list[i].cls,
			list[i].phone_number.number,
			list[i].phone_number.type, list[i].time);

out:
	cb(&error, num, list, cbd->data);
	g_free(list);
}

static void at_ccfc_query(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_query_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_forwarding_get_data(cf);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	cbd->user = GINT_TO_POINTER(cls);

	if (cls == 7)
		snprintf(buf, sizeof(buf), "AT+CCFC=%d,2", type);
	else
		snprintf(buf, sizeof(buf), "AT+CCFC=%d,2,,,%d", type, cls);

	if (g_at_chat_send(chat, buf, ccfc_prefix,
				ccfc_query_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
}

static void ccfc_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_forwarding_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_ccfc_set(struct ofono_call_forwarding *cf, const char *buf,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	GAtChat *chat = ofono_call_forwarding_get_data(cf);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(chat, buf, none_prefix,
				ccfc_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_ccfc_erasure(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	char buf[128];
	int len;

	len = snprintf(buf, sizeof(buf), "AT+CCFC=%d,4", type);

	if (cls != 7)
		snprintf(buf + len, sizeof(buf) - len, ",,,%d", cls);

	at_ccfc_set(cf, buf, cb, data);
}

static void at_ccfc_deactivation(struct ofono_call_forwarding *cf,
					int type, int cls,
					ofono_call_forwarding_set_cb_t cb,
					void *data)
{
	char buf[128];
	int len;

	len = snprintf(buf, sizeof(buf), "AT+CCFC=%d,0", type);

	if (cls != 7)
		snprintf(buf + len, sizeof(buf) - len, ",,,%d", cls);

	at_ccfc_set(cf, buf, cb, data);
}

static void at_ccfc_activation(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	char buf[128];
	int len;

	len = snprintf(buf, sizeof(buf), "AT+CCFC=%d,1", type);

	if (cls != 7)
		snprintf(buf + len, sizeof(buf) - len, ",,,%d", cls);

	at_ccfc_set(cf, buf, cb, data);
}

static void at_ccfc_registration(struct ofono_call_forwarding *cf,
					int type, int cls,
					const struct ofono_phone_number *ph,
					int time,
					ofono_call_forwarding_set_cb_t cb,
					void *data)
{
	char buf[128];
	int offset;

	offset = snprintf(buf, sizeof(buf), "AT+CCFC=%d,3,\"%s\",%d,%d", type,
				ph->number, ph->type, cls);

	if (type == 2 || type == 4 || type == 5)
		snprintf(buf+offset, sizeof(buf) - offset, ",,,%d", time);

	at_ccfc_set(cf, buf, cb, data);
}

static gboolean at_ccfc_register(gpointer user)
{
	struct ofono_call_forwarding *cf = user;

	ofono_call_forwarding_register(cf);

	return FALSE;
}

static int at_ccfc_probe(struct ofono_call_forwarding *cf, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;

	ofono_call_forwarding_set_data(cf, g_at_chat_clone(chat));
	g_idle_add(at_ccfc_register, cf);

	return 0;
}

static void at_ccfc_remove(struct ofono_call_forwarding *cf)
{
	GAtChat *chat = ofono_call_forwarding_get_data(cf);

	g_at_chat_unref(chat);
	ofono_call_forwarding_set_data(cf, NULL);
}

static struct ofono_call_forwarding_driver driver = {
	.name		= "atmodem",
	.probe		= at_ccfc_probe,
	.remove		= at_ccfc_remove,
	.registration	= at_ccfc_registration,
	.activation	= at_ccfc_activation,
	.query		= at_ccfc_query,
	.deactivation	= at_ccfc_deactivation,
	.erasure	= at_ccfc_erasure
};

void at_call_forwarding_init(void)
{
	ofono_call_forwarding_driver_register(&driver);
}

void at_call_forwarding_exit(void)
{
	ofono_call_forwarding_driver_unregister(&driver);
}
