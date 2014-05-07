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
#include <grilunsol.h>

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"

#include "drivers/mtkmodem/mtkmodem.h"
#include "drivers/mtkmodem/mtk_constants.h"
#include "drivers/mtkmodem/mtkutil.h"
#include "drivers/mtkmodem/mtkrequest.h"

#define MAX_SIM_STATUS_RETRIES 15

#define MULTISIM_SLOT_0 0
#define MULTISIM_SLOT_1 1

#define SIM_1_ACTIVE 1
#define SIM_2_ACTIVE 2
#define NO_SIM_ACTIVE 0

typedef void (*pending_cb_t)(struct cb_data *cbd);

struct mtk_data {
	GRil *modem;
	int sim_status_retries;
	ofono_bool_t connected;
	ofono_bool_t have_sim;
	ofono_bool_t ofono_online;
	int radio_state;
	struct ofono_sim *sim;
	/* pending_* are used in case we are disconnected from the socket */
	pending_cb_t pending_cb;
	struct cb_data *pending_cbd;
	int slot;
	struct ril_sim_data sim_data;
	struct ofono_devinfo *devinfo;
};

/*
 * MTK dual SIM sockets are not completely symmetric: some requests (essentially
 * those related for radio power management and SIM slot enablement) can be sent
 * only through the socket for slot 0. So we need a pointer to the main socket.
 * Also, we need to access information of one channel from the other channel.
 */
static struct mtk_data *mtk_0;
static struct mtk_data *mtk_1;

static void send_get_sim_status(struct ofono_modem *modem);
static int create_gril(struct ofono_modem *modem);

static void mtk_debug(const char *str, void *user_data)
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

static void mtk_radio_state_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *ril = ofono_modem_get_data(modem);
	int radio_state = g_ril_unsol_parse_radio_state_changed(ril->modem,
								message);

	if (radio_state != ril->radio_state) {

		ofono_info("%s, slot %d: state: %s ril->ofono_online: %d",
				__func__, ril->slot,
				ril_radio_state_to_string(radio_state),
				ril->ofono_online);

		ril->radio_state = radio_state;

		switch (radio_state) {
		case RADIO_STATE_ON:
		/* MTK */
		case RADIO_STATE_SIM_NOT_READY:
		case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
		case RADIO_STATE_SIM_READY:
			break;

		case RADIO_STATE_UNAVAILABLE:
		case RADIO_STATE_OFF:
			if (ril->ofono_online) {
				ofono_warn("%s, slot %d: radio powered off!",
						__func__, ril->slot);
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
	struct mtk_data *ril = ofono_modem_get_data(modem);
	struct reply_sim_status *status;

	DBG("slot %d", ril->slot);

	if (message->error != RIL_E_SUCCESS) {
		ril->sim_status_retries++;

		ofono_error("[slot %d] GET_SIM_STATUS request failed: "
				"%s; retries: %d",
				ril->slot, ril_error_to_string(message->error),
				ril->sim_status_retries);

		if (ril->sim_status_retries < MAX_SIM_STATUS_RETRIES)
			g_timeout_add_seconds(2, sim_status_retry, modem);
		else
			ofono_error("[slot %d] Max retries for GET_SIM_STATUS"
					" exceeded!", ril->slot);
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
				ofono_warn("[slot %d] Card NOT_PRESENT",
						ril->slot);

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
	struct mtk_data *ril = ofono_modem_get_data(modem);

	DBG("slot %d", ril->slot);

	if (g_ril_send(ril->modem, RIL_REQUEST_GET_SIM_STATUS, NULL,
			sim_status_cb, modem, NULL) == 0)
		ofono_error("%s: failure sending request", __func__);
}

static int mtk_probe(struct ofono_modem *modem)
{
	struct mtk_data *ril = g_try_new0(struct mtk_data, 1);

	if (ril == NULL) {
		errno = ENOMEM;
		goto error;
	}

	ril->have_sim = FALSE;
	ril->ofono_online = FALSE;
	ril->radio_state = RADIO_STATE_OFF;

	ril->slot = ofono_modem_get_integer(modem, "Slot");

	if (ril->slot == MULTISIM_SLOT_0)
		mtk_0 = ril;
	else
		mtk_1 = ril;

	DBG("slot %d", ril->slot);

	ofono_modem_set_data(modem, ril);

	return 0;

error:
	g_free(ril);

	return -errno;
}

static void mtk_remove(struct ofono_modem *modem)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);

	ofono_modem_set_data(modem, NULL);

	if (!ril)
		return;

	g_ril_unref(ril->modem);

	g_free(ril);
}

static void mtk_pre_sim(struct ofono_modem *modem)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);

	DBG("slot %d", ril->slot);
}

static void mtk_post_sim(struct ofono_modem *modem)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);

	DBG("slot %d", ril->slot);
}

static void sim_state_watch(enum ofono_sim_state new_state, void *data)
{
	if (new_state == OFONO_SIM_STATE_READY) {
		struct ofono_modem *modem = data;
		struct mtk_data *ril = ofono_modem_get_data(modem);
		struct ofono_gprs *gprs;
		struct ofono_gprs_context *gc;
		struct ofono_message_waiting *mw;

		DBG("SIM ready, creating more atoms");

		/*
		 * TODO: this function should setup:
		 *  - phonebook
		 *  - stk ( SIM toolkit )
		 *  - radio_settings
		 */
		ofono_sms_create(modem, OFONO_RIL_VENDOR_MTK,
					RILMODEM, ril->modem);

		/* netreg needs access to the SIM (SPN, SPDI) */
		ofono_netreg_create(modem, OFONO_RIL_VENDOR_MTK,
					RILMODEM, ril->modem);
		ofono_ussd_create(modem, OFONO_RIL_VENDOR_MTK,
					RILMODEM, ril->modem);
		ofono_call_settings_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);
		ofono_call_forwarding_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);
		ofono_radio_settings_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);
		ofono_call_barring_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);

		gprs = ofono_gprs_create(modem, OFONO_RIL_VENDOR_MTK,
						MTKMODEM, ril->modem);

		gc = ofono_gprs_context_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);
		if (gc) {
			ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
			ofono_gprs_add_context(gprs, gc);
		}

		gc = ofono_gprs_context_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);
		if (gc) {
			ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_MMS);
			ofono_gprs_add_context(gprs, gc);
		}

		mw = ofono_message_waiting_create(modem);
		if (mw)
			ofono_message_waiting_register(mw);
	}
}

static void mtk_post_online(struct ofono_modem *modem)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);

	DBG("slot %d", ril->slot);

	ril->sim_data.gril = ril->modem;
	ril->sim_data.modem = modem;
	ril->sim_data.ril_state_watch = sim_state_watch;

	ril->sim = ofono_sim_create(modem, OFONO_RIL_VENDOR_MTK,
					RILMODEM, &ril->sim_data);
	g_assert(ril->sim != NULL);

	/* Create interfaces useful for emergency calls */
	ofono_voicecall_create(modem, OFONO_RIL_VENDOR_MTK,
					MTKMODEM, ril->modem);
	ofono_call_volume_create(modem, OFONO_RIL_VENDOR_MTK,
					RILMODEM, ril->modem);

	/* Ask sim status */
	ril->sim_status_retries = 0;
	send_get_sim_status(modem);
}

static void mtk_sim_mode_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_modem *modem = cbd->user;
	struct mtk_data *ril = ofono_modem_get_data(modem);

	ril->pending_cb = NULL;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(ril->modem, message);

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static int sim_state()
{
	int state = mtk_0->ofono_online ? SIM_1_ACTIVE : NO_SIM_ACTIVE;
	if (mtk_1 && mtk_1->ofono_online)
		state |= SIM_2_ACTIVE;

	return state;
}

static void mtk_send_sim_mode(GRilResponseFunc func, gpointer user_data)
{
	struct parcel rilp;
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb;
	GDestroyNotify notify = NULL;
	int sim_mode;

	if (cbd != NULL) {
		notify = g_free;
		cb = cbd->cb;
	}

	sim_mode = sim_state();

	if (sim_mode == NO_SIM_ACTIVE)
		sim_mode = MTK_SWITCH_MODE_ALL_INACTIVE;

	g_mtk_request_dual_sim_mode_switch(mtk_0->modem, sim_mode, &rilp);

	/* This request is always sent through the main socket */
	if (g_ril_send(mtk_0->modem, RIL_REQUEST_DUAL_SIM_MODE_SWITCH,
			&rilp, func, cbd, notify) == 0 && cbd != NULL) {
		ofono_error("%s: failure sending request", __func__);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void poweron_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->user;
	struct mtk_data *ril = ofono_modem_get_data(modem);
	ofono_modem_online_cb_t cb = cbd->cb;

	DBG("");

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(ril->modem, message);

		mtk_send_sim_mode(mtk_sim_mode_cb, cbd);
	} else {
		ofono_error("%s RADIO_POWERON error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
	}
}

static void poweron_disconnect(struct cb_data *cbd)
{
	DBG("Execute pending sim mode switch");

	mtk_send_sim_mode(mtk_sim_mode_cb, cbd);
}

static void online_off_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_modem *modem = cbd->user;
	struct mtk_data *ril = ofono_modem_get_data(modem);

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(ril->modem, message);

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void mtk_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t callback, void *data)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(callback, data, modem);
	ofono_modem_online_cb_t cb = cbd->cb;
	int current_state, next_state;

	current_state = sim_state();

	ril->ofono_online = online;

	/* Changes as ril points to either mtk_0 or mtk_1 global variables */
	next_state = sim_state();

	DBG("setting mtk_%d->ofono_online to: %d (from %d to %d)",
		ril->slot, online, current_state, next_state);

	if (current_state == next_state) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		g_free(cbd);
		return;
	}

	/* Reset mtk_data variables */
	if (online == FALSE) {
		ril->have_sim = FALSE;
		ril->sim_status_retries = 0;
	}

	if (current_state == NO_SIM_ACTIVE) {
		/* Old state was off, need to power on the modem */
		if (g_ril_send(mtk_0->modem, RIL_REQUEST_RADIO_POWERON, NULL,
				poweron_cb, cbd, NULL) == 0) {
			CALLBACK_WITH_FAILURE(cb, cbd->data);
			g_free(cbd);
		} else {
			/* Socket might disconnect... failsafe */
			mtk_0->pending_cb = poweron_disconnect;
			mtk_0->pending_cbd = cbd;
		}
	} else if (next_state == NO_SIM_ACTIVE) {
		if (g_ril_send(mtk_0->modem, RIL_REQUEST_RADIO_POWEROFF, NULL,
				online_off_cb, cbd, g_free) == 0) {
			ofono_error("%s: failure sending request", __func__);
			CALLBACK_WITH_FAILURE(cb, cbd->data);
			g_free(cbd);
		}
	} else {
		mtk_send_sim_mode(mtk_sim_mode_cb, cbd);
	}
}

static gboolean mtk_connected(gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct mtk_data *ril = ofono_modem_get_data(modem);

        ofono_info("[slot %d] CONNECTED", ril->slot);

	/* TODO: need a disconnect function to restart things! */
	ril->connected = TRUE;

	DBG("calling set_powered(TRUE)");

	ofono_modem_set_powered(modem, TRUE);

	/* Call the function just once */
	return FALSE;
}

static gboolean reconnect_rild(gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct mtk_data *ril = ofono_modem_get_data(modem);

        ofono_info("[slot %d] trying to reconnect", ril->slot);

	if (create_gril(modem) < 0)
		return TRUE;

	ril->devinfo = ofono_devinfo_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);

	if (ril->pending_cb)
		ril->pending_cb(ril->pending_cbd);

	/* Reconnected: do not call this again */
	return FALSE;
}

#define WAIT_FOR_RILD_TO_RESTART_MS 8000	/* Milliseconds */

static void socket_disconnected(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *ril = ofono_modem_get_data(modem);

	DBG("slot %d", ril->slot);

	/* Uses old gril object, remove and recreate later */
	ofono_devinfo_remove(ril->devinfo);

	g_ril_unref(ril->modem);
	ril->modem = NULL;

	/* The disconnection happens because rild is re-starting, wait for it */
	g_timeout_add(WAIT_FOR_RILD_TO_RESTART_MS, reconnect_rild, modem);
}

#ifdef MTK_INITIAL_OFF
static void poweroff_cb(struct ril_msg *message, gpointer user_data)
{
	if (message->error != RIL_E_SUCCESS &&
			message->error != RIL_E_RADIO_NOT_AVAILABLE)
		ofono_error("%s RADIO_POWEROFF error %s", __func__,
				ril_error_to_string(message->error));
	else
		mtk_connected(user_data);
}
#endif

static const char sock_slot_0[] = "/dev/socket/rild";
static const char sock_slot_1[] = "/dev/socket/rild2";
static const char hex_slot_0[] = "Slot 0: ";
static const char hex_slot_1[] = "Slot 1: ";

static int create_gril(struct ofono_modem *modem)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);
	const char *sock_path;
	const char *hex_prefix;

	DBG("slot %d", ril->slot);

	if (ril->slot == MULTISIM_SLOT_0) {
		sock_path = sock_slot_0;
		hex_prefix = hex_slot_0;
	} else {
		sock_path = sock_slot_1;
		hex_prefix = hex_slot_1;
	}

	/* Opens the socket to RIL */
	ril->modem = g_ril_new(sock_path, OFONO_RIL_VENDOR_MTK);

	/*
	 * NOTE: Since AT modems open a tty, and then call
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

	g_ril_set_vendor_print_msg_id_funcs(ril->modem,
						mtk_request_id_to_string,
						mtk_unsol_request_to_string);

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(ril->modem, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(ril->modem, mtk_debug, (char *) hex_prefix);

	g_ril_set_disconnect_function(ril->modem, socket_disconnected, modem);

	g_ril_register(ril->modem, RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
			mtk_radio_state_changed, modem);

	return 0;
}

static int mtk_enable(struct ofono_modem *modem)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);
	int ret;

	ret = create_gril(modem);
	if (ret < 0)
		return ret;

	ril->devinfo = ofono_devinfo_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, ril->modem);

#ifdef MTK_INITIAL_OFF
	g_ril_send(ril->modem, RIL_REQUEST_RADIO_POWEROFF, NULL,
			poweroff_cb, modem, NULL);
#else
	mtk_connected(modem);
#endif

	return -EINPROGRESS;
}

static int mtk_disable(struct ofono_modem *modem)
{
	struct mtk_data *ril = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (ril->ofono_online) {
		ril->ofono_online = FALSE;
		mtk_send_sim_mode(NULL, NULL);
	}

	return 0;
}

static struct ofono_modem_driver mtk_driver = {
	.name = "mtk",
	.probe = mtk_probe,
	.remove = mtk_remove,
	.enable = mtk_enable,
	.disable = mtk_disable,
	.pre_sim = mtk_pre_sim,
	.post_sim = mtk_post_sim,
	.post_online = mtk_post_online,
	.set_online = mtk_set_online,
};

static int mtk_init(void)
{
	int retval = 0;

	if ((retval = ofono_modem_driver_register(&mtk_driver)))
		DBG("ofono_modem_driver_register returned: %d", retval);

	return retval;
}

static void mtk_exit(void)
{
	DBG("");
	ofono_modem_driver_unregister(&mtk_driver);
}

OFONO_PLUGIN_DEFINE(mtk, "MTK modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, mtk_init, mtk_exit)
