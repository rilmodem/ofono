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
#include <ofono/call-barring.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *clck_prefix[] = { "+CLCK:", NULL };
static const char *none_prefix[] = { NULL };

static void clck_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_query_cb_t callback = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int status_mask, status, class, line;

	decode_at_error(&error, g_at_result_final_response(result));

	status_mask = 0;
	line = 0;
	g_at_result_iter_init(&iter, result);
	while (g_at_result_iter_next(&iter, "+CLCK:")) {
		line++;

		if (!g_at_result_iter_next_number(&iter, &status))
			continue;

		if (!g_at_result_iter_next_number(&iter, &class)) {
			if (line > 1)
				continue;
			else
				class = 7;
		}

		if (status)
			status_mask |= class;
		else
			status_mask &= ~class;
	}

	callback(&error, status_mask, cbd->data);
}

static void at_call_barring_query(struct ofono_call_barring *cb,
					const char *lock, int cls,
					ofono_call_barring_query_cb_t callback,
					void *data)
{
	GAtChat *chat = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data);
	char buf[64];

	if (strlen(lock) != 2)
		goto error;

	if (cls == 7)
		snprintf(buf, sizeof(buf), "AT+CLCK=\"%s\",2", lock);
	else
		snprintf(buf, sizeof(buf), "AT+CLCK=\"%s\",2,,%d", lock, cls);

	if (g_at_chat_send(chat, buf, clck_prefix,
				clck_query_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(callback, 0, data);
}

static void clck_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_set_cb_t callback = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	callback(&error, cbd->data);
}

static void at_call_barring_set(struct ofono_call_barring *cb, const char *lock,
				int enable, const char *passwd, int cls,
				ofono_call_barring_set_cb_t callback,
				void *data)
{
	GAtChat *chat = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data);
	char buf[64];
	int len;

	if (strlen(lock) != 2 || (cls && passwd == NULL))
		goto error;

	len = snprintf(buf, sizeof(buf), "AT+CLCK=\"%s\",%i", lock, enable);
	if (passwd) {
		len += snprintf(buf + len, sizeof(buf) - len,
				",\"%s\"", passwd);
		/* Assume cls == 7 means use defaults */
		if (cls != 7)
			snprintf(buf + len, sizeof(buf) - len, ",%i", cls);
	}

	if (g_at_chat_send(chat, buf, none_prefix,
				clck_set_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(callback, data);
}

static void cpwd_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_set_cb_t callback = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	callback(&error, cbd->data);
}

static void at_call_barring_set_passwd(struct ofono_call_barring *cb,
					const char *lock,
					const char *old_passwd,
					const char *new_passwd,
					ofono_call_barring_set_cb_t callback,
					void *data)
{
	GAtChat *chat = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data);
	char buf[64];

	if (strlen(lock) != 2)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CPWD=\"%s\",\"%s\",\"%s\"",
			lock, old_passwd, new_passwd);

	if (g_at_chat_send(chat, buf, none_prefix,
				cpwd_set_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(callback, data);
}

static gboolean at_call_barring_register(gpointer user)
{
	struct ofono_call_barring *cb = user;

	ofono_call_barring_register(cb);

	return FALSE;
}

static int at_call_barring_probe(struct ofono_call_barring *cb,
					unsigned int vendor, void *user)
{
	GAtChat *chat = user;

	ofono_call_barring_set_data(cb, g_at_chat_clone(chat));
	g_idle_add(at_call_barring_register, cb);

	return 0;
}

static void at_call_barring_remove(struct ofono_call_barring *cb)
{
	GAtChat *chat = ofono_call_barring_get_data(cb);

	g_idle_remove_by_data(cb);
	g_at_chat_unref(chat);
	ofono_call_barring_set_data(cb, NULL);
}

static struct ofono_call_barring_driver driver = {
	.name		= "atmodem",
	.probe		= at_call_barring_probe,
	.remove		= at_call_barring_remove,
	.set		= at_call_barring_set,
	.query		= at_call_barring_query,
	.set_passwd	= at_call_barring_set_passwd,
};

void at_call_barring_init(void)
{
	ofono_call_barring_driver_register(&driver);
}

void at_call_barring_exit(void)
{
	ofono_call_barring_driver_unregister(&driver);
}
