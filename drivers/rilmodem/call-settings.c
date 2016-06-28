/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Copyright (C) 2013 Canonical Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-settings.h>

#include "gril.h"
#include "grilutil.h"
#include "grilrequest.h"
#include "grilreply.h"

#include "rilmodem.h"
#include "rilutil.h"
#include "ril_constants.h"
#include "common.h"

struct settings_data {
	GRil *ril;
};

static void ril_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_call_settings *cs = cbd->user;
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	ofono_call_settings_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(sd->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_cw_set(struct ofono_call_settings *cs, int mode, int cls,
			ofono_call_settings_set_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data, cs);
	int ret;
	struct parcel rilp;

	g_ril_request_set_call_waiting(sd->ril, mode, cls, &rilp);

	ret = g_ril_send(sd->ril, RIL_REQUEST_SET_CALL_WAITING, &rilp,
				ril_set_cb, cbd, g_free);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_cw_query_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_call_settings *cs = cbd->user;
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	ofono_call_settings_status_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		int res;

		res = g_ril_reply_parse_query_call_waiting(sd->ril, message);

		CALLBACK_WITH_SUCCESS(cb, res, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	}
}

static void ril_cw_query(struct ofono_call_settings *cs, int cls,
				ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data, cs);
	int ret;
	struct parcel rilp;

	g_ril_request_query_call_waiting(sd->ril, cls, &rilp);

	ret = g_ril_send(sd->ril, RIL_REQUEST_QUERY_CALL_WAITING, &rilp,
				ril_cw_query_cb, cbd, g_free);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_clip_query_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_call_settings *cs = cbd->user;
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	ofono_call_settings_status_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		int res;

		res = g_ril_reply_parse_query_clip(sd->ril, message);

		CALLBACK_WITH_SUCCESS(cb, res, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	}
}

static void ril_clip_query(struct ofono_call_settings *cs,
			ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data, cs);
	int ret;

	ret = g_ril_send(sd->ril, RIL_REQUEST_QUERY_CLIP, NULL,
				ril_clip_query_cb, cbd, g_free);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_clir_query_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_call_settings *cs = cbd->user;
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	ofono_call_settings_clir_cb_t cb = cbd->cb;
	struct reply_clir *rclir;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: Reply failure: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	rclir = g_ril_reply_parse_get_clir(sd->ril, message);
	if (rclir == NULL) {
		ofono_error("%s: parse error", __func__);
		goto error;
	}

	CALLBACK_WITH_SUCCESS(cb, rclir->status, rclir->provisioned, cbd->data);

	g_ril_reply_free_get_clir(rclir);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, cbd->data);
}

static void ril_clir_query(struct ofono_call_settings *cs,
			ofono_call_settings_clir_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data, cs);
	int ret;

	ret = g_ril_send(sd->ril, RIL_REQUEST_GET_CLIR, NULL,
				ril_clir_query_cb, cbd, g_free);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, -1, data);
	}
}


static void ril_clir_set(struct ofono_call_settings *cs, int mode,
			ofono_call_settings_set_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data, cs);
	struct parcel rilp;
	int ret;

	g_ril_request_set_clir(sd->ril, mode, &rilp);

	ret = g_ril_send(sd->ril, RIL_REQUEST_SET_CLIR, &rilp,
				ril_set_cb, cbd, g_free);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_settings *cs = user_data;

	ofono_call_settings_register(cs);

	return FALSE;
}

static int ril_call_settings_probe(struct ofono_call_settings *cs,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct settings_data *sd = g_new0(struct settings_data, 1);

	sd->ril = g_ril_clone(ril);

	ofono_call_settings_set_data(cs, sd);

	g_idle_add(ril_delayed_register, cs);

	return 0;
}

static void ril_call_settings_remove(struct ofono_call_settings *cs)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	ofono_call_settings_set_data(cs, NULL);

	g_ril_unref(sd->ril);
	g_free(sd);
}

static struct ofono_call_settings_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_call_settings_probe,
	.remove			= ril_call_settings_remove,
	.clip_query		= ril_clip_query,
	.cw_query		= ril_cw_query,
	.cw_set			= ril_cw_set,
	.clir_query		= ril_clir_query,
	.clir_set		= ril_clir_set

	/*
	 * Not supported in RIL API
	 * .colp_query		= ril_colp_query,
	 * .colr_query		= ril_colr_query
	 */
};

void ril_call_settings_init(void)
{
	ofono_call_settings_driver_register(&driver);
}

void ril_call_settings_exit(void)
{
	ofono_call_settings_driver_unregister(&driver);
}
