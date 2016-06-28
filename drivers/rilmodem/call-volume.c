/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013 Canonical Ltd.
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
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-volume.h>

#include "gril.h"
#include "grilutil.h"

#include "common.h"

#include "rilmodem.h"
#include "rilutil.h"
#include "parcel.h"
#include "grilrequest.h"
#include "grilreply.h"

struct cv_data {
	GRil *ril;
	unsigned int vendor;
};

static void volume_mute_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_volume_cb_t cb = cbd->cb;
	struct cv_data *cvd = cbd->user;
	struct ofono_error error;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");

		g_ril_print_response_no_args(cvd->ril, message);

	} else {
		ofono_error("Could not set the ril mute state");
		decode_ril_error(&error, "FAIL");
	}

	cb(&error, cbd->data);
}

static void ril_call_volume_mute(struct ofono_call_volume *cv, int muted,
					ofono_call_volume_cb_t cb, void *data)
{
	struct cv_data *cvd = ofono_call_volume_get_data(cv);
	struct cb_data *cbd = cb_data_new(cb, data, cvd);
	struct parcel rilp;

	DBG("Initial ril muted state: %d", muted);

	g_ril_request_set_mute(cvd->ril, muted, &rilp);

	if (g_ril_send(cvd->ril, RIL_REQUEST_SET_MUTE, &rilp,
			volume_mute_cb, cbd, g_free) == 0) {
		ofono_error("Send RIL_REQUEST_SET_MUTE failed.");
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void probe_mute_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *cvd = ofono_call_volume_get_data(cv);
	int muted;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Could not retrieve the ril mute state");
		return;
	}

	muted = g_ril_reply_parse_get_mute(cvd->ril, message);

	ofono_call_volume_set_muted(cv, muted);
}

static void call_probe_mute(gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *cvd = ofono_call_volume_get_data(cv);

	g_ril_send(cvd->ril, RIL_REQUEST_GET_MUTE, NULL,
			probe_mute_cb, cv, NULL);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	DBG("");
	ofono_call_volume_register(cv);

	/* Probe the mute state */
	call_probe_mute(user_data);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_call_volume_probe(struct ofono_call_volume *cv,
					unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct cv_data *cvd;

	cvd = g_new0(struct cv_data, 1);
	if (cvd == NULL)
		return -ENOMEM;

	cvd->ril = g_ril_clone(ril);
	cvd->vendor = vendor;

	ofono_call_volume_set_data(cv, cvd);

	/*
	 * ofono_call_volume_register() needs to be called after
	 * the driver has been set in ofono_call_volume_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle event instead.
	 */
	g_idle_add(ril_delayed_register, cv);

	return 0;
}

static void ril_call_volume_remove(struct ofono_call_volume *cv)
{
	struct cv_data *cvd = ofono_call_volume_get_data(cv);

	ofono_call_volume_set_data(cv, NULL);

	g_ril_unref(cvd->ril);
	g_free(cvd);
}

static struct ofono_call_volume_driver driver = {
	.name = RILMODEM,
	.probe = ril_call_volume_probe,
	.remove = ril_call_volume_remove,
	.mute = ril_call_volume_mute,
};

void ril_call_volume_init(void)
{
	ofono_call_volume_driver_register(&driver);
}

void ril_call_volume_exit(void)
{
	ofono_call_volume_driver_unregister(&driver);
}
