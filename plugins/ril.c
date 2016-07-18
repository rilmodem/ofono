/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014 Canonical Ltd.
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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/phonebook.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/sms.h>
#include <ofono/cbs.h>
#include <ofono/sim.h>
#include <ofono/ussd.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/call-meter.h>
#include <ofono/call-volume.h>
#include <ofono/radio-settings.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/audio-settings.h>
#include <ofono/types.h>

#include "ofono.h"

#include <grilreply.h>
#include <grilrequest.h>
#include <grilunsol.h>

#include "ril.h"
#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/rilutil.h"
#include "drivers/rilmodem/vendor.h"
#include "drivers/qcommsimmodem/qcom_msim_modem.h"

#define MULTISIM_SLOT_0 0
#define MULTISIM_SLOT_1 1

#define MAX_SIM_STATUS_RETRIES 15

/* this gives 30s for rild to initialize */
#define RILD_MAX_CONNECT_RETRIES 5
#define RILD_CONNECT_RETRY_TIME_S 5

struct ril_data {
	GRil *ril;
	enum ofono_ril_vendor vendor;
	int sim_status_retries;
	ofono_bool_t init_state;
	ofono_bool_t ofono_online;
	int radio_state;
	struct ofono_sim *sim;
	int rild_connect_retries;
	GRilMsgIdToStrFunc request_id_to_string;
	GRilMsgIdToStrFunc unsol_request_to_string;
	ril_get_driver_type_func get_driver_type;
	struct cb_data *set_online_cbd;
};

/*
 * Some times we need to access slot B from slot A in dual-SIM modems, so we
 * need these global variables.
 */
static struct ril_data *ril_data_0;
static struct ril_data *ril_data_1;

/* Get complementary GRil */
GRil *ril_get_gril_complement(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	if (rd == ril_data_0 && ril_data_1 != NULL)
		return ril_data_1->ril;
	else if (rd == ril_data_1 && ril_data_0 != NULL)
		return ril_data_0->ril;

	return NULL;
}

static void ril_debug(const char *str, void *user_data)
{
	struct ril_data *rd = user_data;

	ofono_info("Device %d: %s", g_ril_get_slot(rd->ril), str);
}

static const char *get_driver_type(struct ril_data *rd,
					enum ofono_atom_type atom)
{
	if (rd->get_driver_type != NULL)
		return rd->get_driver_type(atom);

	return RILMODEM;
}

/*
 * oFono moves to "Online" state only when told to online the modem AND there is
 * a SIM a card. However, we want to have RadioSettings even when there is no
 * SIM, but we also want it *only* when we are online. Normally, ofono atoms are
 * created/destroyed when the ofono state changes, but for this atom the ofono
 * states do not fit, as we do never move from Offline state if there is no SIM.
 * Therefore, we handle this atom on modem onlining/offlining.
 */
static void manage_radio_settings_atom(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ofono_radio_settings *rs;

	rs = __ofono_atom_find(OFONO_ATOM_TYPE_RADIO_SETTINGS, modem);

	if (rd->ofono_online && rs == NULL) {
		struct ril_radio_settings_driver_data rs_data =
							{ rd->ril, modem };

		ofono_radio_settings_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_RADIO_SETTINGS),
			&rs_data);
	} else if(!rd->ofono_online && rs != NULL) {
		ofono_radio_settings_remove(rs);
	}
}

static void ril_send_power(struct ril_data *rd, ofono_bool_t online,
				GRilResponseFunc func, gpointer user_data)
{
	struct parcel rilp;

	DBG("(online = 1, offline = 0)): %d", online);

	g_ril_request_power(rd->ril, (const gboolean) online, &rilp);

	if (g_ril_send(rd->ril, RIL_REQUEST_RADIO_POWER, &rilp,
			func, user_data, NULL) == 0 && func != NULL) {
		ofono_error("%s: could not set radio to %d", __func__, online);
		func(NULL, user_data);
	}
}

static void ril_set_powered_off_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	if (message == NULL)
		return;

	if (message->error == RIL_E_SUCCESS)
		g_ril_print_response_no_args(rd->ril, message);
	else
		ofono_error("%s: RIL error %s", __func__,
					ril_error_to_string(message->error));
}

static void ril_radio_state_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	int radio_state = g_ril_unsol_parse_radio_state_changed(rd->ril,
								message);

	ofono_info("%s: state: %s, init: %d, rd->ofono_online: %d",
			__func__, ril_radio_state_to_string(radio_state),
			rd->init_state, rd->ofono_online);

	rd->radio_state = radio_state;

	/*
	 * Before showing the modem as powered we make sure the radio is off so
	 * we start in a sane state (as in AOSP). Note that we must always
	 * receive an event with the current radio state on initialization
	 * and also that power state has changed effectively when the event with
	 * the new radio state has been received (we cannot rely on the reply to
	 * RIL_REQUEST_RADIO_POWER). Finally, note that powering off on start is
	 * a must for turbo devices (otherwise radio state never moves from
	 * unavailable).
	 */
	if (rd->init_state) {
		if (radio_state != RADIO_STATE_OFF) {
			DBG("powering off radio on init");

			ril_send_power(rd, FALSE, ril_set_powered_off_cb, modem);
		} else {
			DBG("calling set_powered(TRUE)");
			rd->init_state = FALSE;

			/* Note that modem hw is powered, but radio is off */
			ofono_modem_set_powered(modem, TRUE);
		}
		return;
	}

	/* We process pending callbacks */
	if (rd->set_online_cbd != NULL && (
			(rd->ofono_online && radio_state == RADIO_STATE_ON) ||
			(!rd->ofono_online && radio_state == RADIO_STATE_OFF))
			) {
		ofono_modem_online_cb_t cb = rd->set_online_cbd->cb;

		DBG("%s: set_online OK: rd->ofono_online: %d",
						__func__, rd->ofono_online);
		manage_radio_settings_atom(modem);

		CALLBACK_WITH_SUCCESS(cb, rd->set_online_cbd->data);

		g_free(rd->set_online_cbd);
		rd->set_online_cbd = NULL;
	}

	if ((radio_state == RADIO_STATE_UNAVAILABLE ||
				radio_state == RADIO_STATE_OFF)
			&& rd->ofono_online
			&& rd->vendor != OFONO_RIL_VENDOR_MTK2) {
		/*
		 * Unexpected radio state change, as we are supposed to
		 * be online. UNAVAILABLE has been seen occassionally
		 * when powering off the phone. We wait 5 secs to avoid
		 * too fast re-spawns, then exit with error to make
		 * upstart re-start ofono. In midori we receive an OFF
		 * event and immediately after that an ON event when we
		 * enter the SIM PIN, so do nothing for that device.
		 */
		ofono_error("%s: radio self-powered off!", __func__);
		sleep(5);
		exit(1);
	}
}

int ril_create(struct ofono_modem *modem, enum ofono_ril_vendor vendor,
		GRilMsgIdToStrFunc request_id_to_string,
		GRilMsgIdToStrFunc unsol_request_to_string,
		ril_get_driver_type_func get_driver_type)
{
	struct ril_data *rd = g_try_new0(struct ril_data, 1);
	if (rd == NULL) {
		errno = ENOMEM;
		goto error;
	}

	DBG("");

	rd->vendor = vendor;
	rd->ofono_online = FALSE;
	rd->radio_state = RADIO_STATE_UNAVAILABLE;
	rd->request_id_to_string = request_id_to_string;
	rd->unsol_request_to_string = unsol_request_to_string;
	rd->get_driver_type = get_driver_type;
	rd->init_state = TRUE;

	ofono_modem_set_data(modem, rd);

	return 0;

error:
	g_free(rd);

	return -errno;
}

static int ril_probe(struct ofono_modem *modem)
{
	return ril_create(modem, OFONO_RIL_VENDOR_AOSP, NULL, NULL, NULL);
}

void ril_remove(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_modem_set_data(modem, NULL);

	if (!rd)
		return;

	g_ril_unref(rd->ril);

	g_free(rd);
}

void ril_pre_sim(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ril_voicecall_driver_data vc_data = { rd->ril, modem };
	struct ril_sim_data sim_data;

	DBG("");

	ofono_devinfo_create(modem, rd->vendor,
				get_driver_type(rd, OFONO_ATOM_TYPE_DEVINFO),
				rd->ril);
	ofono_voicecall_create(modem, rd->vendor,
				get_driver_type(rd, OFONO_ATOM_TYPE_VOICECALL),
				&vc_data);
	ofono_call_volume_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPES_CALL_VOLUME),
			rd->ril);

	sim_data.gril = rd->ril;
	sim_data.modem = modem;
	sim_data.ril_state_watch = NULL;

	rd->sim = ofono_sim_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_SIM), &sim_data);
}

void ril_post_sim(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	ofono_sms_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_SMS), rd->ril);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);

	ofono_phonebook_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_PHONEBOOK), modem);
}

void ril_post_online(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	struct ril_gprs_driver_data gprs_data = { rd->ril, modem };
	struct ril_gprs_context_data
		inet_ctx = { rd->ril, modem, OFONO_GPRS_CONTEXT_TYPE_INTERNET };
	struct ril_gprs_context_data
		mms_ctx = { rd->ril, modem, OFONO_GPRS_CONTEXT_TYPE_MMS };

	ofono_netreg_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_NETREG), rd->ril);
	ofono_ussd_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_USSD), rd->ril);
	ofono_call_settings_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_CALL_SETTINGS),
			rd->ril);
	ofono_call_barring_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_CALL_BARRING),
			rd->ril);
	ofono_call_forwarding_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_CALL_FORWARDING),
			rd->ril);
	gprs = ofono_gprs_create(modem, rd->vendor,
				get_driver_type(rd, OFONO_ATOM_TYPE_GPRS),
				&gprs_data);
	gc = ofono_gprs_context_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_GPRS_CONTEXT),
			&inet_ctx);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}

	gc = ofono_gprs_context_create(modem, rd->vendor,
			get_driver_type(rd, OFONO_ATOM_TYPE_GPRS_CONTEXT),
			&mms_ctx);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_MMS);
		ofono_gprs_add_context(gprs, gc);
	}
}

static void ril_set_online_cb(struct ril_msg *message, gpointer user_data)
{
	struct ril_data *rd = user_data;

	if (message != NULL && message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);

		/*
		 * Wait for radio state change event now, as that is the real
		 * moment when radio state changes.
		 */
	} else {
		ofono_modem_online_cb_t cb = rd->set_online_cbd->cb;

		ofono_error("%s: set_online: %d failed", __func__,
				rd->ofono_online);
		CALLBACK_WITH_FAILURE(cb, rd->set_online_cbd->data);

		g_free(rd->set_online_cbd);
		rd->set_online_cbd = NULL;
	}
}

static gboolean set_online_done_cb(gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->user;
	struct ril_data *rd = ofono_modem_get_data(modem);
	ofono_modem_online_cb_t cb = cbd->cb;

	DBG("%s: set_online OK: rd->ofono_online: %d",
						__func__, rd->ofono_online);
	manage_radio_settings_atom(modem);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	g_free(cbd);

	return FALSE;
}

void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
			ofono_modem_online_cb_t callback, void *data)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(callback, data, modem);

	rd->ofono_online = online;

	DBG("setting rd->ofono_online to: %d", online);

	if ((online && rd->radio_state == RADIO_STATE_ON) ||
			(!online && rd->radio_state == RADIO_STATE_OFF)) {
		g_idle_add(set_online_done_cb, cbd);
	} else {
		rd->set_online_cbd = cbd;
		ril_send_power(rd, online, ril_set_online_cb, rd);
	}
}

static void ril_connected(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	int version;

	/*
	 * We will use RIL version to check for presence of some features. The
	 * version is updated in AOSP after major changes. For instance:
	 *
	 * Version  9 -> AOSP 4.4
	 * Version 10 -> AOSP 5.0.0
	 * Version 11 -> AOSP 6.0.0
	 *
	 * Note that all Ubuntu phones are based on BSP >= 4.4.
	 */
	version = g_ril_unsol_parse_connected(rd->ril, message);
	g_ril_set_version(rd->ril, version);

	ofono_info("[%d,UNSOL]< %s, version %d", g_ril_get_slot(rd->ril),
		g_ril_unsol_request_to_string(rd->ril, message->req), version);
}

static int create_gril(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	int slot_id = ofono_modem_get_integer(modem, "Slot");
	const gchar *socket = ofono_modem_get_string(modem, "Socket");

	ofono_info("Using %s as socket for slot %d.", socket, slot_id);
	rd->ril = g_ril_new(socket, rd->vendor);

	/*
	 * NOTE: Since AT modems open a tty, and then call
	 * g_at_chat_new(), they're able to return -EIO if
	 * the first fails, and -ENOMEM if the second fails.
	 * in our case, we already return -EIO if the ril_new
	 * fails.  If this is important, we can create a ril_socket
	 * abstraction... ( probaby not a bad idea ).
	 */
	if (rd->ril == NULL) {
		ofono_error("g_ril_new() failed to create modem!");
		return -EIO;
	}

	if (slot_id == MULTISIM_SLOT_0)
		ril_data_0 = rd;
	else
		ril_data_1 = rd;

	g_ril_set_slot(rd->ril, slot_id);
	g_ril_set_vendor_print_msg_id_funcs(rd->ril,
						rd->request_id_to_string,
						rd->unsol_request_to_string);

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(rd->ril, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(rd->ril, ril_debug, rd);

	g_ril_register(rd->ril, RIL_UNSOL_RIL_CONNECTED,
			ril_connected, modem);

	g_ril_register(rd->ril, RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
			ril_radio_state_changed, modem);

	return 0;
}

static gboolean connect_rild(gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_info("Trying to reconnect to rild...");

	if (rd->rild_connect_retries++ < RILD_MAX_CONNECT_RETRIES) {
		if (create_gril(modem) < 0)
			return TRUE;
	} else {
		ofono_error("Exiting, can't connect to rild.");
		exit(0);
	}

	return FALSE;
}

int ril_enable(struct ofono_modem *modem)
{
	int ret;

	DBG("");

	ret = create_gril(modem);
	if (ret < 0)
		g_timeout_add_seconds(RILD_CONNECT_RETRY_TIME_S,
					connect_rild, modem);

	return -EINPROGRESS;
}

int ril_disable(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ril_send_power(rd, FALSE, NULL, NULL);

	return 0;
}

static struct ofono_modem_driver ril_driver = {
	.name = "ril",
	.probe = ril_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

/*
 * This plugin is a generic ( aka default ) device plugin for RIL-based devices.
 * The plugin 'rildev' is used to determine which RIL plugin should be loaded
 * based upon an environment variable.
 */
static int ril_init(void)
{
	int retval = ofono_modem_driver_register(&ril_driver);

	if (retval != 0)
		DBG("ofono_modem_driver_register returned: %d", retval);

	return retval;
}

static void ril_exit(void)
{
	DBG("");
	ofono_modem_driver_unregister(&ril_driver);
}

OFONO_PLUGIN_DEFINE(ril, "RIL modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ril_init, ril_exit)
