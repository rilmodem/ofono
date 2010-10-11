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
#include <gisi/client.h>

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
#include <ofono/ssn.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/call-meter.h>
#include <ofono/radio-settings.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>

#include "drivers/isimodem/isimodem.h"
#include "drivers/isimodem/isiutil.h"
#include "drivers/isimodem/mtc.h"
#include "drivers/isimodem/debug.h"

struct isi_data {
	struct ofono_modem *modem;
	char const *ifname;
	GIsiModem *idx;
	GIsiClient *client;
	GPhonetNetlink *link;
	GPhonetLinkState linkstate;
	unsigned interval;
	int reported;
	ofono_bool_t online;
	struct isi_cb_data *online_cbd;
};

static void report_powered(struct isi_data *isi, ofono_bool_t powered)
{
	if (powered != isi->reported)
		ofono_modem_set_powered(isi->modem, isi->reported = powered);
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

static void set_power_by_mtc_state(struct isi_data *isi, int mtc_state)
{
	if (isi->online_cbd)
		report_online(isi, mtc_state == MTC_NORMAL);

	switch (mtc_state) {
	case MTC_STATE_NONE:
	case MTC_POWER_OFF:
	case MTC_CHARGING:
	case MTC_SELFTEST_FAIL:
		report_powered(isi, 0);
		break;

	case MTC_RF_INACTIVE:
	case MTC_NORMAL:
	default:
		report_powered(isi, 1);
	}
}

static void mtc_state_ind_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return;
	}

	if (len < 3 || msg[0] != MTC_STATE_INFO_IND)
		return;

	if (msg[2] == MTC_START) {
		DBG("target modem state: %s (0x%02X)",
			mtc_modem_state_name(msg[1]), msg[1]);
	} else if (msg[2] == MTC_READY) {
		DBG("current modem state: %s (0x%02X)",
			mtc_modem_state_name(msg[1]), msg[1]);
		set_power_by_mtc_state(isi, msg[1]);
	}
}

static gboolean mtc_poll_query_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		const unsigned char req[] = {
			MTC_STATE_QUERY_REQ, 0x00, 0x00
		};

		if (isi->linkstate != PN_LINK_UP)
			return TRUE;

		isi->interval *= 2;
		if (isi->interval >= 20)
			isi->interval = 20;

		g_isi_send(client, req, sizeof(req), isi->interval,
				mtc_poll_query_cb, opaque, NULL);

		return TRUE;
	}

	if (len < 3 || msg[0] != MTC_STATE_QUERY_RESP)
		return FALSE;

	g_isi_subscribe(client, MTC_STATE_INFO_IND, mtc_state_ind_cb, opaque);

	DBG("current modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[1]), msg[1]);
	DBG("target modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[2]), msg[2]);

	if (msg[1] == msg[2])
		set_power_by_mtc_state(isi, msg[1]);

	return TRUE;
}

static gboolean mtc_query_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return TRUE;
	}

	if (len < 3 || msg[0] != MTC_STATE_QUERY_RESP)
		return FALSE;

	DBG("current modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[1]), msg[1]);
	DBG("target modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[2]), msg[2]);

	if (msg[1] == msg[2])
		set_power_by_mtc_state(isi, msg[1]);

	return TRUE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	struct isi_data *isi = opaque;

	const unsigned char msg[] = {
		MTC_STATE_QUERY_REQ,
		0x00, 0x00 /* Filler */
	};

	if (!alive) {
		DBG("MTC client: %s", strerror(-g_isi_client_error(client)));

		if (isi->linkstate == PN_LINK_UP)
			g_isi_send(client, msg, sizeof(msg),
					isi->interval = MTC_TIMEOUT,
					mtc_poll_query_cb, opaque, NULL);
		return;
	}

	DBG("%s (v.%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_isi_subscribe(client, MTC_STATE_INFO_IND, mtc_state_ind_cb, opaque);
	g_isi_send(client, msg, sizeof(msg), MTC_TIMEOUT,
			mtc_query_cb, opaque, NULL);
}

static void phonet_status_cb(GIsiModem *idx,
				GPhonetLinkState st,
				char const *ifname,
				void *data)
{
	struct ofono_modem *modem = data;
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("Link %s (%u) is %s",
		isi->ifname, g_isi_modem_index(isi->idx),
		st == PN_LINK_REMOVED ? "removed" :
		st == PN_LINK_DOWN ? "down" : "up");

	isi->linkstate = st;

	if (st == PN_LINK_UP)
		g_isi_verify(isi->client, reachable_cb, isi);
	else if (st == PN_LINK_DOWN)
		set_power_by_mtc_state(isi, MTC_STATE_NONE);
}

static int isigen_probe(struct ofono_modem *modem)
{
	struct isi_data *isi;
	const char *ifname = ofono_modem_get_string(modem, "Interface");
	unsigned address = ofono_modem_get_integer(modem, "Address");
	GIsiModem *idx;
	GPhonetNetlink *link;

	if (ifname == NULL)
		return -EINVAL;

	DBG("(%p) with %s", modem, ifname);

	idx = g_isi_modem_by_name(ifname);
	if (idx == NULL) {
		DBG("Interface=%s: %s", ifname, strerror(errno));
		return -errno;
	}

	if (g_pn_netlink_by_modem(idx)) {
		DBG("%s: %s", ifname, strerror(EBUSY));
		return -EBUSY;
	}

	link = g_pn_netlink_start(idx, phonet_status_cb, modem);
	if (!link) {
		DBG("%s: %s", ifname, strerror(errno));
		return -errno;
	}

	if (address) {
		int error = g_pn_netlink_set_address(idx, PN_DEV_PC);
		if (error && error != -EEXIST) {
			DBG("g_pn_netlink_set_address: %s\n", strerror(-error));
			return -errno;
		}
	}

	isi = g_new0(struct isi_data, 1);
	if (isi == NULL)
		return -ENOMEM;

	ofono_modem_set_data(isi->modem = modem, isi);

	isi->idx = idx;
	isi->ifname = ifname;
	isi->link = link;
	isi->client = g_isi_client_create(isi->idx, PN_MTC);
	isi->reported = -1;

	return 0;
}

static void isigen_remove(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	if (!isi)
		return;

	ofono_modem_set_data(modem, NULL);
	g_isi_client_destroy(isi->client);
	g_pn_netlink_stop(isi->link);
	g_free(isi);
}

static gboolean mtc_state_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	struct ofono_modem *modem = cbd->user;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct isi_data *isi = ofono_modem_get_data(modem);
	const unsigned char *msg = data;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto err;
	}

	if (len < 3 || msg[0] != MTC_STATE_RESP)
		return FALSE;

	DBG("cause: %s (0x%02X)", mtc_isi_cause_name(msg[1]), msg[1]);

	if (msg[1] == MTC_OK) {
		isi->online_cbd = cbd;
		return TRUE;
	}

err:
	if (msg && msg[1] == MTC_ALREADY_ACTIVE)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
	return TRUE;
}

static void isigen_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *data)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	const unsigned char req[] = {
		MTC_STATE_REQ, online ? MTC_NORMAL : MTC_RF_INACTIVE, 0x00
	};
	struct isi_cb_data *cbd = isi_cb_data_new(modem, cb, data);

	DBG("(%p) with %s", modem, isi->ifname);

	if (!cbd)
		goto error;

	isi->online = online;

	if (g_isi_send(isi->client, req, sizeof(req), MTC_TIMEOUT,
			mtc_state_cb, cbd, NULL))
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isigen_pre_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("(%p) with %s", modem, isi->ifname);

	ofono_sim_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_devinfo_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_voicecall_create(isi->modem, 0, "isimodem", isi->idx);
}

static void isigen_post_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("(%p) with %s", modem, isi->ifname);

	ofono_phonebook_create(isi->modem, 0, "isimodem", isi->idx);
}

static void isigen_post_online(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("(%p) with %s", modem, isi->ifname);

	ofono_netreg_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_sms_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_cbs_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_ssn_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_ussd_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_forwarding_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_settings_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_barring_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_meter_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_radio_settings_create(isi->modem, 0, "isimodem", isi->idx);
	gprs = ofono_gprs_create(isi->modem, 0, "isimodem", isi->idx);
	gc = ofono_gprs_context_create(isi->modem, 0, "isimodem", isi->idx);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
	else
		DBG("Failed to add context");
}

static int isigen_enable(struct ofono_modem *modem)
{
	return 0;
}

static int isigen_disable(struct ofono_modem *modem)
{
	return 0;
}

static struct ofono_modem_driver driver = {
	.name = "isigen",
	.probe = isigen_probe,
	.remove = isigen_remove,
	.set_online = isigen_online,
	.pre_sim = isigen_pre_sim,
	.post_sim = isigen_post_sim,
	.post_online = isigen_post_online,
	.enable = isigen_enable,
	.disable = isigen_disable,
};

static int isigen_init(void)
{
	return ofono_modem_driver_register(&driver);
}

static void isigen_exit(void)
{
	ofono_modem_driver_unregister(&driver);
}

OFONO_PLUGIN_DEFINE(isigen, "Generic modem driver for isi",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			isigen_init, isigen_exit)
