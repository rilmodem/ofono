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
#include <stdint.h>

#include <glib.h>

#include <ofono.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gril.h"

#include "rilutil.h"
#include "rilmodem.h"

#include "grilrequest.h"
#include "grilreply.h"
#include "radio-settings.h"

struct radio_data *radio_data_0;
struct radio_data *radio_data_1;

static int g_session;

static struct radio_data *radio_data_complement(struct radio_data *rd)
{
	if (rd == radio_data_0)
		return radio_data_1;
	else
		return radio_data_0;
}

static void set_ia_apn_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: reply failure: %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	g_ril_print_response_no_args(rd->ril, message);
}

static void set_ia_apn(struct ofono_radio_settings *rs)
{
	char mccmnc[OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 1];
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct parcel rilp;
	struct ofono_gprs *gprs;
	const struct ofono_gprs_primary_context *ia_ctx;

	if ((rd->available_rats & OFONO_RADIO_ACCESS_MODE_LTE) == 0)
		return;

	gprs = __ofono_atom_find(OFONO_ATOM_TYPE_GPRS, rd->modem);
	if (gprs == NULL)
		return;

	/* Ask for APN data */
	ia_ctx = ofono_gprs_get_ia_apn(gprs, mccmnc);
	if (ia_ctx == NULL)
		return;

	g_ril_request_set_initial_attach_apn(rd->ril, ia_ctx->apn,
						ia_ctx->proto, ia_ctx->username,
						ia_ctx->password, mccmnc,
						&rilp);

	if (g_ril_send(rd->ril, RIL_REQUEST_SET_INITIAL_ATTACH_APN,
			&rilp, set_ia_apn_cb, rs, NULL) == 0)
		ofono_error("%s: failure sending request", __func__);
}

static void ril_set_rat_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		rd->rat_mode = rd->pending_mode;

		g_ril_print_response_no_args(rd->ril, message);

		if (rd->rat_mode == OFONO_RADIO_ACCESS_MODE_LTE)
			set_ia_apn(rs);

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: rat mode setting failed", __func__);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void set_preferred_network(struct radio_data *rd, struct cb_data *cbd,
					enum ofono_radio_access_mode mode)
{
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
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

	rd->pending_mode = mode;

	g_ril_request_set_preferred_network_type(rd->ril, pref, &rilp);

	if (g_ril_send(rd->ril, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
				&rilp, ril_set_rat_cb, cbd, g_free) == 0) {
		ofono_error("%s: unable to set rat mode", __func__);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static gboolean send_set_radio_cap(struct radio_data *rd,
					int session, int phase, int ril_rats,
					const char *logical_modem, int status,
					GRilResponseFunc cb)
{
	struct parcel rilp;
	int version = 1;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, version);
	parcel_w_int32(&rilp, session);
	parcel_w_int32(&rilp, phase);
	parcel_w_int32(&rilp, ril_rats);
	parcel_w_string(&rilp, logical_modem);
	parcel_w_int32(&rilp, status);

	g_ril_append_print_buf(rd->ril, "(%d,%d,%d,0x%X,%s,%d)", version,
			session, phase, ril_rats, logical_modem, status);

	if (g_ril_send(rd->ril, RIL_REQUEST_SET_RADIO_CAPABILITY,
						&rilp, cb, rd, NULL) == 0)
		return FALSE;

	return TRUE;
}

static unsigned set_rat_from_ril_rat(int ril_rat)
{
	unsigned rat = 0;

	if (ril_rat & RIL_RAF_GSM)
		rat |= OFONO_RADIO_ACCESS_MODE_GSM;

	if (ril_rat & (RIL_RAF_UMTS | RIL_RAF_TD_SCDMA))
		rat |= OFONO_RADIO_ACCESS_MODE_UMTS;

	if (ril_rat & RIL_RAF_LTE)
		rat |= OFONO_RADIO_ACCESS_MODE_LTE;

	return rat;
}

static void set_preferred_cb(const struct ofono_error *error, void *data)
{
	struct radio_data *rd = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("%s: error setting radio access mode", __func__);

		return;
	}

	ofono_radio_settings_set_rat_mode(rd->radio_settings, rd->rat_mode);
}

static enum ofono_radio_access_mode
				get_best_available_tech(unsigned available_rats)
{
	int i;
	uint32_t tech;

	for (i = sizeof(uint32_t) * CHAR_BIT; i > 0; i--) {
		tech = 1 << (i - 1);

		if ((available_rats & tech) != 0)
			break;
	}

	if (i == 0)
		tech = OFONO_RADIO_ACCESS_MODE_GSM;

	return tech;
}

static void switch_finish_cb(struct ril_msg *message, gpointer user_data)
{
	struct radio_data *rd = user_data;
	struct switch_data *sd = rd->switch_d;
	struct radio_data *rd1 = sd->rd_1;
	struct radio_data *rd2 = sd->rd_2;
	struct reply_radio_capability *caps;

	sd->pending_msgs--;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	caps = g_ril_reply_parse_get_radio_capability(rd->ril, message);
	if (caps == NULL) {
		ofono_error("%s: parse error", __func__);
		return;
	}

	if (sd->pending_msgs != 0)
		return;

	ofono_info("Switching radio caps between slots - FINISH");

	set_preferred_network(rd1, sd->cbd, sd->mode_to_switch);

	/*
	 * If the complementary slot does not support anymore its current
	 * technology, we change it to the best possible among available ones.
	 */
	if ((rd2->rat_mode & rd2->available_rats) == 0) {

		struct cb_data *cbd =
			cb_data_new(set_preferred_cb, rd2, rd2->radio_settings);

		set_preferred_network(rd2, cbd,
				get_best_available_tech(rd2->available_rats));
	}

	rd1->switch_d = NULL;
	rd2->switch_d = NULL;
	g_free(sd);
	g_free(caps);
}

static void radio_caps_event(struct ril_msg *message, gpointer user_data)
{
	struct radio_data *rd = user_data;
	struct switch_data *sd = rd->switch_d;
	struct radio_data *rd1;
	struct radio_data *rd2;
	struct reply_radio_capability *caps;

	if (sd == NULL)
		return;

	rd1 = sd->rd_1;
	rd2 = sd->rd_2;

	caps = g_ril_reply_parse_get_radio_capability(rd->ril, message);
	if (caps == NULL) {
		ofono_error("%s: parse error", __func__);
		return;
	}

	/*
	 * Update rats. They come also in the replies to SET_RADIO_CAPABILITY,
	 * but those seem to be unreliable, at least for midori.
	 */
	rd->ril_rats = caps->rat;
	rd->available_rats = set_rat_from_ril_rat(caps->rat);

	strcpy(rd->modem_uuid, caps->modem_uuid);

	sd->pending_msgs--;

	if (sd->pending_msgs != 0)
		return;

	DBG("Sending requests for FINISH phase");

	send_set_radio_cap(rd1, g_session, RIL_RC_PHASE_FINISH,
				rd1->ril_rats, rd1->modem_uuid,
				RIL_RC_STATUS_SUCCESS, switch_finish_cb);
	send_set_radio_cap(rd2, g_session, RIL_RC_PHASE_FINISH,
				rd2->ril_rats, rd2->modem_uuid,
				RIL_RC_STATUS_SUCCESS, switch_finish_cb);
	sd->pending_msgs = 2;

	g_free(caps);
}

/*
 * This function is just for completeness, as we actually need to wait for the
 * unsolocited events to continue the capabilities switch.
 */
static void switch_apply_cb(struct ril_msg *message, gpointer user_data)
{
	struct radio_data *rd = user_data;
	struct reply_radio_capability *caps;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	caps = g_ril_reply_parse_get_radio_capability(rd->ril, message);
	if (caps == NULL)
		ofono_error("%s: parse error", __func__);

	g_free(caps);
}

static void switch_start_cb(struct ril_msg *message, gpointer user_data)
{
	struct radio_data *rd = user_data;
	struct switch_data *sd = rd->switch_d;
	struct radio_data *rd1 = sd->rd_1;
	struct radio_data *rd2 = sd->rd_2;
	struct reply_radio_capability *caps;

	sd->pending_msgs--;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	caps = g_ril_reply_parse_get_radio_capability(rd->ril, message);
	if (caps == NULL) {
		ofono_error("%s: parse error", __func__);
		return;
	}

	if (sd->pending_msgs != 0)
		return;

	DBG("Sending requests for APPLY phase");

	send_set_radio_cap(rd1, g_session, RIL_RC_PHASE_APPLY,
				rd2->ril_rats, rd2->modem_uuid,
				RIL_RC_STATUS_NONE, switch_apply_cb);
	send_set_radio_cap(rd2, g_session, RIL_RC_PHASE_APPLY,
				rd1->ril_rats, rd1->modem_uuid,
				RIL_RC_STATUS_NONE, switch_apply_cb);
	sd->pending_msgs = 2;

	g_free(caps);
}

static void switch_caps(struct switch_data *sd)
{
	struct radio_data *rd1 = sd->rd_1;
	struct radio_data *rd2 = sd->rd_2;

	/* START phase */
	g_session++;

	send_set_radio_cap(rd1, g_session, RIL_RC_PHASE_START,
				rd1->ril_rats, rd1->modem_uuid,
				RIL_RC_STATUS_NONE, switch_start_cb);
	send_set_radio_cap(rd2, g_session, RIL_RC_PHASE_START,
				rd2->ril_rats, rd2->modem_uuid,
				RIL_RC_STATUS_NONE, switch_start_cb);
	sd->pending_msgs = 2;
}

static void get_rs_with_mode(struct ofono_modem *modem, void *data)
{
	struct switch_data *sd = data;
	struct radio_data *rd_ref = sd->rd_1;
	struct ofono_atom *atom;
	struct ofono_radio_settings *rs;
	struct radio_data *rd;
	const char *standby_group, *modem_group;

	atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_RADIO_SETTINGS);
	if (atom == NULL)
		return;

	rs = __ofono_atom_get_data(atom);
	rd = ofono_radio_settings_get_data(rs);
	if (rd == rd_ref)
		return;

	standby_group = ofono_modem_get_string(rd_ref->modem, "StandbyGroup");
	if (standby_group == NULL)
		return;

	modem_group = ofono_modem_get_string(modem, "StandbyGroup");
	if (g_strcmp0(standby_group, modem_group) != 0)
		return;

	if ((rd->available_rats & sd->mode_to_switch) == 0)
		return;

	sd->rd_2 = rd;
}

void ril_set_rat_mode(struct ofono_radio_settings *rs,
			enum ofono_radio_access_mode mode,
			ofono_radio_settings_rat_mode_set_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct switch_data *sd = NULL;

	if (rd->switch_d != NULL)
		goto error;

	if ((rd->available_rats & mode) == 0) {
		if (g_ril_get_version(rd->ril) < 11)
			goto error;

		/* Check if we can switch rats with other slot */
		sd = g_malloc0(sizeof (*sd));
		sd->rd_1 = rd;
		sd->mode_to_switch = mode;
		sd->cbd = cbd;

		__ofono_modem_foreach(get_rs_with_mode, sd);

		if (sd->rd_2 == NULL)
			goto error;

		ofono_info("Switching radio caps between slots - START");
		sd->rd_1->switch_d = sd;
		sd->rd_2->switch_d = sd;

		switch_caps(sd);
	} else {
		set_preferred_network(rd, cbd, mode);
	}

	return;

error:
	ofono_error("%s: unable to set rat mode", __func__);
	g_free(sd);
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
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
		goto error;
	}

	pref = g_ril_reply_parse_get_preferred_network_type(rd->ril, message);
	if (pref < 0) {
		ofono_error("%s: parse error", __func__);
		goto error;
	}

	/*
	 * GSM_WCDMA_AUTO -> ril.h: GSM/WCDMA (auto mode, according to PRL)
	 * PRL: preferred roaming list.
	 * This value is returned when selecting the slot as having 3G
	 * capabilities, so it is sort of the default for MTK modems.
	 */

	switch (pref) {
	case PREF_NET_TYPE_GSM_WCDMA:
	case PREF_NET_TYPE_GSM_WCDMA_AUTO:
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

	rd->rat_mode = mode;

	CALLBACK_WITH_SUCCESS(cb, mode, cbd->data);

	return;

error:
	/*
	 * If error, we assume GSM. This is preferable to not being able to
	 * access the radio settings properties. Midori returns error if we
	 * have not completed successfully a capability switch. This should
	 * not happen if there are no bugs in our implementation, but it is
	 * better to leave this here so system settings shows something that
	 * can be manually changed by the user, just in case.
	 */
	CALLBACK_WITH_SUCCESS(cb, OFONO_RADIO_ACCESS_MODE_GSM, cbd->data);
}

void ril_query_rat_mode(struct ofono_radio_settings *rs,
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

void ril_query_fast_dormancy(struct ofono_radio_settings *rs,
			ofono_radio_settings_fast_dormancy_query_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	CALLBACK_WITH_SUCCESS(cb, rd->fast_dormancy, data);
}

static void ril_display_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_fast_dormancy_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);

		rd->fast_dormancy = rd->pending_fd;

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

void ril_set_fast_dormancy(struct ofono_radio_settings *rs,
				ofono_bool_t enable,
				ofono_radio_settings_fast_dormancy_set_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct parcel rilp;

	g_ril_request_screen_state(rd->ril, enable ? 0 : 1, &rilp);

	rd->pending_fd = enable;

	if (g_ril_send(rd->ril, RIL_REQUEST_SCREEN_STATE, &rilp,
			ril_display_state_cb, cbd, g_free) <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static ofono_bool_t query_available_rats_cb(gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_available_rats_query_cb_t cb = cbd->cb;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	rd->available_rats = OFONO_RADIO_ACCESS_MODE_GSM
						| OFONO_RADIO_ACCESS_MODE_UMTS;

	if (getenv("OFONO_RIL_RAT_LTE") != NULL)
		rd->available_rats |= OFONO_RADIO_ACCESS_MODE_LTE;

	CALLBACK_WITH_SUCCESS(cb, rd->available_rats, cbd->data);

	g_free(cbd);

	return FALSE;
}

static void get_radio_caps_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_available_rats_query_cb_t cb = cbd->cb;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct radio_data *rd_comp;
	struct reply_radio_capability *caps;
	unsigned all_rats;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
		return;
	}

	caps = g_ril_reply_parse_get_radio_capability(rd->ril, message);
	if (caps == NULL) {
		ofono_error("%s: parse error", __func__);
		CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
		return;
	}

	rd->ril_rats = caps->rat;
	rd->available_rats = set_rat_from_ril_rat(caps->rat);

	strcpy(rd->modem_uuid, caps->modem_uuid);

	g_free(caps);

	/* We show all rats, as we can switch the ownership between slots */
	all_rats = rd->available_rats;

	rd_comp = radio_data_complement(rd);
	if (rd_comp != NULL)
		all_rats |= rd_comp->available_rats;

	CALLBACK_WITH_SUCCESS(cb, all_rats, cbd->data);
}

void ril_query_available_rats(struct ofono_radio_settings *rs,
			ofono_radio_settings_available_rats_query_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);

	if (g_ril_get_version(rd->ril) < 11) {
		g_idle_add(query_available_rats_cb, cbd);
		return;
	}

	if (g_ril_send(rd->ril, RIL_REQUEST_GET_RADIO_CAPABILITY, NULL,
					get_radio_caps_cb, cbd, g_free) <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, 0, data);
	}
}

static void gprs_watch_cb(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_radio_settings *rs = data;

	if (cond != OFONO_ATOM_WATCH_CONDITION_REGISTERED)
		return;

	set_ia_apn(rs);
}

static void set_safe_preferred_cb(const struct ofono_error *error, void *data)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("%s: error setting radio access mode", __func__);

		return;
	}
}

static void radio_settings_register(const struct ofono_error *error,
					unsigned int available_rats,
					void *data)
{
	struct ofono_radio_settings *rs = data;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	g_ril_register(rd->ril, RIL_UNSOL_RADIO_CAPABILITY,
							radio_caps_event, rd);

	rd->gprs_atom_watch =
		__ofono_modem_add_atom_watch(rd->modem, OFONO_ATOM_TYPE_GPRS,
						gprs_watch_cb, rs, NULL);

	/*
	 * If the preferred technology was unknown/unsupported, change to a
	 * valid one (midori can return PREF_NET_TYPE_CDMA_ONLY, for instance).
	 */
	if (rd->rat_mode == OFONO_RADIO_ACCESS_MODE_ANY) {
		struct cb_data *cbd = cb_data_new(set_safe_preferred_cb, rd,
							rd->radio_settings);

		set_preferred_network(rd, cbd,
				get_best_available_tech(rd->available_rats));
	}

	/*
	 * We register in all cases, setting FD some times fails until radio is
	 * available (this happens on turbo and maybe in other devices).
	 */
	ofono_radio_settings_register(rs);
}

static void ril_after_query_rat_mode(const struct ofono_error *error,
					enum ofono_radio_access_mode mode,
					void *data)
{
	struct ofono_radio_settings *rs = data;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	rd->virt_tbl->query_available_rats(rs, radio_settings_register, rs);
}

void ril_delayed_register(const struct ofono_error *error, void *user_data)
{
	struct ofono_radio_settings *rs = user_data;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_error("%s: cannot set default fast dormancy", __func__);

	rd->radio_settings = rs;

	if (ofono_modem_get_integer(rd->modem, "Slot") == 0)
		radio_data_0 = rd;
	else
		radio_data_1 = rd;

	rd->virt_tbl->query_rat_mode(rs, ril_after_query_rat_mode, rs);
}

static struct ofono_radio_settings_driver driver;

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user)
{
	struct ril_radio_settings_driver_data *rs_init_data = user;
	struct radio_data *rsd = g_try_new0(struct radio_data, 1);

	if (rsd == NULL) {
		ofono_error("%s: cannot allocate memory", __func__);
		return -ENOMEM;
	}

	rsd->virt_tbl = &driver;
	rsd->ril = g_ril_clone(rs_init_data->gril);
	rsd->modem = rs_init_data->modem;

	ofono_radio_settings_set_data(rs, rsd);

	ril_set_fast_dormancy(rs, FALSE, ril_delayed_register, rs);

	return 0;
}

void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	ofono_radio_settings_set_data(rs, NULL);

	if (rd->gprs_atom_watch)
		__ofono_modem_remove_atom_watch(rd->modem, rd->gprs_atom_watch);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_radio_settings_probe,
	.remove			= ril_radio_settings_remove,
	.query_rat_mode		= ril_query_rat_mode,
	.set_rat_mode		= ril_set_rat_mode,
	.query_fast_dormancy	= ril_query_fast_dormancy,
	.set_fast_dormancy	= ril_set_fast_dormancy,
	.query_available_rats	= ril_query_available_rats
};

void ril_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void ril_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
