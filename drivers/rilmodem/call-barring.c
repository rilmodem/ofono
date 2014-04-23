/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014 Jolla Ltd
 *  Contact: Miia Leinonen
 *  Copyright (C) 2014  Canonical Ltd
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

#include <errno.h>
#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>

#include "common.h"
#include "grilrequest.h"
#include "grilreply.h"
#include "call-barring.h"
#include "rilmodem.h"
#include "ril_constants.h"

struct barring_data {
	GRil *ril;
};

static void ril_call_barring_query_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_query_cb_t cb = cbd->cb;
	struct barring_data *bd = cbd->user;
	int bearer_class;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: query failed, err: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	bearer_class = g_ril_reply_parse_query_facility_lock(bd->ril, message);
	if (bearer_class < 0)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, bearer_class, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_call_barring_query(struct ofono_call_barring *cb,
					const char *lock, int cls,
					ofono_call_barring_query_cb_t callback,
					void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data, bd);
	struct parcel rilp;

	DBG("lock: %s, services to query: %d", lock, cls);

	/*
	 * RIL modems do not support 7 as default bearer class. According to
	 * TS 22.030 Annex C: When service code is not given it corresponds to
	 * "All tele and bearer services"
	 */
	if (cls == BEARER_CLASS_DEFAULT)
		cls = SERVICE_CLASS_NONE;

	/* ril.h: password should be empty string "" when not needed */
	g_ril_request_query_facility_lock(bd->ril, lock, "", cls, &rilp);

	if (g_ril_send(bd->ril, RIL_REQUEST_QUERY_FACILITY_LOCK, &rilp,
				ril_call_barring_query_cb, cbd, g_free) <= 0) {
		ofono_error("%s: sending failed", __func__);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(callback, -1, data);
	}
}

static void ril_call_barring_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_set_cb_t cb = cbd->cb;
	struct barring_data *bd = cbd->user;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: set failed, err: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	/* Just for printing return value */
	g_ril_reply_parse_set_facility_lock(bd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_call_barring_set(struct ofono_call_barring *cb,
				const char *lock, int enable,
				const char *passwd, int cls,
				ofono_call_barring_set_cb_t callback,
				void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data, bd);
	struct parcel rilp;

	DBG("lock: %s, enable: %d, bearer class: %d", lock, enable, cls);

	/*
	 * RIL modem does not support 7 as default bearer class. According to
	 * the 22.030 Annex C: When service code is not given it corresponds to
	 * "All tele and bearer services"
	 */
	if (cls == BEARER_CLASS_DEFAULT)
		cls = SERVICE_CLASS_NONE;

	g_ril_request_set_facility_lock(bd->ril, lock, enable,
					passwd, cls, &rilp);

	if (g_ril_send(bd->ril, RIL_REQUEST_SET_FACILITY_LOCK, &rilp,
			ril_call_barring_set_cb, cbd, g_free) <= 0) {
		ofono_error("%s: sending failed", __func__);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(callback, data);
	}
}

static void ril_call_barring_set_passwd_cb(struct ril_msg *message,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_set_cb_t cb = cbd->cb;
	struct barring_data *bd = cbd->user;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: set password failed, err: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	g_ril_print_response_no_args(bd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_call_barring_set_passwd(struct ofono_call_barring *barr,
					const char *lock,
					const char *old_passwd,
					const char *new_passwd,
					ofono_call_barring_set_cb_t cb,
					void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(barr);
	struct cb_data *cbd = cb_data_new(cb, data, bd);
	struct parcel rilp;

	DBG("lock %s old %s new %s", lock, old_passwd, new_passwd);

	g_ril_request_change_barring_password(bd->ril, lock, old_passwd,
						new_passwd, &rilp);

	if (g_ril_send(bd->ril, RIL_REQUEST_CHANGE_BARRING_PASSWORD, &rilp,
			ril_call_barring_set_passwd_cb, cbd, g_free) <= 0) {
		ofono_error("%s: sending failed", __func__);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_barring *cb = user_data;

	ofono_call_barring_register(cb);
	return FALSE;
}

static int ril_call_barring_probe(struct ofono_call_barring *cb,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct barring_data *bd = g_try_new0(struct barring_data, 1);
	if (bd == NULL)
		return -ENOMEM;

	bd->ril = g_ril_clone(ril);
	ofono_call_barring_set_data(cb, bd);

	g_idle_add(ril_delayed_register, cb);

	return 0;
}

static void ril_call_barring_remove(struct ofono_call_barring *cb)
{
	struct barring_data *data = ofono_call_barring_get_data(cb);
	ofono_call_barring_set_data(cb, NULL);

	g_ril_unref(data->ril);
	g_free(data);
}

static struct ofono_call_barring_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_call_barring_probe,
	.remove			= ril_call_barring_remove,
	.query			= ril_call_barring_query,
	.set			= ril_call_barring_set,
	.set_passwd		= ril_call_barring_set_passwd
};

void ril_call_barring_init(void)
{
	ofono_call_barring_driver_register(&driver);
}

void ril_call_barring_exit(void)
{
	ofono_call_barring_driver_unregister(&driver);
}
