/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
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

#include "isimodem.h"
#include "isiutil.h"
#include "mtc.h"
#include "debug.h"

struct isi_data {
	struct ofono_modem *modem;
	GIsiModem *idx;
	GIsiClient *client;
};

static GPhonetNetlink *link = NULL;
static GSList *g_modems = NULL;

static struct isi_data *find_modem_by_idx(GSList *modems, GIsiModem *idx)
{
	GSList *m = NULL;

	for (m = g_modems; m; m = m->next) {
		struct isi_data *isi = m->data;

		if (isi->idx == idx)
			return isi;
	}
	return NULL;
}

static void mtc_state_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return;
	}

	if (len < 3 || msg[0] != MTC_STATE_INFO_IND)
		return;

	DBG("current modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[1]), msg[1]);
	DBG("target modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[2]), msg[2]);

	ofono_modem_set_powered(isi->modem, msg[1] != MTC_POWER_OFF);
}

static bool mtc_query_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return true;
	}

	if (len < 3 || msg[0] != MTC_STATE_QUERY_RESP)
		return false;

	DBG("current modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[1]), msg[1]);
	DBG("target modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[2]), msg[2]);

	ofono_modem_set_powered(isi->modem, msg[1] != MTC_POWER_OFF);

	return true;
}

static void reachable_cb(GIsiClient *client, bool alive, uint16_t object,
				void *opaque)
{
	const unsigned char msg[] = {
		MTC_STATE_QUERY_REQ,
		0x00, 0x00 /* Filler */
	};

	if (!alive) {
		DBG("Unable to bootstrap mtc driver");
		return;
	}

	DBG("%s (v.%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_isi_subscribe(client, MTC_STATE_INFO_IND, mtc_state_cb, opaque);
	g_isi_request_make(client, msg, sizeof(msg), MTC_TIMEOUT,
				mtc_query_cb, opaque);
}

static void netlink_status_cb(bool up, uint8_t addr, GIsiModem *idx,
				void *data)
{
	struct isi_data *isi = find_modem_by_idx(g_modems, idx);

	DBG("PhoNet is %s, addr=0x%02x, idx=%p",
		up ? "up" : "down", addr, idx);

	if (up) {

		if (isi) {

			DBG("Modem already registered: (0x%02x)",
				g_isi_modem_index(idx));
			return;
		}

		isi = g_new0(struct isi_data, 1);
		if (!isi)
			return;

		isi->idx = idx;
		isi->modem = ofono_modem_create(NULL, "isimodem");
		if (!isi->modem) {
			g_free(isi);
			return;
		}

		g_modems = g_slist_prepend(g_modems, isi);
		ofono_modem_set_data(isi->modem, isi);
		ofono_modem_register(isi->modem);

		DBG("Done regging modem");

 	} else {
		if (!isi) {
			DBG("Unknown modem: (0x%02x)",
				g_isi_modem_index(idx));
			return;
		}

		g_modems = g_slist_remove(g_modems, isi);
		g_isi_client_destroy(isi->client);

		DBG("Now removing modem");
			ofono_modem_remove(isi->modem);
		g_free(isi);
		isi = NULL;
	}
}

static bool mtc_power_on_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return true;
	}

	if (len < 2 || msg[0] != MTC_POWER_ON_RESP)
		return false;

	if (msg[1] == MTC_OK)
		ofono_modem_set_powered(isi->modem, TRUE);

	return true;
}

static bool mtc_power_off_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return true;
	}

	if (len < 2 || msg[0] != MTC_POWER_OFF_RESP)
		return false;

	if (msg[1] == MTC_OK)
		ofono_modem_set_powered(isi->modem, FALSE);

	return true;
}

static int isi_modem_probe(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	isi->client = g_isi_client_create(isi->idx, PN_MTC);
	if (!isi->client)
		return -ENOMEM;

	g_isi_verify(isi->client, reachable_cb, isi);

	return 0;
}

static void isi_modem_remove(struct ofono_modem *modem)
{
	DBG("");
}

static int isi_modem_enable(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	const unsigned char msg[] = {
		MTC_POWER_ON_REQ,
		0x00, 0x00 /* Filler */
	};

	if (!g_isi_request_make(isi->client, msg, sizeof(msg), MTC_TIMEOUT,
				mtc_power_on_cb, isi))
		return -EINVAL;

	return -EINPROGRESS;
}

static int isi_modem_disable(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	const unsigned char msg[] = {
		MTC_POWER_OFF_REQ,
		0x00, 0x00 /* Filler */
	};

	if (!g_isi_request_make(isi->client, msg, sizeof(msg), MTC_TIMEOUT,
				mtc_power_off_cb, isi))
		return -EINVAL;

	return -EINPROGRESS;
}

static void isi_modem_pre_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	ofono_sim_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_devinfo_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_voicecall_create(isi->modem, 0, "isimodem", isi->idx);
}

static void isi_modem_post_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	ofono_phonebook_create(isi->modem, 0, "isimodem", isi->idx);
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
}

static struct ofono_modem_driver driver = {
	.name = "isimodem",
	.probe = isi_modem_probe,
	.remove = isi_modem_remove,
	.enable = isi_modem_enable,
	.disable = isi_modem_disable,
	.pre_sim = isi_modem_pre_sim,
	.post_sim = isi_modem_post_sim,
};

static int isimodem_init(void)
{
	link = g_pn_netlink_start(netlink_status_cb, NULL);

	isi_devinfo_init();
	isi_phonebook_init();
	isi_netreg_init();
	isi_voicecall_init();
	isi_sms_init();
	isi_cbs_init();
	isi_sim_init();
	isi_ssn_init();
	isi_ussd_init();
	isi_call_forwarding_init();
	isi_call_settings_init();
	isi_call_barring_init();
	isi_call_meter_init();
	isi_radio_settings_init();

	ofono_modem_driver_register(&driver);

	return 0;
}

static void isimodem_exit(void)
{
	GSList *m;

	for (m = g_modems; m; m = m->next) {
		struct isi_data *isi = m->data;

		ofono_modem_remove(isi->modem);
		g_free(isi);
	}

	g_slist_free(g_modems);
	g_modems = NULL;

	if (link) {
		g_pn_netlink_stop(link);
		link = NULL;
	}

	ofono_modem_driver_unregister(&driver);

	isi_devinfo_exit();
	isi_phonebook_exit();
	isi_netreg_exit();
	isi_voicecall_exit();
	isi_sms_exit();
	isi_cbs_exit();
	isi_sim_exit();
	isi_ssn_exit();
	isi_ussd_exit();
	isi_call_forwarding_exit();
	isi_call_settings_exit();
	isi_call_barring_exit();
	isi_call_meter_exit();
	isi_radio_settings_exit();
}

OFONO_PLUGIN_DEFINE(isimodem, "PhoNet / ISI modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, isimodem_init, isimodem_exit)
