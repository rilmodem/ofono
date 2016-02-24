/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Canonical Ltd.
 *  Copyright (C) 2015 Ratchanan Srirattanamet
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
#include <ofono.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>
#include <ofono/sim.h>

#include "gril.h"
#include "grilrequest.h"
#include "grilreply.h"

#include "drivers/rilmodem/radio-settings.h"
#include "drivers/rilmodem/rilutil.h"
#include "qcom_msim_modem.h"
#include "qcom_msim_constants.h"

struct qcom_msim_pending_pref_setting {
	struct ofono_radio_settings *rs;
	int pref;
	int pending_gsm_pref_remaining;
	struct cb_data *cbd;
};

struct qcom_msim_set_2g_rat {
	struct ofono_radio_settings *rs;
	struct qcom_msim_pending_pref_setting *pps;
};

static struct ofono_radio_settings *multisim_rs[QCOMMSIM_NUM_SLOTS_MAX];

static void qcom_msim_set_rat_cb(struct ril_msg *message, gpointer user_data)
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

static void qcom_msim_do_set_rat_mode(struct ofono_radio_settings *rs, int pref,
							struct cb_data *cbd)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct parcel rilp;
	ofono_radio_settings_rat_mode_set_cb_t cb;

	g_ril_request_set_preferred_network_type(rd->ril, pref, &rilp);

	if (g_ril_send(rd->ril, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
			&rilp, qcom_msim_set_rat_cb, cbd, g_free) == 0) {
		ofono_error("%s: unable to set rat mode", __func__);
		cb = cbd->cb;
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void qcom_msim_set_2g_rat_cb(struct ril_msg *message,
							gpointer user_data)
{
	struct qcom_msim_set_2g_rat *set_2g_rat_data = user_data;
	struct ofono_radio_settings *rs = set_2g_rat_data->rs;
	struct qcom_msim_pending_pref_setting *pps = set_2g_rat_data->pps;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_rat_mode_set_cb_t cb;

	pps->pending_gsm_pref_remaining -= 1;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);
		ofono_radio_settings_set_rat_mode(rs,
						OFONO_RADIO_ACCESS_MODE_GSM);
	} else {
		ofono_error("%s: rat mode setting failed", __func__);
		if (pps->cbd != NULL) {
			cb = pps->cbd->cb;
			CALLBACK_WITH_FAILURE(cb, pps->cbd->data);

			g_free(pps->cbd);
			pps->cbd = NULL;
		}
	}

	if (pps->pending_gsm_pref_remaining == 0) {
		if (pps->cbd != NULL)
			qcom_msim_do_set_rat_mode(pps->rs, pps->pref, pps->cbd);

		g_free(pps);
	}
}

static void qcom_msim_set_rat_mode(struct ofono_radio_settings *rs,
			enum ofono_radio_access_mode mode,
			ofono_radio_settings_rat_mode_set_cb_t cb,
			void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct parcel rilp;
	int pref = PREF_NET_TYPE_GSM_WCDMA;
	struct qcom_msim_pending_pref_setting *pps = NULL;

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

	if (pref != PREF_NET_TYPE_GSM_ONLY) {
		int i;
		for (i = 0; i < QCOMMSIM_NUM_SLOTS_MAX; i++) {
			struct radio_data *temp_rd;
			struct qcom_msim_set_2g_rat *set_2g_rat_data;
			struct ofono_atom *sim_atom;
			struct ofono_sim *sim;

			if (multisim_rs[i] == rs || multisim_rs[i] == NULL)
				continue;

			temp_rd = ofono_radio_settings_get_data(multisim_rs[i]);
			sim_atom = __ofono_modem_find_atom(temp_rd->modem,
							OFONO_ATOM_TYPE_SIM);
			if (sim_atom == NULL) {
				if (pps != NULL)
					pps->cbd = NULL;
				g_free(cbd);
				CALLBACK_WITH_FAILURE(cb, data);
				break;
			}

			sim = __ofono_atom_get_data(sim_atom);
			if (ofono_sim_get_state(sim) ==
						OFONO_SIM_STATE_NOT_PRESENT)
				continue;

			if (pps == NULL) {
				pps = g_try_new0(
					struct qcom_msim_pending_pref_setting,
									1);
				pps->rs = rs;
				pps->pref = pref;
				pps->cbd = cbd;
				pps->pending_gsm_pref_remaining = 0;
			}

			set_2g_rat_data =
				g_try_new0(struct qcom_msim_set_2g_rat, 1);
			set_2g_rat_data->pps = pps;
			set_2g_rat_data->rs = multisim_rs[i];

			g_ril_request_set_preferred_network_type(temp_rd->ril,
						PREF_NET_TYPE_GSM_ONLY, &rilp);

			if (g_ril_send(temp_rd->ril,
					RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
					&rilp, qcom_msim_set_2g_rat_cb,
					set_2g_rat_data, g_free) == 0) {
				ofono_error("%s: unable to set rat mode",
								__func__);
				pps->cbd = NULL;
				g_free(cbd);
				g_free(set_2g_rat_data);
				CALLBACK_WITH_FAILURE(cb, data);
				break;
			} else {
				pps->pending_gsm_pref_remaining += 1;
			}
		}
	}

	if (pps && pps->pending_gsm_pref_remaining == 0) {
		g_free(pps);
		pps = NULL;
	}

	if (pps == NULL)
		qcom_msim_do_set_rat_mode(rs, pref, cbd);
}

static int qcom_msim_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user)
{
	struct ril_radio_settings_driver_data *rs_init_data = user;
	struct radio_data *rsd = g_try_new0(struct radio_data, 1);
	int slot_id;

	if (rsd == NULL) {
		ofono_error("%s: cannot allocate memory", __func__);
		return -ENOMEM;
	}

	rsd->ril = g_ril_clone(rs_init_data->gril);
	rsd->modem = rs_init_data->modem;

	ofono_radio_settings_set_data(rs, rsd);

	ril_set_fast_dormancy(rs, FALSE, ril_delayed_register, rs);

	slot_id = ofono_modem_get_integer(rsd->modem, "Slot");
	multisim_rs[slot_id] = rs;

	return 0;
}

static void qcom_msim_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	int slot_id = ofono_modem_get_integer(rd->modem, "Slot");

	multisim_rs[slot_id] = NULL;

	ofono_radio_settings_set_data(rs, NULL);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= QCOMMSIMMODEM,
	.probe			= qcom_msim_radio_settings_probe,
	.remove			= qcom_msim_radio_settings_remove,
	.query_rat_mode		= ril_query_rat_mode,
	.set_rat_mode		= qcom_msim_set_rat_mode,
	.query_fast_dormancy	= ril_query_fast_dormancy,
	.set_fast_dormancy	= ril_set_fast_dormancy,
	.query_available_rats	= ril_query_available_rats
};

void qcom_msim_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void qcom_msim_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
