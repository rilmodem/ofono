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

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"

#define MAX_SIM_STATUS_RETRIES 15

#define RILD_CMD_SOCKET "/dev/socket/rild"

struct ril_data {
	GRil *modem;
	int sim_status_retries;
	ofono_bool_t connected;
	ofono_bool_t have_sim;
	ofono_bool_t ofono_online;
	int radio_state;
	struct ofono_sim *sim;
	struct ofono_voicecall *voice;
};

static void send_get_sim_status(struct ofono_modem *modem);

static void ril_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static gboolean sim_status_retry(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	send_get_sim_status(modem);

	/* Makes this a single shot */
	return FALSE;
}

static void ril_radio_state_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);
	int radio_state = g_ril_unsol_parse_radio_state_changed(ril->modem,
								message);

	if (radio_state != ril->radio_state) {

		ofono_info("%s: state: %s ril->ofono_online: %d",
				__func__,
				ril_radio_state_to_string(radio_state),
				ril->ofono_online);

		ril->radio_state = radio_state;

		switch (radio_state) {
		case RADIO_STATE_ON:

			if (ril->voice == NULL)
				ril->voice =
					ofono_voicecall_create(modem,
								0,
								RILMODEM,
								ril->modem);

			send_get_sim_status(modem);
			break;

		case RADIO_STATE_UNAVAILABLE:
		case RADIO_STATE_OFF:

			/*
			 * If radio powers off asychronously, then
			 * assert, and let upstart re-start the stack.
			 */
			if (ril->ofono_online) {
				ofono_error("%s: radio self-powered off!",
						__func__);
				g_assert(FALSE);
			}
			break;
		default:
			/* Malformed parcel; no radio state == broken rild */
			g_assert(FALSE);
		}
	}
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct reply_sim_status *status;

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ril->sim_status_retries++;

		ofono_error("GET_SIM_STATUS request failed: %s; retries: %d",
				ril_error_to_string(message->error),
				ril->sim_status_retries);

		if (ril->sim_status_retries < MAX_SIM_STATUS_RETRIES)
			g_timeout_add_seconds(2, sim_status_retry, modem);
		else
			ofono_error("Max retries for GET_SIM_STATUS exceeded!");
	} else {

		if ((status = g_ril_reply_parse_sim_status(ril->modem, message))
				!= NULL) {

			if (status->card_state == RIL_CARDSTATE_PRESENT) {
				DBG("Card PRESENT; num_apps: %d",
					status->num_apps);

				if (!ril->have_sim) {
					DBG("notify SIM inserted");
					ril->have_sim = TRUE;

					ofono_sim_inserted_notify(ril->sim, TRUE);
				}

			} else {
				ofono_warn("Card NOT_PRESENT.");

				if (ril->have_sim) {
					DBG("notify SIM removed");
					ril->have_sim = FALSE;

					ofono_sim_inserted_notify(ril->sim, FALSE);
				}
			}
			g_ril_reply_free_sim_status(status);
		}
	}
}

static void send_get_sim_status(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("");

	g_ril_send(ril->modem, RIL_REQUEST_GET_SIM_STATUS, NULL,
			sim_status_cb, modem, NULL);
}

static int ril_probe(struct ofono_modem *modem)
{
	struct ril_data *ril = g_try_new0(struct ril_data, 1);
	if (ril == NULL) {
		errno = ENOMEM;
		goto error;
	}

	DBG("");

	ril->have_sim = FALSE;
	ril->ofono_online = FALSE;
	ril->radio_state = RADIO_STATE_OFF;

	ofono_modem_set_data(modem, ril);

	return 0;

error:
	g_free(ril);

	return -errno;
}

static void ril_remove(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);


	ofono_modem_set_data(modem, NULL);

	if (!ril)
		return;

	g_ril_unref(ril->modem);

	g_free(ril);
}

static void ril_pre_sim(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct ril_sim_data sim_data;

	DBG("");

	sim_data.gril = ril->modem;
	sim_data.modem = modem;
	sim_data.ril_state_watch = NULL;

	ril->sim = ofono_sim_create(modem, 0, RILMODEM, &sim_data);
	g_assert(ril->sim != NULL);
}

static void ril_post_sim(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	struct ofono_message_waiting *mw;

	/* TODO: this function should setup:
	 *  - phonebook
	 *  - stk ( SIM toolkit )
	 *  - radio_settings
	 */
	ofono_sms_create(modem, 0, RILMODEM, ril->modem);

	gprs = ofono_gprs_create(modem, 0, RILMODEM, ril->modem);
	gc = ofono_gprs_context_create(modem, 0, RILMODEM, ril->modem);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}

	gc = ofono_gprs_context_create(modem, 0, RILMODEM, ril->modem);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_MMS);
		ofono_gprs_add_context(gprs, gc);
	}

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);

	ofono_call_forwarding_create(modem, 0, RILMODEM, ril->modem);
}

static void ril_post_online(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	ofono_call_volume_create(modem, 0, RILMODEM, ril->modem);
	ofono_netreg_create(modem, 0, RILMODEM, ril->modem);
	ofono_ussd_create(modem, 0, RILMODEM, ril->modem);
	ofono_call_settings_create(modem, 0, RILMODEM, ril->modem);
	ofono_radio_settings_create(modem, 0, RILMODEM, ril->modem);
}

static void ril_set_online_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ril_data *ril = cbd->user;
	ofono_modem_online_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		DBG("%s: set_online OK: ril->ofono_online: %d", __func__,
			ril->ofono_online);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: set_online: %d failed", __func__,
				ril->ofono_online);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_send_power(struct ril_data *ril, ofono_bool_t online,
				GRilResponseFunc func,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb;
	GDestroyNotify notify = NULL;
	struct parcel rilp;

	if (cbd != NULL) {
		notify = g_free;
		cb = cbd->cb;
	}

	DBG("(online = 1, offline = 0)): %i", online);

	g_ril_request_power(ril->modem, (const gboolean) online, &rilp);

	if (g_ril_send(ril->modem, RIL_REQUEST_RADIO_POWER, &rilp,
			func, cbd, notify) == 0 && cbd != NULL) {

		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t callback, void *data)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(callback, data, ril);

	ril->ofono_online = online;

	DBG("setting ril->ofono_online to: %d", online);

	ril_send_power(ril, online, ril_set_online_cb, cbd);
}

static void ril_connected(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);

        ofono_info("[UNSOL]< %s", g_ril_unsol_request_to_string(ril->modem,
								message->req));

	/* TODO: need a disconnect function to restart things! */
	ril->connected = TRUE;

	DBG("calling set_powered(TRUE)");

	ofono_modem_set_powered(modem, TRUE);
}

static int create_gril(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	ril->modem = g_ril_new(RILD_CMD_SOCKET, OFONO_RIL_VENDOR_AOSP);

	/* NOTE: Since AT modems open a tty, and then call
	 * g_at_chat_new(), they're able to return -EIO if
	 * the first fails, and -ENOMEM if the second fails.
	 * in our case, we already return -EIO if the ril_new
	 * fails.  If this is important, we can create a ril_socket
	 * abstraction... ( probaby not a bad idea ).
	 */

	if (ril->modem == NULL) {
		DBG("g_ril_new() failed to create modem!");
		return -EIO;
	}

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(ril->modem, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(ril->modem, ril_debug, "Device: ");

	g_ril_register(ril->modem, RIL_UNSOL_RIL_CONNECTED,
			ril_connected, modem);

	g_ril_register(ril->modem, RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
			ril_radio_state_changed, modem);

	return 0;
}

static int ril_enable(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	int ret;

	DBG("");

	ret = create_gril(modem);
	if (ret < 0)
		return ret;

	ofono_devinfo_create(modem, 0, RILMODEM, ril->modem);

	return -EINPROGRESS;
}

static int ril_disable(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ril_send_power(ril, FALSE, NULL, NULL);

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
	int retval = 0;

	if ((retval = ofono_modem_driver_register(&ril_driver)))
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
