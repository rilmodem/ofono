/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
 *  Copyright (C) 2014 Canonical Ltd.
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
#include <ofono/call-forwarding.h>

#include "gril.h"
#include "grilrequest.h"
#include "grilreply.h"
#include "grilunsol.h"

#include "rilutil.h"
#include "rilmodem.h"

#include "common.h"

enum cf_action {
	CF_ACTION_DISABLE,
	CF_ACTION_ENABLE,
	CF_ACTION_INTERROGATE,
	CF_ACTION_REGISTRATION,
	CF_ACTION_ERASURE,
};

struct forw_data {
	GRil *ril;
	enum cf_action last_action;
	int last_cls;
};

static const char *cf_action_to_string(enum cf_action action)
{
	switch (action) {
	case CF_ACTION_DISABLE:
		return "DISABLE";
	case CF_ACTION_ENABLE:
		return "ENABLE";
	case CF_ACTION_INTERROGATE:
		return "INTERROGATE";
	case CF_ACTION_REGISTRATION:
		return "REGISTRATION";
	case CF_ACTION_ERASURE:
		return "ERASURE";
	}

	return NULL;
}

static void ril_query_call_fwd_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct forw_data *fd = ofono_call_forwarding_get_data(cbd->user);
	ofono_call_forwarding_query_cb_t cb = cbd->cb;
	struct ofono_call_forwarding_condition *list;
	unsigned int list_size;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: rild error: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	list = g_ril_reply_parse_query_call_fwd(fd->ril, message, &list_size);
	/*
	 * From atmodem:
	 *
	 * Specification is really unclear about this
	 * generate status=0 for all classes just in case
	 */
	if (list_size == 0) {
		list = g_new0(struct ofono_call_forwarding_condition, 1);
		list_size = 1;

		list->status = 0;
		list->cls = fd->last_cls;
	} else if (list == NULL) {
		goto error;
	}

	CALLBACK_WITH_SUCCESS(cb, (int) list_size, list, cbd->data);
	g_free(list);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
}

static void ril_set_forward_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_forwarding_set_cb_t cb = cbd->cb;
	struct forw_data *fd = ofono_call_forwarding_get_data(cbd->user);

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(fd->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: CF %s failed; rild error: %s", __func__,
				cf_action_to_string(fd->last_action),
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static int ril_send_forward_cmd(int type, int cls,
				const struct ofono_phone_number *number,
				int time,
				struct cb_data *cbd,
				enum cf_action action)
{
	struct ofono_call_forwarding *cf = cbd->user;
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct parcel rilp;
	struct req_call_fwd fwd_req;
	int ret = 0, request;
	GRilResponseFunc response_func;

	if (action == CF_ACTION_INTERROGATE) {
		request = RIL_REQUEST_QUERY_CALL_FORWARD_STATUS;
		response_func = ril_query_call_fwd_cb;
	} else {
		request = RIL_REQUEST_SET_CALL_FORWARD;
		response_func = ril_set_forward_cb;
	}

	DBG("%s - %s", ril_request_id_to_string(request),
		cf_action_to_string(action));

	/*
	 * Modem seems to respond with error to all queries
	 * or settings made with bearer class
	 * BEARER_CLASS_DEFAULT. Design decision: If given
	 * class is BEARER_CLASS_DEFAULT let's map it to
	 * SERVICE_CLASS_NONE as with it e.g. ./send-ussd '*21*<phone_number>#'
	 * returns cls:53 i.e. 1+4+16+32 as service class.
	*/
	if (cls == BEARER_CLASS_DEFAULT)
		cls = SERVICE_CLASS_NONE;

	fd->last_action = action;
	fd->last_cls = cls;

	fwd_req.action = (int) action;
	fwd_req.type = type;
	fwd_req.cls = cls;
	fwd_req.number = number;

	/*
	 * time has no real meaing for action commands other
	 * then registration, so if not needed, set arbitrary
	 * 60s time so rild doesn't return an error.
	 */
	if (time == -1)
		fwd_req.time = 60;
	else
		fwd_req.time = time;

	g_ril_request_call_fwd(fd->ril, &fwd_req, &rilp);

	ret = g_ril_send(fd->ril, request, &rilp, response_func, cbd, g_free);
	if (ret == 0)
		ofono_error("%s: CF action %s failed", __func__,
				cf_action_to_string(action));
	return ret;
}

static void ril_activate(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, cf);

	if (ril_send_forward_cmd(type, cls, NULL, -1, cbd,
					CF_ACTION_ENABLE) == 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void ril_erasure(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, cf);

	if (ril_send_forward_cmd(type, cls, NULL, -1, cbd,
					CF_ACTION_ERASURE) == 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void ril_deactivate(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, cf);

	if (ril_send_forward_cmd(type, cls, NULL, -1, cbd,
					CF_ACTION_DISABLE) == 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void ril_registration(struct ofono_call_forwarding *cf, int type,
				int cls,
				const struct ofono_phone_number *number,
				int time, ofono_call_forwarding_set_cb_t cb,
				void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, cf);

	if (ril_send_forward_cmd(type, cls, number, time, cbd,
					CF_ACTION_REGISTRATION) == 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void ril_query(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_query_cb_t cb,
				void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, cf);

	if (ril_send_forward_cmd(type, cls, NULL, -1, cbd,
					CF_ACTION_INTERROGATE) == 0) {
		CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
		g_free(cbd);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_forwarding *cf = user_data;

	ofono_call_forwarding_register(cf);
	return FALSE;
}

static int ril_call_forwarding_probe(struct ofono_call_forwarding *cf,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct forw_data *fd;

	fd = g_try_new0(struct forw_data, 1);
	if (fd == NULL)
		return -ENOMEM;

	fd->ril = g_ril_clone(ril);
	ofono_call_forwarding_set_data(cf, fd);

	/*
	 * ofono_call_forwarding_register() needs to be called after
	 * the driver has been set in ofono_call_forwarding_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle event instead.
	 */
	g_idle_add(ril_delayed_register, cf);

	return 0;
}

static void ril_call_forwarding_remove(struct ofono_call_forwarding *cf)
{
	struct forw_data *data = ofono_call_forwarding_get_data(cf);
	ofono_call_forwarding_set_data(cf, NULL);

	g_ril_unref(data->ril);
	g_free(data);
}

static struct ofono_call_forwarding_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_call_forwarding_probe,
	.remove			= ril_call_forwarding_remove,
	.erasure		= ril_erasure,
	.deactivation		= ril_deactivate,
	.query			= ril_query,
	.registration		= ril_registration,
	.activation		= ril_activate
};

void ril_call_forwarding_init(void)
{
	ofono_call_forwarding_driver_register(&driver);
}

void ril_call_forwarding_exit(void)
{
	ofono_call_forwarding_driver_unregister(&driver);
}
