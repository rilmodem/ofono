/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Canonical Ltd.
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
#include <ofono/radio-settings.h>

#include "mtk_constants.h"
#include "mtkunsol.h"
#include "mtkreply.h"
#include "mtkrequest.h"

#include "common.h"
#include "mtkmodem.h"
#include "drivers/rilmodem/radio-settings.h"
#include "drivers/rilmodem/rilutil.h"

/*
 * This is the radio settings atom implementation for mtkmodem. Most of the
 * implementation can be found in the rilmodem atom. The main reason for
 * creating a new atom is the need to handle specific MTK requests.
 */

struct set_fd_mode {
	struct ofono_radio_settings *rst;
	gboolean on;
};

static void mtk_set_fd_mode_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct set_fd_mode *user = cbd->user;
	struct ofono_radio_settings *rs = user->rst;
	struct radio_data *rsd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_fast_dormancy_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rsd->ril, message);
		ril_set_fast_dormancy(rs, user->on, cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}

	g_free(user);
}

static void mtk_set_fast_dormancy(struct ofono_radio_settings *rs,
				ofono_bool_t enable,
				ofono_radio_settings_fast_dormancy_set_cb_t cb,
				void *data)
{
	struct radio_data *rsd = ofono_radio_settings_get_data(rs);
	struct set_fd_mode *user = g_malloc0(sizeof(*user));
	struct cb_data *cbd;
	struct parcel rilp;

	user->rst = rs;
	user->on = enable;

	cbd = cb_data_new(cb, data, user);

	g_mtk_request_set_fd_mode(rsd->ril, MTK_FD_MODE_SCREEN_STATUS,
					enable ? FALSE : TRUE, 0, &rilp);

	if (g_ril_send(rsd->ril, MTK_RIL_REQUEST_SET_FD_MODE, &rilp,
			mtk_set_fd_mode_cb, cbd, g_free) <= 0) {
		g_free(cbd);
		g_free(user);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static int mtk_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct radio_data *rsd = g_try_new0(struct radio_data, 1);

	if (rsd == NULL) {
		ofono_error("%s: cannot allocate memory", __func__);
		return -ENOMEM;
	}

	rsd->ril = g_ril_clone(ril);

	ofono_radio_settings_set_data(rs, rsd);

	mtk_set_fast_dormancy(rs, FALSE, ril_delayed_register, rs);

	return 0;
}

static void mtk_query_modem_rats_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_modem_rats_query_cb_t cb = cbd->cb;
	ofono_bool_t modem_rats[OFONO_RADIO_ACCESS_MODE_LAST] = { FALSE };
	int is_3g;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	is_3g = g_mtk_reply_parse_get_3g_capability(rd->ril, message);
	if (is_3g < 0) {
		ofono_error("%s: parse error", __func__);
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	modem_rats[OFONO_RADIO_ACCESS_MODE_GSM] = TRUE;

	if (is_3g) {
		modem_rats[OFONO_RADIO_ACCESS_MODE_UMTS] = TRUE;

		if (getenv("OFONO_RIL_RAT_LTE"))
			modem_rats[OFONO_RADIO_ACCESS_MODE_LTE] = TRUE;
	}

	CALLBACK_WITH_SUCCESS(cb, modem_rats, cbd->data);
}

static void mtk_query_modem_rats(struct ofono_radio_settings *rs,
				ofono_radio_settings_modem_rats_query_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);

	if (g_ril_send(rd->ril, MTK_RIL_REQUEST_GET_3G_CAPABILITY, NULL,
			mtk_query_modem_rats_cb, cbd, g_free) <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static struct ofono_radio_settings_driver driver = {
	.name			= MTKMODEM,
	.probe			= mtk_radio_settings_probe,
	.remove			= ril_radio_settings_remove,
	.query_rat_mode		= ril_query_rat_mode,
	.set_rat_mode		= ril_set_rat_mode,
	.query_fast_dormancy	= ril_query_fast_dormancy,
	.set_fast_dormancy	= mtk_set_fast_dormancy,
	.query_modem_rats	= mtk_query_modem_rats
};

void mtk_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void mtk_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
