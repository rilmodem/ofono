/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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

#include <gisi/netlink.h>
#include <gisi/modem.h>
#include <gisi/client.h>
#include <gisi/message.h>

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
#include <ofono/radio-settings.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/message-waiting.h>

#include "drivers/isimodem/isimodem.h"
#include "drivers/isimodem/isiutil.h"
#include "drivers/isimodem/mtc.h"
#include "drivers/isimodem/debug.h"

struct isi_data {
	char const *ifname;
	GIsiModem *modem;
	GIsiClient *client;
	GIsiPhonetNetlink *link;
	enum GIsiPhonetLinkState linkstate;
	unsigned interval;
	int reported;
	ofono_bool_t online;
	struct isi_cb_data *online_cbd;
};

static gboolean check_response_status(const GIsiMessage *msg, uint8_t msgid)
{
	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			mtc_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}
	return TRUE;
}

static void report_powered(struct ofono_modem *modem, struct isi_data *isi,
				ofono_bool_t powered)
{
	if (powered == isi->reported)
		return;

	isi->reported = powered;
	ofono_modem_set_powered(modem, powered);
}

static void report_online(struct isi_data *isi, ofono_bool_t online)
{
	struct isi_cb_data *cbd = isi->online_cbd;
	ofono_modem_online_cb_t cb = cbd->cb;

	isi->online_cbd = NULL;

	if (isi->online == online)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void set_power_by_mtc_state(struct ofono_modem *modem,
					struct isi_data *isi, int mtc_state)
{
	if (isi->online_cbd)
		report_online(isi, mtc_state == MTC_NORMAL);

	switch (mtc_state) {
	case MTC_STATE_NONE:
	case MTC_POWER_OFF:
	case MTC_CHARGING:
	case MTC_SELFTEST_FAIL:
		report_powered(modem, isi, FALSE);
		break;

	case MTC_RF_INACTIVE:
	case MTC_NORMAL:
	default:
		report_powered(modem, isi, TRUE);
	}
}

static void mtc_state_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct isi_data *isi = ofono_modem_get_data(modem);
	uint8_t state;
	uint8_t action;

	if (!isi || g_isi_msg_id(msg) != MTC_STATE_INFO_IND)
		return;

	if (!g_isi_msg_data_get_byte(msg, 0, &state) ||
			!g_isi_msg_data_get_byte(msg, 1, &action))
		return;

	switch (action) {
	case MTC_START:
		DBG("target modem state: %s (0x%02X)",
			mtc_modem_state_name(state), state);
		break;

	case MTC_READY:
		DBG("current modem state: %s (0x%02X)",
			mtc_modem_state_name(state), state);
		set_power_by_mtc_state(modem, isi, state);
		break;
	}
}

static void mtc_query_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct isi_data *isi = ofono_modem_get_data(modem);
	uint8_t current;
	uint8_t target;

	if (!check_response_status(msg, MTC_STATE_QUERY_RESP))
		return;

	if (!g_isi_msg_data_get_byte(msg, 0, &current) ||
			!g_isi_msg_data_get_byte(msg, 1, &target))
		return;

	DBG("Modem state: current=%s (0x%02X) target=%s (0x%02X)",
		mtc_modem_state_name(current), current,
		mtc_modem_state_name(target), target);

	if (current == target)
		set_power_by_mtc_state(modem, isi, current);
}

static gboolean bootstrap_current_state(gpointer user)
{
	struct ofono_modem *om = user;
	struct isi_data *isi = ofono_modem_get_data(om);

	const uint8_t req[] = {
		MTC_STATE_QUERY_REQ,
		0x00, 0x00 /* Filler */
	};
	size_t len = sizeof(req);

	g_isi_client_send(isi->client, req, len, mtc_query_cb, om, NULL);

	return FALSE;
}

static void reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_modem *om = data;
	struct isi_data *isi = ofono_modem_get_data(om);

	if (!g_isi_msg_error(msg) < 0)
		return;

	ISI_RESOURCE_DBG(msg);

	g_isi_client_ind_subscribe(isi->client, MTC_STATE_INFO_IND,
					mtc_state_ind_cb, om);

	/*
	 * FIXME: There is a theoretical race condition here:
	 * g_isi_client_ind_subscribe() adds the actual message
	 * sending for committing changes to subscriptions in idle
	 * loop, which may or may not preserve ordering.  Thus, we
	 * might miss a state indication if the bootstrap request ends
	 * up being sent first.
	 */
	g_idle_add(bootstrap_current_state, om);
}

static void phonet_status_cb(GIsiModem *modem, enum GIsiPhonetLinkState st,
				char const *ifname, void *data)
{
	struct ofono_modem *om = data;
	struct isi_data *isi = ofono_modem_get_data(om);

	DBG("Link %s (%u) is %s", isi->ifname, g_isi_modem_index(isi->modem),
		st == PN_LINK_REMOVED ? "removed" :
		st == PN_LINK_DOWN ? "down" : "up");

	isi->linkstate = st;

	if (st == PN_LINK_UP)
		g_isi_client_verify(isi->client, reachable_cb, om, NULL);
	else if (st == PN_LINK_DOWN)
		set_power_by_mtc_state(om, isi, MTC_STATE_NONE);
}

static int isiusb_probe(struct ofono_modem *modem)
{
	const char *ifname = ofono_modem_get_string(modem, "Interface");
	unsigned address = ofono_modem_get_integer(modem, "Address");
	GIsiModem *isimodem;
	GIsiClient *client = NULL;
	GIsiPhonetNetlink *link = NULL;
	struct isi_data *isi = NULL;

	if (!ifname)
		return -EINVAL;

	DBG("(%p) with %s", modem, ifname);

	isimodem = g_isi_modem_create_by_name(ifname);
	if (!isimodem) {
		DBG("Interface=%s: %s", ifname, strerror(errno));
		return -errno;
	}

	g_isi_modem_set_userdata(isimodem, modem);
	g_isi_modem_set_flags(isimodem, GISI_MODEM_FLAG_USE_LEGACY_SUBSCRIBE);

	if (getenv("OFONO_ISI_DEBUG"))
		g_isi_modem_set_debug(isimodem, ofono_debug);

	if (getenv("OFONO_ISI_TRACE"))
		g_isi_modem_set_trace(isimodem, isi_trace);

	if (g_isi_pn_netlink_by_modem(isimodem)) {
		DBG("%s: %s", ifname, strerror(EBUSY));
		errno = EBUSY;
		goto error;
	}

	link = g_isi_pn_netlink_start(isimodem, phonet_status_cb, modem);
	if (link == NULL) {
		DBG("%s: %s", ifname, strerror(errno));
		goto error;
	}

	if (address) {
		int error = g_isi_pn_netlink_set_address(isimodem, address);
		if (error && error != -EEXIST) {
			DBG("g_isi_pn_netlink_set_address(): %s\n",
				strerror(-error));
			errno = -error;
			goto error;
		}
	}

	isi = g_try_new0(struct isi_data, 1);
	if (!isi) {
		errno = ENOMEM;
		goto error;
	}

	client = g_isi_client_create(isimodem, PN_MTC);
	if (!client)
		goto error;

	isi->modem = isimodem;
	isi->ifname = ifname;
	isi->link = link;
	isi->reported = -1;
	isi->client = client;

	ofono_modem_set_data(modem, isi);
	return 0;

error:
	g_isi_pn_netlink_stop(link);
	g_isi_client_destroy(client);
	g_isi_modem_destroy(isimodem);
	g_free(isi);

	return -errno;
}

static void isiusb_remove(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	ofono_modem_set_data(modem, NULL);

	if (isi == NULL)
		return;

	g_isi_pn_netlink_stop(isi->link);
	g_isi_client_destroy(isi->client);
	g_isi_modem_destroy(isi->modem);
	g_free(isi);
}

static void mtc_state_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	struct ofono_modem *modem = cbd->user;
	ofono_modem_online_cb_t cb = cbd->cb;

	struct isi_data *isi = ofono_modem_get_data(modem);
	uint8_t cause;

	if (!check_response_status(msg, MTC_STATE_RESP))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 0, &cause))
		goto error;

	DBG("MTC cause: %s (0x%02X)", mtc_isi_cause_name(cause), cause);

	if (cause == MTC_OK || cause == MTC_STATE_TRANSITION_GOING_ON) {
		isi->online_cbd = cbd;
		return;
	}

	if (cause == MTC_ALREADY_ACTIVE) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		g_free(cbd);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void isiusb_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *data)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	struct isi_cb_data *cbd = isi_cb_data_new(modem, cb, data);
	const uint8_t req[] = {
		MTC_STATE_REQ,
		online ? MTC_NORMAL : MTC_RF_INACTIVE,
		0x00
	};

	DBG("(%p) with %s", modem, isi->ifname);

	if (cbd == NULL || isi == NULL)
		goto error;

	if (g_isi_client_send_with_timeout(isi->client, req, sizeof(req),
				MTC_STATE_REQ_TIMEOUT,
				mtc_state_cb, cbd, NULL)) {
		isi->online = online;
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void isiusb_pre_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("(%p) with %s", modem, isi->ifname);

	ofono_sim_create(modem, 0, "isimodem", isi->modem);
	ofono_devinfo_create(modem, 0, "isimodem", isi->modem);
	ofono_voicecall_create(modem, 0, "isimodem", isi->modem);
}

static void isiusb_post_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("(%p) with %s", modem, isi->ifname);

	ofono_phonebook_create(modem, 0, "isimodem", isi->modem);
	ofono_call_forwarding_create(modem, 0, "isimodem", isi->modem);
	ofono_radio_settings_create(modem, 0, "isimodem", isi->modem);
}

static void isiusb_post_online(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	DBG("(%p) with %s", modem, isi->ifname);

	ofono_netreg_create(modem, 0, "isimodem", isi->modem);
	ofono_sms_create(modem, 0, "isimodem", isi->modem);
	ofono_cbs_create(modem, 0, "isimodem", isi->modem);
	ofono_ussd_create(modem, 0, "isimodem", isi->modem);
	ofono_call_settings_create(modem, 0, "isimodem", isi->modem);
	ofono_call_barring_create(modem, 0, "isimodem", isi->modem);
	ofono_call_meter_create(modem, 0, "isimodem", isi->modem);
	ofono_gprs_create(modem, 0, "isimodem", isi->modem);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static int isiusb_enable(struct ofono_modem *modem)
{
	return 0;
}

static int isiusb_disable(struct ofono_modem *modem)
{
	return 0;
}

static struct ofono_modem_driver driver = {
	.name = "isiusb",
	.probe = isiusb_probe,
	.remove = isiusb_remove,
	.set_online = isiusb_online,
	.pre_sim = isiusb_pre_sim,
	.post_sim = isiusb_post_sim,
	.post_online = isiusb_post_online,
	.enable = isiusb_enable,
	.disable = isiusb_disable,
};

static int isiusb_init(void)
{
	return ofono_modem_driver_register(&driver);
}

static void isiusb_exit(void)
{
	ofono_modem_driver_unregister(&driver);
}

OFONO_PLUGIN_DEFINE(isiusb, "Generic modem driver for isi",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			isiusb_init, isiusb_exit)
