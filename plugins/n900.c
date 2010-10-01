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

#include <gisi/modem.h>
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
#include <ofono/audio-settings.h>

#include "drivers/isimodem/isimodem.h"
#include "drivers/isimodem/isiutil.h"
#include "drivers/isimodem/infoserver.h"
#include "drivers/isimodem/mtc.h"
#include "drivers/isimodem/debug.h"

#include "nokia-gpio.h"

struct isi_data {
	struct ofono_modem *modem;
	char const *ifname;
	GIsiModem *idx;
	GIsiClient *client;
	struct isi_infoserver *infoserver;
	ofono_bool_t enabled;
	ofono_bool_t online;
	ofono_bool_t reported;
	enum power_state power_state;
	int mtc_state;
	guint timeout;
	struct isi_cb_data *online_cbd;
};

static void set_power_by_mtc_state(struct isi_data *isi, int state);
static void mtc_power_off(struct isi_data *isi);
static gboolean mtc_power_off_poll(gpointer user);

static void report_powered(struct isi_data *isi, ofono_bool_t powered)
{
	if (powered == isi->reported)
		return;

	DBG("%s", powered ? "Powered on"
		: isi->enabled ? "Reset"
		: "Powered off");

	isi->reported = powered;

	ofono_modem_set_powered(isi->modem, powered);
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
	isi->mtc_state = mtc_state;

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

static void mtc_state_ind_cb(GIsiClient *client, const void *restrict data,
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

	if (msg[2] == MTC_START) {
		DBG("target modem state: %s (0x%02X)",
			mtc_modem_state_name(msg[1]), msg[1]);

		if (msg[1] == MTC_POWER_OFF) {
			isi->power_state = POWER_STATE_OFF_STARTED;
			mtc_power_off_poll(isi);
		}
	} else if (msg[2] == MTC_READY) {
		DBG("current modem state: %s (0x%02X)",
			mtc_modem_state_name(msg[1]), msg[1]);

		set_power_by_mtc_state(isi, msg[1]);
	}
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
		/* Wait until MTC_READY indication */
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

static GIsiRequest *mtc_state(struct isi_data *isi, uint8_t state,
				struct isi_cb_data *cbd)
{
	const unsigned char req[3] = {
		MTC_STATE_REQ, state
	};

	return g_isi_send(isi->client, req, sizeof(req), MTC_TIMEOUT,
				mtc_state_cb, cbd, NULL);
}

static gboolean mtc_startup_synq_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;

	if (!msg) {
		DBG("%s: %s", "MTC_STARTUP_SYNQ",
			strerror(-g_isi_client_error(client)));
		return TRUE;
	}

	if (len < 3 || msg[0] != MTC_STARTUP_SYNQ_RESP)
		return FALSE;

	return TRUE;
}

static void mtc_startup_synq(struct isi_data *isi)
{
	static const unsigned char msg[3] = {
		MTC_STARTUP_SYNQ_REQ,
	};

	g_isi_send(isi->client, msg, sizeof(msg), MTC_TIMEOUT,
			mtc_startup_synq_cb, NULL, NULL);
}

static gboolean mtc_state_query_cb(GIsiClient *client,
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

	DBG("current: %s (0x%02X)", mtc_modem_state_name(msg[1]), msg[1]);
	DBG("target: %s (0x%02X)", mtc_modem_state_name(msg[2]), msg[2]);

	set_power_by_mtc_state(isi, msg[1]);

	mtc_startup_synq(isi);

	return TRUE;
}

static void mtc_state_query(struct isi_data *isi)
{
	static const unsigned char msg[3] = {
		MTC_STATE_QUERY_REQ,
	};

	g_isi_send(isi->client, msg, sizeof(msg), MTC_TIMEOUT,
			mtc_state_query_cb, isi, NULL);
}

static void mtc_reachable_cb(GIsiClient *client, gboolean alive,
				uint16_t object, void *opaque)
{
	struct isi_data *isi = opaque;

	if (!alive) {
		DBG("MTC client: %s", strerror(-g_isi_client_error(client)));
		/* enable is terminated eventually by timeout */
		return;
	}

	DBG("%s (v.%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_isi_subscribe(client, MTC_STATE_INFO_IND, mtc_state_ind_cb, opaque);

	mtc_state_query(isi);
}

static void mtc_shutdown_sync(struct isi_data *isi)
{
	const unsigned char msg[3] = {
		MTC_SHUTDOWN_SYNC_REQ,
	};

	g_isi_send(isi->client, msg, sizeof(msg), MTC_TIMEOUT,
			NULL, NULL, NULL);
}


static gboolean mtc_power_off_poll(gpointer user)
{
	struct isi_data *isi = user;

	isi->timeout = 0;

	if (isi->power_state == POWER_STATE_ON_STARTED
			|| isi->power_state == POWER_STATE_OFF
			|| isi->power_state == POWER_STATE_OFF_WAITING)
		return FALSE;

	mtc_shutdown_sync(isi);

	isi->timeout = g_timeout_add(200, mtc_power_off_poll, user);

	return FALSE;
}

static gboolean mtc_power_off_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	struct isi_data *isi = opaque;
	const unsigned char *msg = data;

	if (!msg) {
		DBG("%s: %s", "MTC_POWER_OFF_RESP",
			strerror(-g_isi_client_error(client)));

		if (isi->power_state == POWER_STATE_OFF_STARTED)
			mtc_power_off(isi);

		return TRUE;
	}

	if (len < 3 || msg[0] != MTC_POWER_OFF_RESP)
		return FALSE;

	/* power off poll is started by mtc_state_ind_cb() */

	return TRUE;
}

static void mtc_power_off(struct isi_data *isi)
{
	static const unsigned char msg[3] = {
		MTC_POWER_OFF_REQ,
	};

	g_isi_send(isi->client, msg, sizeof(msg), MTC_TIMEOUT,
			mtc_power_off_cb, isi, NULL);
}

static void n900_power_cb(enum power_state state, void *data)
{
	struct ofono_modem *modem = data;
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("power state %s", gpio_power_state_name(state));

	isi->power_state = state;

	if (state == POWER_STATE_OFF_STARTED)
		mtc_power_off(isi);
	else if (isi->timeout)
		g_source_remove(isi->timeout);

	if (state == POWER_STATE_ON)
		g_isi_verify(isi->client, mtc_reachable_cb, isi);
	else if (isi->enabled)
		/* If enabled, report modem crash */
		set_power_by_mtc_state(isi, MTC_STATE_NONE);
	else if (state == POWER_STATE_OFF || state == POWER_STATE_ON_FAILED)
		/* If being disabled, report powered off only when safe */
		report_powered(isi, 0);
	else
		isi->mtc_state = MTC_STATE_NONE;
}

static int n900_probe(struct ofono_modem *modem)
{
	char const *ifname = ofono_modem_get_string(modem, "Interface");
	unsigned address = ofono_modem_get_integer(modem, "Address");
	GIsiModem *idx;
	struct isi_data *isi;

	if (ifname == NULL)
		return -EINVAL;

	DBG("(%p) with %s", modem, ifname);

	idx = g_isi_modem_by_name(ifname);
	if (!idx) {
		DBG("Interface=%s: %s", ifname, strerror(errno));
		return -errno;
	}

	if (gpio_probe(idx, address, n900_power_cb, modem) != 0) {
		DBG("gpio for %s: %s", ifname, strerror(errno));
		return -errno;
	}

	isi = g_new0(struct isi_data, 1);
	if (!isi) {
		gpio_remove(modem);
		return -ENOMEM;
	}

	ofono_modem_set_data(isi->modem = modem, isi);

	isi->idx = idx;
	isi->ifname = ifname;
	isi->client = g_isi_client_create(isi->idx, PN_MTC);

	return 0;
}

static void n900_remove(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("");

	if (isi == NULL)
		return;

	gpio_remove(modem);

	if (isi->timeout)
		g_source_remove(isi->timeout);

	g_isi_client_destroy(isi->client);

	g_free(isi);
}

static void n900_set_online(struct ofono_modem *modem,
					ofono_bool_t online,
					ofono_modem_online_cb_t cb, void *data)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	struct isi_cb_data *cbd = isi_cb_data_new(modem, cb, data);

	DBG("(%p) with %s", modem, isi->ifname);

	if (!cbd)
		goto error;

	if (isi->power_state != POWER_STATE_ON)
		goto error;

	if (isi->mtc_state == MTC_SELFTEST_FAIL)
		goto error;

	if (mtc_state(isi, online ? MTC_NORMAL : MTC_RF_INACTIVE, cbd)) {
		isi->online = online;
		return;
	}

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void n900_pre_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("");

	isi->infoserver = isi_infoserver_create(isi->modem, isi->idx);

	ofono_sim_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_devinfo_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_voicecall_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_audio_settings_create(isi->modem, 0, "isimodem", isi->idx);
}

static void n900_post_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("");

	ofono_phonebook_create(isi->modem, 0, "isimodem", isi->idx);
}

static void n900_post_online(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("");

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

static int n900_enable(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("modem=%p with %p", modem, isi ? isi->ifname : NULL);

	isi->enabled = TRUE;

	return gpio_enable(modem);
}

static int n900_disable(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("modem=%p with %p", modem, isi ? isi->ifname : NULL);

	isi->enabled = FALSE;

	return gpio_disable(modem);
}

static struct ofono_modem_driver n900_driver = {
	.name = "n900",
	.probe = n900_probe,
	.remove = n900_remove,
	.enable = n900_enable,
	.disable = n900_disable,
	.set_online = n900_set_online,
	.pre_sim = n900_pre_sim,
	.post_sim = n900_post_sim,
	.post_online = n900_post_online,
};

static int n900_init(void)
{
	return ofono_modem_driver_register(&n900_driver);
}

static void n900_exit(void)
{
	ofono_modem_driver_unregister(&n900_driver);
}

OFONO_PLUGIN_DEFINE(n900, "Nokia N900 modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, n900_init, n900_exit)
