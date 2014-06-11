/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014 Canonical Ltd
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
#include <ofono/powerd.h>
#include <smsutil.h>
#include <util.h>

#include "gril.h"
#include "grilutil.h"
#include "grilrequest.h"
#include "grilunsol.h"

#include "rilmodem.h"

#include "ril_constants.h"
#include "powerd.h"

static void ril_display_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_powerd *powerd = cbd->user;
	struct powerd_data *pwrd = ofono_powerd_get_data(powerd);
	ofono_powerd_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(pwrd->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

void ril_powerd_set_display_state(struct ofono_powerd *powerd, gboolean on,
					ofono_powerd_cb_t cb, void *user_data)
{
	struct powerd_data *pwrd = ofono_powerd_get_data(powerd);
	struct cb_data *cbd = cb_data_new(cb, user_data, powerd);
	struct parcel rilp;

	g_ril_request_screen_state(pwrd->ril, on, &rilp);

	if (g_ril_send(pwrd->ril, RIL_REQUEST_SCREEN_STATE, &rilp,
			ril_display_state_cb, cbd, g_free) <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, user_data);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_powerd *powerd = user_data;

	DBG("");

	ofono_powerd_register(powerd);

	return FALSE;
}

int ril_powerd_probe(struct ofono_powerd *powerd,
			unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct powerd_data *pwrd = g_new0(struct powerd_data, 1);

	pwrd->ril = g_ril_clone(ril);
	ofono_powerd_set_data(powerd, pwrd);

	g_idle_add(ril_delayed_register, powerd);

	return 0;
}

void ril_powerd_remove(struct ofono_powerd *powerd)
{
	struct powerd_data *pwrd = ofono_powerd_get_data(powerd);
	ofono_powerd_set_data(powerd, NULL);

	g_ril_unref(pwrd->ril);
	g_free(pwrd);
}

static struct ofono_powerd_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_powerd_probe,
	.remove			= ril_powerd_remove,
	.set_display_state	= ril_powerd_set_display_state
};

void ril_powerd_init(void)
{
	ofono_powerd_driver_register(&driver);
}

void ril_powerd_exit(void)
{
	ofono_powerd_driver_unregister(&driver);
}
