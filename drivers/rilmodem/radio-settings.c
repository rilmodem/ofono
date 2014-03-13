/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
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
#include <ofono/radio-settings.h>

#include "gril.h"

#include "rilmodem.h"

#include "grilrequest.h"
#include "grilreply.h"

struct radio_data {
	GRil *ril;
};

static void ril_set_rat_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: rat mode setting failed", __func__);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_set_rat_mode(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct parcel rilp;
	int pref = PREF_NET_TYPE_GSM_WCDMA;

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		pref = PREF_NET_TYPE_LTE_GSM_WCDMA;
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		pref = PREF_NET_TYPE_GSM_ONLY;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		pref = PREF_NET_TYPE_GSM_WCDMA;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		pref = PREF_NET_TYPE_LTE_GSM_WCDMA;
		break;
	}

	g_ril_request_set_preferred_network_type(rd->ril, pref, &rilp);

	if (g_ril_send(rd->ril, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
				&rilp, ril_set_rat_cb, cbd, g_free) == 0) {
		ofono_error("%s: unable to set rat mode", __func__);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_rat_mode_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	int mode, pref;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	pref = g_ril_reply_parse_get_preferred_network_type(rd->ril, message);
	if (pref < 0) {
		ofono_error("%s: parse error", __func__);
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	switch (pref) {
	case PREF_NET_TYPE_GSM_WCDMA:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	case PREF_NET_TYPE_GSM_ONLY:
		mode = OFONO_RADIO_ACCESS_MODE_GSM;
		break;
	case PREF_NET_TYPE_LTE_GSM_WCDMA:
		mode = OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	default:
		ofono_error("%s: Unexpected preferred network type (%d)",
				__func__, pref);
		mode = OFONO_RADIO_ACCESS_MODE_ANY;
		break;
	}

	CALLBACK_WITH_SUCCESS(cb, mode, cbd->data);
}

static void ril_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);

	if (g_ril_send(rd->ril, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
				NULL, ril_rat_mode_cb, cbd, g_free) == 0) {
		ofono_error("%s: unable to send rat mode query", __func__);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;

	ofono_radio_settings_register(rs);
	return FALSE;
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
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
	g_idle_add(ril_delayed_register, rs);

	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_set_data(rs, NULL);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static struct ofono_radio_settings_driver driver = {
	.name		= RILMODEM,
	.probe		= ril_radio_settings_probe,
	.remove		= ril_radio_settings_remove,
	.query_rat_mode	= ril_query_rat_mode,
	.set_rat_mode	= ril_set_rat_mode,
};

void ril_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void ril_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
