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

#include <gril.h>
#include <grilreply.h>
#include <grilrequest.h>
#include <grilunsol.h>

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"

#include "drivers/mtkmodem/mtkmodem.h"
#include "drivers/mtkmodem/mtk_constants.h"
#include "drivers/mtkmodem/mtkutil.h"
#include "drivers/mtkmodem/mtkrequest.h"
#include "drivers/mtkmodem/mtkreply.h"
#include "drivers/mtkmodem/mtkunsol.h"
#include "drivers/mtkmodem/mtksettings.h"

#define MAX_SIM_STATUS_RETRIES 15

#define MULTISIM_SLOT_0 0
#define MULTISIM_SLOT_1 1

#define SIM_1_ACTIVE 1
#define SIM_2_ACTIVE 2
#define NO_SIM_ACTIVE 0

#define SOCKET_NUM_FOR_DBG_0 -1
#define SOCKET_NUM_FOR_DBG_1 -2

/* this gives 30s for rild to initialize */
#define RILD_MAX_CONNECT_RETRIES 5
#define RILD_CONNECT_RETRY_TIME_S 5

#define T_WAIT_DISCONN_MS 1000
#define T_SIM_SWITCH_FAILSAFE_MS 1000

enum mtk_sim_type {
	MTK_SIM_TYPE_1 = 1,
	MTK_SIM_TYPE_2 = 2,
	MTK_SIM_TYPE_3 = 3
};

static const char hex_slot_0[] = "Slot 0: ";
static const char hex_slot_1[] = "Slot 1: ";

typedef void (*pending_cb_t)(struct cb_data *cbd);

struct mtk_data {
	GRil *ril;
	int sim_status_retries;
	ofono_bool_t ofono_online;
	ofono_bool_t ofono_online_target;
	int radio_state;
	struct ofono_sim *sim;
	/* pending_* are used in case we are disconnected from the socket */
	pending_cb_t pending_cb;
	struct cb_data *pending_cbd;
	int slot;
	struct ril_sim_data sim_data;
	struct ofono_devinfo *devinfo;
	struct ofono_voicecall *voicecall;
	struct ofono_call_volume *callvolume;	
	struct cb_data *pending_online_cbd;
	ofono_bool_t pending_online;
	ofono_bool_t gprs_attach;
	int rild_connect_retries;
	struct ofono_sms *sms;
	struct ofono_netreg *netreg;
	struct ofono_ussd *ussd;
	struct ofono_call_settings *call_settings;
	struct ofono_call_forwarding *call_forwarding;
	struct ofono_call_barring *call_barring;
	struct ofono_phonebook *phonebook;
	struct ofono_gprs *gprs;
	struct ofono_message_waiting *message_waiting;
	struct ofono_modem *modem;
	ofono_bool_t has_3g;
	struct mtk_settings_data *mtk_settings;
	int fw_type;
	ofono_modem_online_cb_t online_cb;
	void *online_data;
};

/*
 * MTK dual SIM sockets are not completely symmetric: some requests (essentially
 * those related for radio power management and SIM slot enablement) can be sent
 * only through the socket for slot 0. So we need a pointer to the main socket.
 * Also, we need to access information of one channel from the other channel.
 */
static struct mtk_data *mtk_data_0;
static struct mtk_data *mtk_data_1;

/* Some variables control global state of the modem and are then static */
static gboolean disconnect_expected;
static guint not_disconn_cb_id;

struct socket_data {
	GRil *ril;
	const char *path;
	int radio_state;
	guint radio_state_ev_id;
};

static struct socket_data *sock_0, *sock_1;

static int create_gril(struct ofono_modem *modem);
static gboolean mtk_connected(gpointer user_data);
static void mtk_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t callback, void *data);
static void query_3g_caps(struct socket_data *sock);
static void socket_disconnected(gpointer user_data);
static void start_slot(struct mtk_data *md, struct socket_data *sock,
			const char *hex_prefix);
static void exec_pending_online(struct mtk_data *md);

static void mtk_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static struct mtk_data *mtk_data_complement(struct mtk_data *md)
{
	if (md->slot == MULTISIM_SLOT_0)
		return mtk_data_1;
	else
		return mtk_data_0;
}

static struct socket_data *socket_complement(struct socket_data *sock)
{
	if (sock == sock_0)
		return sock_1;
	else
		return sock_0;
}

/*
 * mtk_set_attach_state and mtk_detach_received are called by mtkmodem's gprs
 * driver. They are needed to solve an issue with data attachment: in case
 * org.ofono.ConnectionManager Powered property is set for, say, slot 1 while
 * slot 0 has that property also set, slot 1 will not change the data
 * registration state even after slot 0 data connection is finally dropped. To
 * force slot 1 to try to attach we need to send an additional
 * MTK_RIL_REQUEST_SET_GPRS_CONNECT_TYPE. The way to know when to do this is to
 * detect when slot 0 has finally detached. This is done listening for
 * MTK_RIL_UNSOL_GPRS_DETACH events, but unfortunately these events are received
 * in the modem that does not need to know about them, so we have to pass them
 * to the mtk plugin (which has knowledge of both modems) that will take proper
 * action in the other modem.
 */

void mtk_set_attach_state(struct ofono_modem *modem, ofono_bool_t attached)
{
	struct mtk_data *md = ofono_modem_get_data(modem);

	md->gprs_attach = attached;
}

static void detach_received_cb(struct ril_msg *message, gpointer user_data)
{
	struct mtk_data *md = user_data;

	if (message->error == RIL_E_SUCCESS)
		g_ril_print_response_no_args(md->ril, message);
	else
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
}

void mtk_detach_received(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);
	struct mtk_data *md_c = mtk_data_complement(md);

	if (md_c != NULL && md_c->gprs_attach) {
		struct parcel rilp;

		g_mtk_request_set_gprs_connect_type(md_c->ril,
						md_c->gprs_attach, &rilp);

		if (g_ril_send(md_c->ril,
				MTK_RIL_REQUEST_SET_GPRS_CONNECT_TYPE,
				&rilp, detach_received_cb, md_c, NULL) == 0)
			ofono_error("%s: send failed", __func__);
	}
}

static void radio_state_changed(struct ril_msg *message, gpointer user_data)
{
	struct socket_data *sock = user_data;
	int radio_state = g_ril_unsol_parse_radio_state_changed(sock->ril,
								message);

	if (radio_state != sock->radio_state) {
		struct socket_data *sock_c = socket_complement(sock);

		ofono_info("%s, %s, state: %s", __func__, sock->path,
				ril_radio_state_to_string(radio_state));

		/*
		 * If there is just one slot, just start it. Otherwise, we ask
		 * who owns the 3G capabilities in case both slots have already
		 * radio state different from UNAVAILABLE.
		 */
		if (mtk_data_1 == NULL) {
			mtk_data_0->has_3g = TRUE;
			start_slot(mtk_data_0, sock, hex_slot_0);
		} else if (sock->radio_state == RADIO_STATE_UNAVAILABLE &&
				sock_c != NULL && sock_c->radio_state !=
						RADIO_STATE_UNAVAILABLE) {
			query_3g_caps(sock);
		}

		sock->radio_state = radio_state;
	}
}

static void exec_online_callback(struct mtk_data *md)
{
	if (md->online_cb != NULL) {
		/*
		 * We assume success, as the sim switch request was successful
		 * too. Also, the SIM_* states can be returned independently of
		 * the radio state, so we cannot reliably use it to know if the
		 * sim switch command really did what was requested.
		 */
		CALLBACK_WITH_SUCCESS(md->online_cb, md->online_data);
		md->online_cb = NULL;
		md->online_data = NULL;

		if (md->ofono_online)
			md->mtk_settings =
				mtk_settings_create(md->modem, md->ril,
							md->has_3g);
		else
			mtk_settings_remove(md->mtk_settings);

		exec_pending_online(md);
	}
}

static void mtk_radio_state_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);
	int radio_state = g_ril_unsol_parse_radio_state_changed(md->ril,
								message);

	ofono_info("%s, slot %d: state: %s md->ofono_online: %d",
			__func__, md->slot,
			ril_radio_state_to_string(radio_state),
			md->ofono_online);

	md->radio_state = radio_state;

	switch (radio_state) {
	case RADIO_STATE_ON:
	/* RADIO_STATE_SIM_* are deprecated in AOSP, but still used by MTK */
	case RADIO_STATE_SIM_NOT_READY:
	case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
	case RADIO_STATE_SIM_READY:
	case RADIO_STATE_UNAVAILABLE:
	case RADIO_STATE_OFF:
		break;
	default:
		/* Malformed parcel; no radio state == broken rild */
		g_assert(FALSE);
	}

	exec_online_callback(md);
}

static void reg_suspended_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	g_ril_print_response_no_args(md->ril, message);
}

static void reg_suspended(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);
	struct parcel rilp;
	int session_id;

	session_id = g_mtk_unsol_parse_registration_suspended(md->ril, message);
	if (session_id < 0) {
		ofono_error("%s: parse error", __func__);
		return;
	}

	g_mtk_request_resume_registration(md->ril, session_id, &rilp);

	if (g_ril_send(md->ril, MTK_RIL_REQUEST_RESUME_REGISTRATION, &rilp,
			reg_suspended_cb, modem, NULL) == 0)
		ofono_error("%s: failure sending request", __func__);
}

static void sim_removed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	DBG("");

	g_ril_print_unsol_no_args(md->ril, message);

	ofono_modem_set_powered(modem, FALSE);
	g_idle_add(mtk_connected, modem);
}

static void sim_inserted(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	DBG("");

	g_ril_print_unsol_no_args(md->ril, message);

	if (getenv("OFONO_RIL_HOT_SIM_SWAP")) {
		ofono_modem_set_powered(modem, FALSE);
		g_idle_add(mtk_connected, modem);
	}
}

static void set_trm_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	g_ril_print_response_no_args(md->ril, message);

	/*
	 * rild will close now the socket, so we set our state to power off:
	 * we will set the modem to powered when we reconnect to the socket.
	 * Measurements showed that rild takes around 5 seconds to re-start, but
	 * we use an 8 seconds timeout as times can vary and that value is also
	 * compatible with krillin modem.
	 */
	md->ofono_online = FALSE;
	ofono_modem_set_powered(md->modem, FALSE);
}

static void store_type_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);
	struct parcel rilp;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	g_ril_print_response_no_args(md->ril, message);

	/*
	 * Send SET_TRM, which reloads the FW. We do not know the meaning of the
	 * magic number 0x0B.
	 */
	g_mtk_request_set_trm(md->ril, 0x0B, &rilp);

	if (g_ril_send(md->ril, MTK_RIL_REQUEST_SET_TRM, &rilp,
			set_trm_cb, modem, NULL) == 0)
		ofono_error("%s: failure sending request", __func__);
}

static gboolean find_in_table(const char *str,
				const char **table, int num_elem)
{
	int i;

	for (i = 0; i < num_elem; ++i)
		if (strncmp(str, table[i], strlen(table[i])) == 0)
			return TRUE;

	return FALSE;
}

static enum mtk_sim_type sim_type_for_fw_loading(const char *imsi)
{
	/* China Mobile MCC/MNC codes */
	const char *table_type1[] = { "46000", "46002", "46007" };
	/* China Unicom and China Telecom MCC/MNC codes */
	const char *table_type3[] =
		{ "46001", "46006", "46009", "45407", "46005", "45502" };

	if (find_in_table(imsi, table_type1,
				sizeof(table_type1)/sizeof(table_type1[0])))
		return MTK_SIM_TYPE_1;

	if (find_in_table(imsi, table_type3,
				sizeof(table_type3)/sizeof(table_type3[0])))
		return MTK_SIM_TYPE_3;

	return MTK_SIM_TYPE_2;
}

static int sim_modem_fw(const char *imsi)
{
	enum mtk_sim_type sim_type;
	int sim_fw = MTK_MD_TYPE_LWG;

	/* Find out our SIM type */
	sim_type = sim_type_for_fw_loading(imsi);
	switch (sim_type) {
	case MTK_SIM_TYPE_1:
		/* LTE TDD - TD-SCDMA - GSM */
		sim_fw = MTK_MD_TYPE_LTG;
		break;
	case MTK_SIM_TYPE_2:
	case MTK_SIM_TYPE_3:
		/* LTE FDD - WCDMA - GSM */
		sim_fw = MTK_MD_TYPE_LWG;
		break;
	};

	return sim_fw;
}

static void set_fw_type(struct ofono_modem *modem, int type)
{
	ofono_bool_t lte_cap;
	struct mtk_data *md = ofono_modem_get_data(modem);

	md->fw_type = type;

	lte_cap = (type >= MTK_MD_TYPE_LWG) ? TRUE : FALSE;
	ofono_modem_set_boolean(modem, MODEM_PROP_LTE_CAPABLE, lte_cap);
}

static void check_modem_fw(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);
	int sim_type;
	struct parcel rilp;

	/* We handle only LWG <-> LTG modem fw changes (arale case) */
	if (md->fw_type != MTK_MD_TYPE_LTG && md->fw_type != MTK_MD_TYPE_LWG)
		return;

	/* Right modem fw type for our SIM */
	sim_type = sim_modem_fw(ofono_sim_get_imsi(md->sim));

	if (md->fw_type == sim_type)
		return;

	set_fw_type(modem, sim_type);

	g_mtk_request_store_modem_type(md->ril, sim_type, &rilp);

	if (g_ril_send(md->ril, MTK_RIL_REQUEST_STORE_MODEM_TYPE, &rilp,
			store_type_cb, modem, NULL) == 0)
		ofono_error("%s: failure sending request", __func__);
}

static int mtk_probe(struct ofono_modem *modem)
{
	struct mtk_data *md = g_try_new0(struct mtk_data, 1);

	if (md == NULL) {
		errno = ENOMEM;
		goto error;
	}

	md->ofono_online = FALSE;
	md->radio_state = RADIO_STATE_UNAVAILABLE;

	md->slot = ofono_modem_get_integer(modem, "Slot");

	if (md->slot == MULTISIM_SLOT_0)
		mtk_data_0 = md;
	else
		mtk_data_1 = md;

	DBG("slot %d", md->slot);

	md->modem = modem;

	ofono_modem_set_data(modem, md);

	return 0;

error:
	g_free(md);

	return -errno;
}

static void mtk_remove(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);

	ofono_modem_set_data(modem, NULL);

	if (!md)
		return;

	g_ril_unref(md->ril);

	g_free(md);
}

static void mtk_pre_sim(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);

	DBG("slot %d", md->slot);
}

static void mtk_post_sim(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);

	DBG("slot %d", md->slot);
}

/*
 * sim_state_watch listens to SIM state changes and creates/removes atoms
 * accordingly. This is needed because we cannot rely on the modem core code,
 * which handles modem state transitions, to do this due to the SIM not being
 * accessible in the offline state for mtk modems. This causes a mismatch
 * between what the core thinks it can do in some states and what the mtk modem
 * can really do in those. This is a workaround to solve that.
 */
static void sim_state_watch(enum ofono_sim_state new_state, void *data)
{
	struct ofono_modem *modem = data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	if (new_state == OFONO_SIM_STATE_READY) {
		struct ofono_gprs_context *gc;
		struct ril_gprs_driver_data gprs_data = { md->ril, modem };
		struct ril_gprs_context_data inet_ctx =
			{ md->ril, OFONO_GPRS_CONTEXT_TYPE_INTERNET };
		struct ril_gprs_context_data mms_ctx =
			{ md->ril, OFONO_GPRS_CONTEXT_TYPE_MMS };

		DBG("SIM ready, creating more atoms");

		/*
		 * TODO: this function should setup:
		 *  - phonebook
		 *  - stk ( SIM toolkit )
		 *  - radio_settings
		 */
		md->sms = ofono_sms_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, md->ril);

		/* netreg needs access to the SIM (SPN, SPDI) */
		md->netreg = ofono_netreg_create(modem, OFONO_RIL_VENDOR_MTK,
							RILMODEM, md->ril);
		md->ussd = ofono_ussd_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, md->ril);
		md->call_settings =
			ofono_call_settings_create(modem, OFONO_RIL_VENDOR_MTK,
							RILMODEM, md->ril);
		md->call_forwarding =
			ofono_call_forwarding_create(modem,
							OFONO_RIL_VENDOR_MTK,
							RILMODEM, md->ril);
		md->call_barring =
			ofono_call_barring_create(modem, OFONO_RIL_VENDOR_MTK,
							RILMODEM, md->ril);
		md->phonebook =
			ofono_phonebook_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, modem);
		md->gprs = ofono_gprs_create(modem, OFONO_RIL_VENDOR_MTK,
						MTKMODEM, &gprs_data);

		gc = ofono_gprs_context_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, &inet_ctx);
		if (gc) {
			ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
			ofono_gprs_add_context(md->gprs, gc);
		}

		gc = ofono_gprs_context_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, &mms_ctx);
		if (gc) {
			ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_MMS);
			ofono_gprs_add_context(md->gprs, gc);
		}

		md->message_waiting = ofono_message_waiting_create(modem);
		if (md->message_waiting)
			ofono_message_waiting_register(md->message_waiting);

		/*
		 * Now that we can access IMSI, see if a FW change is needed.
		 * TODO: Roaming case and timeout case when no network is found.
		 */
		check_modem_fw(modem);

	} else if (new_state == OFONO_SIM_STATE_LOCKED_OUT) {

		DBG("SIM locked, removing atoms");

		if (md->message_waiting) {
			ofono_message_waiting_remove(md->message_waiting);
			md->message_waiting = NULL;
		}
		if (md->gprs) {
			ofono_gprs_remove(md->gprs);
			md->gprs = NULL;
		}
		if (md->phonebook) {
			ofono_phonebook_remove(md->phonebook);
			md->phonebook = NULL;
		}
		if (md->call_barring) {
			ofono_call_barring_remove(md->call_barring);
			md->call_barring = NULL;
		}
		if (md->call_forwarding) {
			ofono_call_forwarding_remove(md->call_forwarding);
			md->call_forwarding = NULL;
		}
		if (md->call_settings) {
			ofono_call_settings_remove(md->call_settings);
			md->call_settings = NULL;
		}
		if (md->ussd) {
			ofono_ussd_remove(md->ussd);
			md->ussd = NULL;
		}
		if (md->netreg) {
			ofono_netreg_remove(md->netreg);
			md->netreg = NULL;
		}
		if (md->sms) {
			ofono_sms_remove(md->sms);
			md->sms = NULL;
		}
	}
}

static void create_online_atoms(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);
	struct ril_radio_settings_driver_data rs_data = { md->ril, modem };

	md->sim_data.gril = md->ril;
	md->sim_data.modem = modem;
	md->sim_data.ril_state_watch = sim_state_watch;

	md->sim = ofono_sim_create(modem, OFONO_RIL_VENDOR_MTK,
					RILMODEM, &md->sim_data);
	g_assert(md->sim != NULL);

	/* Radio settings does not depend on the SIM */
	ofono_radio_settings_create(modem, OFONO_RIL_VENDOR_MTK,
					MTKMODEM, &rs_data);
}

static void query_type_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);
	int type;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	type = g_mtk_reply_parse_query_modem_type(md->ril, message);
	if (type < 0) {
		ofono_error("%s: parse error", __func__);
		return;
	}

	set_fw_type(modem, type);

	create_online_atoms(modem);
}

static void mtk_post_online(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);
	DBG("slot %d", md->slot);

	/* With modem powered we can query the type in krillin */
	if (g_ril_send(md->ril, MTK_RIL_REQUEST_QUERY_MODEM_TYPE,
			NULL, query_type_cb, md->modem, NULL) == 0)
		ofono_error("%s: failure sending QUERY_MODEM_TYPE", __func__);

	/* Register for changes in SIM insertion */
	g_ril_register(md->ril, MTK_RIL_UNSOL_SIM_PLUG_OUT,
			sim_removed, modem);
	g_ril_register(md->ril, MTK_RIL_UNSOL_SIM_PLUG_IN,
			sim_inserted, modem);
}

static void exec_pending_online(struct mtk_data *md)
{
	struct mtk_data *md_c;

	DBG("");

	mtk_data_0->pending_cb = NULL;

	/* Execute possible pending operation on the other modem */

	md_c = mtk_data_complement(md);

	if (md_c != NULL && md_c->pending_online_cbd) {
		struct cb_data *pending_cbd = md_c->pending_online_cbd;
		ofono_modem_online_cb_t pending_cb = pending_cbd->cb;

		mtk_set_online(pending_cbd->user, md_c->pending_online,
				pending_cb, pending_cbd->data);

		g_free(md_c->pending_online_cbd);
		md_c->pending_online_cbd = NULL;
	}
}

static gboolean sim_switch_failsafe(gpointer user_data)
{
	struct mtk_data *md = user_data;

	exec_online_callback(md);

	return FALSE;
}

static void mtk_sim_mode_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_modem *modem = cbd->user;
	struct mtk_data *md = ofono_modem_get_data(modem);

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(md->ril, message);
		/*
		 * Although the request was successful, radio state might not
		 * have changed yet. So we wait for the upcoming radio event,
		 * otherwise RIL requests that depend on radio state will fail.
		 * If we are switching the 3G slot, we cannot really trust this
		 * behaviour, so we add a failsafe timer.
		 */
		md->online_cb = cb;
		md->online_data = cbd->data;

		g_timeout_add(T_SIM_SWITCH_FAILSAFE_MS,
						sim_switch_failsafe, md);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		exec_pending_online(md);
	}
}

static int sim_state()
{
	int state = mtk_data_0->ofono_online ? SIM_1_ACTIVE : NO_SIM_ACTIVE;
	if (mtk_data_1 && mtk_data_1->ofono_online)
		state |= SIM_2_ACTIVE;

	return state;
}

static void mtk_send_sim_mode(GRilResponseFunc func, gpointer user_data)
{
	struct parcel rilp;
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = NULL;
	GDestroyNotify notify = NULL;
	int sim_mode;

	if (cbd != NULL) {
		notify = g_free;
		cb = cbd->cb;
	}

	/* Case of modems with just one slot */
	if (mtk_data_1 == NULL) {
		mtk_data_0->pending_cb = NULL;

		if (cbd != NULL) {
			CALLBACK_WITH_SUCCESS(cb, cbd->data);
			g_free(cbd);
		}
		return;
	}

	sim_mode = sim_state();

	if (sim_mode == NO_SIM_ACTIVE)
		sim_mode = MTK_SWITCH_MODE_ALL_INACTIVE;

	g_mtk_request_dual_sim_mode_switch(mtk_data_0->ril, sim_mode, &rilp);

	/* This request is always sent through the main socket */
	if (g_ril_send(mtk_data_0->ril, MTK_RIL_REQUEST_DUAL_SIM_MODE_SWITCH,
			&rilp, func, cbd, notify) == 0 && cbd != NULL) {
		ofono_error("%s: failure sending request", __func__);
		mtk_data_0->pending_cb = NULL;

		if (cbd != NULL) {
			CALLBACK_WITH_FAILURE(cb, cbd->data);
			g_free(cbd);
		}
	}
}

static gboolean no_disconnect_case(gpointer user_data)
{
	struct cb_data *cbd = user_data;

	ofono_info("%s: Execute pending sim mode switch", __func__);
	not_disconn_cb_id = 0;

	mtk_send_sim_mode(mtk_sim_mode_cb, cbd);

	return FALSE;
}

static void poweron_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->user;
	struct mtk_data *md = ofono_modem_get_data(modem);
	ofono_modem_online_cb_t cb = cbd->cb;

	DBG("");

	/*
	 * MTK's rild behavior when a POWERON is sent to it is different
	 * depending on whether a previous POWEROFF had been sent. When
	 * the modem is initialized during device startup, POWERON is
	 * sent without a prior POWEROFF, rild responds with an OK reply,
	 * and the modem is brought up. Any subsequent POWERON requests
	 * are sent whenever both modems have been offlined before ( meaning a
	 * POWEROFF has been sent prior ). rild may respond to the POWERON
	 * request, but will usually ( always? ) trigger a socket disconnect in
	 * this case.
	 *
	 * This means there's a race condition between the POWERON reply
	 * callback and the socket disconnect function ( which triggers a
	 * SIM_MODE_SWITCH request ). In some cases rild is slower than
	 * usual closing the socket, so we add a timeout to avoid following
	 * the code path used when there is not a disconnection. Otherwise,
	 * there would be a race and some requests would return errors due to
	 * having been sent through the about-to-be-disconnected socket, leaving
	 * ofono in an inconsistent state. So, we delay sending the
	 * SIM_MODE_SWITCH for 1s, to allow the disconnect to happen when we
	 * know that we have sent previously a POWEROFF.
	 *
	 * Also, I saw once that sending SIM_MODE while the
	 * socket was being disconnected provoked a crash due to SIGPIPE being
	 * issued. The timeout should also fix this.
	 */

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(md->ril, message);

		if (disconnect_expected)
			not_disconn_cb_id = g_timeout_add(T_WAIT_DISCONN_MS,
						no_disconnect_case, cbd);
		else
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

static void poweroff_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_modem *modem = cbd->user;
	struct mtk_data *md = ofono_modem_get_data(modem);

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(md->ril, message);

		mtk_settings_remove(md->mtk_settings);

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static int power_on_off(GRil *ril, gboolean on, struct cb_data *cbd)
{
	int cancel_id;
	int req;
	struct parcel rilp;
	struct parcel *p_rilp;
	GRilResponseFunc resp;
	GDestroyNotify notify;
	ofono_modem_online_cb_t cb = cbd->cb;

	/* Case of modems with just one slot */
	if (mtk_data_1 == NULL) {
		/* Fall back to generic RIL_REQUEST_RADIO_POWER */
		req = RIL_REQUEST_RADIO_POWER;
		g_ril_request_power(ril, on, &rilp);
		p_rilp = &rilp;
	} else {
		req = on ? MTK_RIL_REQUEST_RADIO_POWERON
			: MTK_RIL_REQUEST_RADIO_POWEROFF;
		p_rilp = NULL;
	}

	if (on) {
		resp = poweron_cb;
		notify = NULL;
	} else {
		resp = poweroff_cb;
		notify = g_free;
	}

	if ((cancel_id = g_ril_send(ril, req, p_rilp, resp, cbd, notify))
			== 0) {
		ofono_error("%s: failure sending request", __func__);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
		return 0;
	}

	return cancel_id;
}

static void mtk_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t callback, void *data)
{
	struct mtk_data *md = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(callback, data, modem);
	ofono_modem_online_cb_t cb = cbd->cb;
	int current_state, next_state;

	/*
	 * Serialize online requests to avoid incoherent states. When changing
	 * the online state of *one* of the modems, we need to send a
	 * DUAL_SIM_MODE_SWITCH request, which affects *both* modems. Also, when
	 * we want to online one modem and at that time both modems are
	 * offline a RADIO_POWERON needs to be sent before DUAL_SIM_MODE_SWITCH,
	 * with the additional complexity of being disconnected from the rild
	 * socket while doing the sequence. This can take some time, and we
	 * cannot change the state of the other modem while the sequence is
	 * happenig, as DUAL_SIM_MODE_SWITCH affects both states. Therefore, we
	 * need to do this serialization, which is different from the one done
	 * per modem by ofono core.
	 */
	if (mtk_data_0->pending_cb != NULL) {
		md->pending_online_cbd = cbd;
		md->pending_online = online;
		return;
	}

	current_state = sim_state();

	md->ofono_online = online;

	/* Changes as md points to either mtk_data_0 or mtk_data_1 variables */
	next_state = sim_state();

	DBG("setting md_%d->ofono_online to: %d (from %d to %d)",
		md->slot, online, current_state, next_state);

	if (current_state == next_state) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		g_free(cbd);
		return;
	}

	/* Reset mtk_data variables */
	if (online == FALSE)
		md->sim_status_retries = 0;

	if (current_state == NO_SIM_ACTIVE) {
		/* Old state was off, need to power on the modem */
		if (power_on_off(mtk_data_0->ril, TRUE, cbd)) {
			/* Socket might disconnect... failsafe */
			mtk_data_0->pending_cb = poweron_disconnect;
			mtk_data_0->pending_cbd = cbd;
		}
	} else if (next_state == NO_SIM_ACTIVE) {
		if (power_on_off(mtk_data_0->ril, FALSE, cbd))
			disconnect_expected = TRUE;
	} else {
		mtk_send_sim_mode(mtk_sim_mode_cb, cbd);
	}
}

static void set_online_cb(const struct ofono_error *error, void *data)
{
	if (mtk_data_1->ofono_online_target && !mtk_data_1->ofono_online)
		mtk_set_online(mtk_data_1->modem, TRUE, set_online_cb, NULL);
}

static void set_offline_cb(const struct ofono_error *error, void *data)
{
	if (mtk_data_1->ofono_online)
		mtk_set_online(mtk_data_1->modem, FALSE, set_offline_cb, NULL);
	else if (mtk_data_0->ofono_online_target)
		mtk_set_online(mtk_data_0->modem, TRUE, set_online_cb, NULL);
	else
		mtk_set_online(mtk_data_1->modem, TRUE, set_online_cb, NULL);
}

void mtk_reset_all_modems(void)
{
	if (!mtk_data_0->ofono_online && !mtk_data_1->ofono_online)
		return;

	mtk_data_0->ofono_online_target = mtk_data_0->ofono_online;
	mtk_data_1->ofono_online_target = mtk_data_1->ofono_online;

	ofono_modem_set_powered(mtk_data_0->modem, FALSE);
	ofono_modem_set_powered(mtk_data_1->modem, FALSE);

	if (mtk_data_0->ofono_online)
		mtk_set_online(mtk_data_0->modem, FALSE, set_offline_cb, NULL);
	else
		mtk_set_online(mtk_data_1->modem, FALSE, set_offline_cb, NULL);
}

static void create_atoms_on_connection(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);
	struct ril_voicecall_driver_data vc_data = { md->ril, modem };

	md->devinfo = ofono_devinfo_create(modem, OFONO_RIL_VENDOR_MTK,
						RILMODEM, md->ril);

	/* Create interfaces useful for emergency calls */
	md->voicecall = ofono_voicecall_create(modem, OFONO_RIL_VENDOR_MTK,
						MTKMODEM, &vc_data);
	md->callvolume = ofono_call_volume_create(modem, OFONO_RIL_VENDOR_MTK,
							RILMODEM, md->ril);
}

static void remove_atoms_on_disconnection(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);

	if (__ofono_modem_find_atom(modem, OFONO_ATOM_TYPES_CALL_VOLUME))
		ofono_call_volume_remove(md->callvolume);
	md->callvolume = NULL;
	if (__ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_VOICECALL))
		ofono_voicecall_remove(md->voicecall);
	md->voicecall = NULL;
	if (__ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_DEVINFO))
		ofono_devinfo_remove(md->devinfo);
	md->devinfo = NULL;
}

static void start_slot(struct mtk_data *md, struct socket_data *sock,
			const char *hex_prefix)
{
	ofono_info("Physical slot %d in socket %s", md->slot, sock->path);

	md->ril = sock->ril;
	md->radio_state = sock->radio_state;

	g_ril_set_slot(md->ril, md->slot);

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(md->ril, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(md->ril, mtk_debug, (char *) hex_prefix);

	g_ril_set_disconnect_function(md->ril, socket_disconnected,
					md->modem);

	g_ril_unregister(sock->ril, sock->radio_state_ev_id);

	g_ril_register(md->ril, RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
			mtk_radio_state_changed, md->modem);

	mtk_connected(md->modem);
}

static void query_3g_caps_cb(struct ril_msg *message, gpointer user_data)
{
	struct socket_data *sock = user_data;
	struct socket_data *sock_for_md_0, *sock_for_md_1;
	int slot_3g;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	slot_3g = g_mtk_reply_parse_get_3g_capability(sock->ril, message);

	/*
	 * The socket at sock_slot_0 always connects to the slot with 3G
	 * capabilities, while sock_slot_1 connects to the slot that is just 2G.
	 * However, the physical slot that owns the 3G capabilities can be
	 * changed dynamically using a RILd request, so the sockets can connect
	 * to different physical slots depending on the current configuration.
	 * We want to keep the relationship between the physical slots and
	 * the modem names in DBus (so /ril_0 and /ril_1 always refer to the
	 * same physical slots), so here we assign the sockets needed by
	 * mtk_data_0 and mtk_data_1 structures to make sure that happens.
	 */
	if (slot_3g == MULTISIM_SLOT_0) {
		sock_for_md_0 = sock_0;
		sock_for_md_1 = sock_1;
		mtk_data_0->has_3g = TRUE;
		mtk_data_1->has_3g = FALSE;
	} else {
		sock_for_md_0 = sock_1;
		sock_for_md_1 = sock_0;
		mtk_data_0->has_3g = FALSE;
		mtk_data_1->has_3g = TRUE;
	}

	start_slot(mtk_data_0, sock_for_md_0, hex_slot_0);
	start_slot(mtk_data_1, sock_for_md_1, hex_slot_1);

	g_free(sock_0);
	sock_0 = NULL;
	g_free(sock_1);
	sock_1 = NULL;
}

static void query_3g_caps(struct socket_data *sock)
{
	if (g_ril_send(sock->ril, MTK_RIL_REQUEST_GET_3G_CAPABILITY, NULL,
			query_3g_caps_cb, sock, NULL) <= 0)
		ofono_error("%s Error querying 3G capabilities", __func__);
}

static gboolean mtk_connected(gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	ofono_info("[slot %d] CONNECTED", md->slot);

	DBG("calling set_powered(TRUE)");

	if (!ofono_modem_get_powered(modem))
		ofono_modem_set_powered(modem, TRUE);

	create_atoms_on_connection(modem);

	if (md->pending_cb)
		md->pending_cb(md->pending_cbd);

	/* Call the function just once */
	return FALSE;
}

static gboolean reconnect_rild(gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	ofono_info("[slot %d] trying to reconnect", md->slot);

	if (create_gril(modem) < 0)
		return TRUE;

	/* Reconnected: do not call this again */
	return FALSE;
}

#define WAIT_FOR_RILD_TO_RESTART_MS 8000	/* Milliseconds */

static void socket_disconnected(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	DBG("slot %d", md->slot);

	/* Atoms use old gril object, remove and recreate later */
	remove_atoms_on_disconnection(modem);

	g_ril_unref(md->ril);
	md->ril = NULL;

	/* Disconnection happened so we do not call failsafe function */
	if (not_disconn_cb_id != 0) {
		g_source_remove(not_disconn_cb_id);
		not_disconn_cb_id = 0;
	}

	/* The disconnection happens because rild is re-starting, wait for it */
	g_timeout_add(WAIT_FOR_RILD_TO_RESTART_MS, reconnect_rild, modem);
}

static const char sock_slot_0[] = "/dev/socket/rild";
static const char sock_slot_1[] = "/dev/socket/rild2";

static int create_gril(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);
	struct socket_data *sock;
	int sock_num;

	DBG("slot %d", md->slot);

	if (md->ril != NULL)
		return 0;

	sock = g_try_malloc0(sizeof(*sock));
	if (sock == NULL) {
		ofono_error("%s: Cannot allocate socket_data", __func__);
		return -ENOMEM;
	}

	if (md->slot == MULTISIM_SLOT_0) {
		sock_num = SOCKET_NUM_FOR_DBG_0;
		sock->path = sock_slot_0;
	} else {
		sock_num = SOCKET_NUM_FOR_DBG_1;
		sock->path = sock_slot_1;
	}

	/* Opens the socket to RIL */
	sock->ril = g_ril_new(sock->path, OFONO_RIL_VENDOR_MTK);

	/*
	 * NOTE: Since AT modems open a tty, and then call
	 * g_at_chat_new(), they're able to return -EIO if
	 * the first fails, and -ENOMEM if the second fails.
	 * in our case, we already return -EIO if the ril_new
	 * fails.  If this is important, we can create a ril_socket
	 * abstraction... ( probaby not a bad idea ).
	 */

	if (sock->ril == NULL) {
		ofono_error("g_ril_new() failed to connect to %s!", sock->path);
		g_free(sock);
		return -EIO;
	} else if (md->slot == MULTISIM_SLOT_0) {
		sock_0 = sock;
	} else {
		sock_1 = sock;
	}

	sock->radio_state = RADIO_STATE_UNAVAILABLE;
	sock->radio_state_ev_id =
		g_ril_register(sock->ril,
				RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
				radio_state_changed, sock);

	g_ril_register(sock->ril, MTK_RIL_UNSOL_RESPONSE_REGISTRATION_SUSPENDED,
			reg_suspended, modem);

	/* sock_num is negative to avoid confusion with physical slots */
	g_ril_set_slot(sock->ril, sock_num);

	g_ril_set_vendor_print_msg_id_funcs(sock->ril,
						mtk_request_id_to_string,
						mtk_unsol_request_to_string);

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(sock->ril, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(sock->ril, mtk_debug, (char *) sock->path);

	return 0;
}

static gboolean connect_rild(gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct mtk_data *md = ofono_modem_get_data(modem);

	ofono_info("Trying to reconnect to slot %d...", md->slot);

	if (md->rild_connect_retries++ < RILD_MAX_CONNECT_RETRIES) {
		if (create_gril(modem) < 0)
			return TRUE;
	} else {
		ofono_error("Exiting, can't connect to rild.");
		exit(0);
	}

	return FALSE;
}

static int mtk_enable(struct ofono_modem *modem)
{
	int ret;

	/* We handle SIM states due to MTK peculiarities */
	ofono_modem_set_driver_watches_sim(modem, TRUE);

	ret = create_gril(modem);
	if (ret < 0)
		g_timeout_add_seconds(RILD_CONNECT_RETRY_TIME_S,
					connect_rild, modem);

	/*
	 * We will mark the modem as powered when we receive an event that
	 * confirms that the radio is in a state different from unavailable
	 */

	return -EINPROGRESS;
}

static int mtk_disable(struct ofono_modem *modem)
{
	struct mtk_data *md = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (md->slot == MULTISIM_SLOT_0 && not_disconn_cb_id != 0) {
		g_source_remove(not_disconn_cb_id);
		not_disconn_cb_id = 0;
	}

	if (md->ofono_online) {
		md->ofono_online = FALSE;
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
