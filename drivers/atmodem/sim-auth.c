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

#include <glib.h>

#include <ofono/modem.h>
#include <ofono/sim-auth.h>

#include "gatchat.h"
#include "gatresult.h"
#include "simutil.h"
#include "vendor.h"

#include "atmodem.h"

struct sim_auth_data {
	GAtChat *chat;
	unsigned int vendor;
};

static const char *cuad_prefix[] = { "+CUAD:", NULL };

static void at_discover_apps_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_list_apps_cb_t cb = cbd->cb;
	struct ofono_error error;
	const unsigned char *dataobj;
	gint linelen;
	unsigned char *buffer;
	int len;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, 0, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	len = 0;
	while (g_at_result_iter_next(&iter, "+CUAD:")) {
		if (!g_at_result_iter_next_hexstring(&iter, NULL, &linelen))
			goto error;

		len += linelen;
	}

	g_at_result_iter_init(&iter, result);

	buffer = g_malloc(len);
	len = 0;

	while (g_at_result_iter_next(&iter, "+CUAD:")) {
		g_at_result_iter_next_hexstring(&iter, &dataobj, &linelen);
		memcpy(buffer + len, dataobj, linelen);
		len += linelen;
	}

	cb(&error, buffer, len, cbd->data);

	g_free(buffer);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static void at_discover_apps(struct ofono_sim_auth *sa,
				ofono_sim_list_apps_cb_t cb,
				void *data)
{
	struct sim_auth_data *sad = ofono_sim_auth_get_data(sa);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(sad->chat, "AT+CUAD", cuad_prefix,
					at_discover_apps_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static gboolean at_sim_auth_register(gpointer user)
{
	struct ofono_sim_auth *sa = user;

	ofono_sim_auth_register(sa);

	return FALSE;
}

static int at_sim_auth_probe(struct ofono_sim_auth *sa, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct sim_auth_data *sad;

	sad = g_new0(struct sim_auth_data, 1);
	sad->chat = g_at_chat_clone(chat);
	sad->vendor = vendor;

	ofono_sim_auth_set_data(sa, sad);
	g_idle_add(at_sim_auth_register, sa);

	return 0;
}

static void at_sim_auth_remove(struct ofono_sim_auth *sa)
{
	struct sim_auth_data *sad = ofono_sim_auth_get_data(sa);

	ofono_sim_auth_set_data(sa, NULL);

	g_at_chat_unref(sad->chat);
	g_free(sad);
}

static struct ofono_sim_auth_driver driver = {
	.name		= "atmodem",
	.probe		= at_sim_auth_probe,
	.remove		= at_sim_auth_remove,
	.list_apps	= at_discover_apps,
};

void at_sim_auth_init(void)
{
	ofono_sim_auth_driver_register(&driver);
}

void at_sim_auth_exit(void)
{
	ofono_sim_auth_driver_unregister(&driver);
}
