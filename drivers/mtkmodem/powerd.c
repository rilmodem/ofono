/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/powerd.h>

#include "mtk_constants.h"
#include "mtkunsol.h"
#include "mtkrequest.h"

#include "common.h"
#include "mtkmodem.h"
#include "drivers/rilmodem/powerd.h"
#include "drivers/rilmodem/rilutil.h"

/*
 * This is the powerd atom implementation for mtkmodem. Most of the
 * implementation can be found in the rilmodem atom. The main reason for
 * creating a new atom is the need to handle specific MTK requests.
 */

struct set_fd_mode {
	struct ofono_powerd *power;
	gboolean on;
};

static void mtk_set_fd_mode_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct set_fd_mode *user = cbd->user;
	struct ofono_powerd *powerd = user->power;
	struct powerd_data *pwrd = ofono_powerd_get_data(powerd);
	ofono_powerd_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(pwrd->ril, message);
		ril_powerd_set_display_state(powerd, user->on, cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}

	g_free(user);
}

static void mtk_powerd_set_display_state(struct ofono_powerd *powerd,
					gboolean on, ofono_powerd_cb_t cb,
					void *data)
{
	struct powerd_data *pwrd = ofono_powerd_get_data(powerd);
	struct set_fd_mode *user = g_malloc0(sizeof(*user));
	struct cb_data *cbd;
	struct parcel rilp;

	user->power = powerd;
	user->on = on;

	cbd = cb_data_new(cb, data, user);

	g_mtk_request_set_fd_mode(pwrd->ril, MTK_FD_MODE_SCREEN_STATUS,
					on, 0, &rilp);

	if (g_ril_send(pwrd->ril, RIL_REQUEST_SET_FD_MODE, &rilp,
			mtk_set_fd_mode_cb, cbd, g_free) <= 0) {
		g_free(cbd);
		g_free(user);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static struct ofono_powerd_driver driver = {
	.name			= MTKMODEM,
	.probe			= ril_powerd_probe,
	.remove			= ril_powerd_remove,
	.set_display_state	= mtk_powerd_set_display_state
};

void mtk_powerd_init(void)
{
	ofono_powerd_driver_register(&driver);
}

void mtk_powerd_exit(void)
{
	ofono_powerd_driver_unregister(&driver);
}
