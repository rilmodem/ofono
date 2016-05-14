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
#include "drivers/rilmodem/vendor.h"
#include "drivers/qcommsimmodem/qcom_msim_modem.h"

#define MAX_SIM_STATUS_RETRIES 15

/* this gives 30s for rild to initialize */
#define RILD_MAX_CONNECT_RETRIES 5
#define RILD_CONNECT_RETRY_TIME_S 5

struct ril_data {
	GRil *ril;
	enum ofono_ril_vendor vendor;
	int sim_status_retries;
	ofono_bool_t connected;
	ofono_bool_t ofono_online;
	int radio_state;
	struct ofono_sim *sim;
	struct ofono_radio_settings *radio_settings;
	int rild_connect_retries;
};

static void ril_debug(const char *str, void *user_data)
{
	struct ril_data *rd = user_data;

	ofono_info("Device %d: %s", g_ril_get_slot(rd->ril), str);
}

static void ril_get_radio_capability_cb(struct ril_msg *message,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct reply_radio_cap *rcap;
	ofono_bool_t lte_cap;

	if (message->error != RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);

		ofono_error("%s: failed to request radio capability", __func__);
		return;
	}

	rcap = g_ril_reply_parse_get_radio_capability(rd->ril, message);
	if (rcap == NULL) {
		ofono_error("%s: invalid parcel", __func__);
		return;
	}

	lte_cap = ((rcap->raf & (0x01 << RADIO_TECH_LTE)) != 0) ? TRUE : FALSE;
	ofono_modem_set_boolean(modem, MODEM_PROP_LTE_CAPABLE, lte_cap);

	g_ril_reply_free_get_radio_capability(rcap);
}

static void ril_radio_state_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	int radio_state = g_ril_unsol_parse_radio_state_changed(rd->ril,
								message);

	if (radio_state != rd->radio_state) {

		ofono_info("%s: state: %s rd->ofono_online: %d",
				__func__,
				ril_radio_state_to_string(radio_state),
				rd->ofono_online);

		rd->radio_state = radio_state;

		switch (radio_state) {
		case RADIO_STATE_ON:

			if (rd->radio_settings == NULL) {
				char *rs_driver;
				struct ril_radio_settings_driver_data
						rs_data = { rd->ril, modem };

				if (rd->vendor == OFONO_RIL_VENDOR_QCOM_MSIM)
					rs_driver = QCOMMSIMMODEM;
				else
					rs_driver = RILMODEM;

				rd->radio_settings =
					ofono_radio_settings_create(modem,
							rd->vendor, rs_driver,
							&rs_data);
			}

			/* Query RadioCapability when radio is available */
			g_ril_send(rd->ril, RIL_REQUEST_GET_RADIO_CAPABILITY,
					NULL, ril_get_radio_capability_cb,
					modem, NULL);
			break;

		case RADIO_STATE_UNAVAILABLE:
		case RADIO_STATE_OFF:

			/*
			 * Unexpected radio state change, as we are supposed to
			 * be online. UNAVAILABLE has been seen occassionally
			 * when powering off the phone. We wait 5 secs to avoid
			 * too fast re-spawns, then exit with error to make
			 * upstart re-start ofono.
			 */
			if (rd->ofono_online) {
				ofono_error("%s: radio self-powered off!",
						__func__);
				sleep(5);
				exit(1);
			}
			break;
		default:
			/* Malformed parcel; no radio state == broken rild */
			g_assert(FALSE);
		}
	}
}

int ril_create(struct ofono_modem *modem, enum ofono_ril_vendor vendor)
{
	ofono_bool_t lte_cap;
	struct ril_data *rd = g_try_new0(struct ril_data, 1);
	if (rd == NULL) {
		errno = ENOMEM;
		goto error;
	}

	DBG("");

	rd->vendor = vendor;
	rd->ofono_online = FALSE;
	rd->radio_state = RADIO_STATE_UNAVAILABLE;

	lte_cap = getenv("OFONO_RIL_RAT_LTE") ? TRUE : FALSE;
	ofono_modem_set_boolean(modem, MODEM_PROP_LTE_CAPABLE, lte_cap);

	ofono_modem_set_data(modem, rd);

	return 0;

error:
	g_free(rd);

	return -errno;
}

static int ril_probe(struct ofono_modem *modem)
{
	return ril_create(modem, OFONO_RIL_VENDOR_AOSP);
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

	ofono_devinfo_create(modem, rd->vendor, RILMODEM, rd->ril);
	ofono_voicecall_create(modem, rd->vendor, RILMODEM, &vc_data);
	ofono_call_volume_create(modem, rd->vendor, RILMODEM, rd->ril);

	sim_data.gril = rd->ril;
	sim_data.modem = modem;
	sim_data.ril_state_watch = NULL;

	rd->sim = ofono_sim_create(modem, rd->vendor, RILMODEM, &sim_data);
	g_assert(rd->sim != NULL);
}

void ril_post_sim(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	struct ofono_message_waiting *mw;
	struct ril_gprs_driver_data gprs_data = { rd->ril, modem };
	struct ril_gprs_context_data
		inet_ctx = { rd->ril, modem, OFONO_GPRS_CONTEXT_TYPE_INTERNET };
	struct ril_gprs_context_data
		mms_ctx = { rd->ril, modem, OFONO_GPRS_CONTEXT_TYPE_MMS };
	char *gprs_driver;

	/* TODO: this function should setup:
	 *  - phonebook
	 *  - stk ( SIM toolkit )
	 *  - radio_settings
	 */
	ofono_sms_create(modem, rd->vendor, RILMODEM, rd->ril);

	if (rd->vendor == OFONO_RIL_VENDOR_QCOM_MSIM)
		gprs_driver = QCOMMSIMMODEM;
	else
		gprs_driver = RILMODEM;

	gprs = ofono_gprs_create(modem, rd->vendor, gprs_driver, &gprs_data);

	gc = ofono_gprs_context_create(modem, rd->vendor, RILMODEM, &inet_ctx);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}

	gc = ofono_gprs_context_create(modem, rd->vendor, RILMODEM, &mms_ctx);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_MMS);
		ofono_gprs_add_context(gprs, gc);
	}

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);

	ofono_call_forwarding_create(modem, rd->vendor, RILMODEM, rd->ril);

	ofono_phonebook_create(modem, rd->vendor, RILMODEM, modem);
}

void ril_post_online(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_netreg_create(modem, rd->vendor, RILMODEM, rd->ril);
	ofono_ussd_create(modem, rd->vendor, RILMODEM, rd->ril);
	ofono_call_settings_create(modem, rd->vendor, RILMODEM, rd->ril);
	ofono_call_barring_create(modem, rd->vendor, RILMODEM, rd->ril);
}

static void ril_set_online_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ril_data *rd = cbd->user;
	ofono_modem_online_cb_t cb = cbd->cb;

	if (message != NULL && message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);

		DBG("%s: set_online OK: rd->ofono_online: %d", __func__,
			rd->ofono_online);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: set_online: %d failed", __func__,
				rd->ofono_online);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}

	g_free(cbd);
}

static void ril_send_power(struct ril_data *rd, ofono_bool_t online,
				GRilResponseFunc func,
				gpointer user_data)
{
	struct parcel rilp;

	DBG("(online = 1, offline = 0)): %i", online);

	g_ril_request_power(rd->ril, (const gboolean) online, &rilp);

	if (g_ril_send(rd->ril, RIL_REQUEST_RADIO_POWER, &rilp,
			func, user_data, NULL) == 0 && func != NULL) {

		func(NULL, user_data);
	}
}

void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
			ofono_modem_online_cb_t callback, void *data)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(callback, data, rd);

	rd->ofono_online = online;

	DBG("setting rd->ofono_online to: %d", online);

	ril_send_power(rd, online, ril_set_online_cb, cbd);
}

static void ril_set_powered_off_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	if (message != NULL && message->error == RIL_E_SUCCESS)
		g_ril_print_response_no_args(rd->ril, message);

	DBG("calling set_powered(TRUE)");

	ofono_modem_set_powered(modem, TRUE);
}

static void ril_connected(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	int version;

	version = g_ril_unsol_parse_connected(rd->ril, message);
	g_ril_set_version(rd->ril, version);

	ofono_info("[%d,UNSOL]< %s, version %d", g_ril_get_slot(rd->ril),
		g_ril_unsol_request_to_string(rd->ril, message->req), version);

	/* TODO: need a disconnect function to restart things! */
	rd->connected = TRUE;

	DBG("calling set_powered(FALSE) on connected");

	ril_send_power(rd, FALSE, ril_set_powered_off_cb, modem);
}

static int create_gril(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	int slot_id = ofono_modem_get_integer(modem, "Slot");
	const gchar *socket = ofono_modem_get_string(modem, "Socket");

	ofono_info("Using %s as socket for slot %d.", socket, slot_id);
	rd->ril = g_ril_new(socket, rd->vendor);

	/* NOTE: Since AT modems open a tty, and then call
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
	g_ril_set_slot(rd->ril, slot_id);

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
