/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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

#include <glib.h>

#include <gisi/client.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "isimodem.h"
#include "isiutil.h"
#include "debug.h"
#include "gpds.h"
#include "gss.h"
#include "network.h"

struct radio_data {
	GIsiClient *gss_client;
	GIsiClient *gpds_client;
	GIsiClient *wran_client;
	uint16_t wran_object;
	uint16_t quick_release:1;
};

static enum ofono_radio_access_mode isi_mode_to_ofono_mode(guint8 mode)
{
	switch (mode) {
	case GSS_DUAL_RAT:
		return OFONO_RADIO_ACCESS_MODE_ANY;
	case GSS_GSM_RAT:
		return OFONO_RADIO_ACCESS_MODE_GSM;
	case GSS_UMTS_RAT:
		return OFONO_RADIO_ACCESS_MODE_UMTS;
	default:
		return -1;
	}
}

static int ofono_mode_to_isi_mode(enum ofono_radio_access_mode mode)
{
	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		return GSS_DUAL_RAT;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		return GSS_GSM_RAT;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		return GSS_UMTS_RAT;
	default:
		return -1;
	}
}

static void rat_mode_read_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	int mode = -1;
	GIsiSubBlockIter iter;

	if (g_isi_msg_error(msg) < 0) {
		DBG("message error");
		goto error;
	}

	if (g_isi_msg_id(msg) == GSS_CS_SERVICE_FAIL_RESP)
		goto error;

	if (g_isi_msg_id(msg) != GSS_CS_SERVICE_RESP)
		return;

	for (g_isi_sb_iter_init(&iter, msg, 2);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case GSS_RAT_INFO: {
			guint8 info;

			if (!g_isi_sb_iter_get_byte(&iter, &info, 2))
				goto error;

			mode = isi_mode_to_ofono_mode(info);

			break;
		}
		default:
			DBG("Skipping sub-block: %s (%zu bytes)",
				gss_subblock_name(
					g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	CALLBACK_WITH_SUCCESS(cb, mode, cbd->data);
	g_free(cbd);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	g_free(cbd);
	return;
}

static void isi_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct isi_cb_data *cbd = isi_cb_data_new(rd, cb, data);

	const unsigned char msg[] = {
		GSS_CS_SERVICE_REQ,
		GSS_SELECTED_RAT_READ,
		0x00 /* subblock count */
	};

	if (cbd == NULL || rd == NULL)
		goto error;

	if (g_isi_client_send(rd->gss_client, msg, sizeof(msg),
				rat_mode_read_resp_cb, cbd, NULL))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static void mode_write_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;

	if (g_isi_msg_error(msg) < 0) {
		DBG("message error");
		goto error;
	}

	if (g_isi_msg_id(msg) == GSS_CS_SERVICE_FAIL_RESP)
		goto error;

	if (g_isi_msg_id(msg) != GSS_CS_SERVICE_RESP)
		return;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	g_free(cbd);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
	return;
}

static void isi_set_rat_mode(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct isi_cb_data *cbd = isi_cb_data_new(rd, cb, data);
	int isi_mode = ofono_mode_to_isi_mode(mode);

	const unsigned char msg[] = {
		GSS_CS_SERVICE_REQ,
		GSS_SELECTED_RAT_WRITE,
		0x01, /* subblock count */
		GSS_RAT_INFO,
		0x04, /* subblock length */
		isi_mode,
		0x00 /* filler */
	};

	if (cbd == NULL || rd == NULL)
		goto error;

	if (isi_mode == -1)
		goto error;

	if (g_isi_client_send(rd->gss_client, msg, sizeof(msg),
				mode_write_resp_cb, cbd, NULL))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void update_fast_dormancy(struct radio_data *rd)
{
	GIsiModem *modem;

	struct sockaddr_pn dst = {
		.spn_family = AF_PHONET,
		.spn_resource = 0x3a,
		.spn_dev = rd->wran_object >> 8,
		.spn_obj = rd->wran_object & 0xff,
	};

	if (!rd->wran_object)
		return;

	modem = g_isi_client_modem(rd->wran_client);

	if (rd->quick_release) {
		const unsigned char msg[] = {
			0x00, 0x1f, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
			0x00, 0x00, 0x00, 0x00
		};

		g_isi_modem_sendto(modem, &dst, msg, sizeof(msg));
	} else {
		const unsigned char msg[] = {
			0x00, 0x1f, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02,
			0x00, 0x0a, 0x00, 0x00
		};

		g_isi_modem_sendto(modem, &dst, msg, sizeof(msg));
	}

	DBG("3G PS quick release %s",
		rd->quick_release ? "enabled" : "disabled");
}

static void gpds_context_activating_ind_cb(const GIsiMessage *msg, void *opaque)
{
	struct radio_data *rd = opaque;
	update_fast_dormancy(rd);
}

static void isi_query_fast_dormancy(struct ofono_radio_settings *rs,
			ofono_radio_settings_fast_dormancy_query_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	CALLBACK_WITH_SUCCESS(cb, rd->quick_release, data);
}

static void isi_set_fast_dormancy(struct ofono_radio_settings *rs,
				ofono_bool_t enable,
				ofono_radio_settings_fast_dormancy_set_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	rd->quick_release = enable;
	update_fast_dormancy(rd);
	CALLBACK_WITH_SUCCESS(cb, data);
}

static void wran_reachable_cb(const GIsiMessage *msg, void *opaque)
{
	struct radio_data *rd = opaque;

	if (g_isi_msg_error(msg) < 0)
		return;

	ISI_RESOURCE_DBG(msg);

	rd->wran_object = g_isi_msg_object(msg);

	DBG("PN_WRAN object = 0x%04x", rd->wran_object);

	update_fast_dormancy(rd);

	g_isi_client_ind_subscribe(rd->gpds_client,
					GPDS_CONTEXT_ACTIVATING_IND,
					gpds_context_activating_ind_cb, rd);
}

static void gss_reachable_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_radio_settings *rs = opaque;

	if (g_isi_msg_error(msg) < 0) {
		ofono_radio_settings_remove(rs);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	ofono_radio_settings_register(rs);
}

static int isi_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor,
					void *user)
{
	GIsiModem *modem = user;
	struct radio_data *rd = g_try_new0(struct radio_data, 1);

	if (rd == NULL)
		return -ENOMEM;

	rd->gss_client = g_isi_client_create(modem, PN_GSS);
	if (rd->gss_client == NULL)
		goto nomem;

	rd->gpds_client = g_isi_client_create(modem, PN_GPDS);
	if (rd->gpds_client == NULL)
		goto nomem;

	rd->wran_client = g_isi_client_create(modem, PN_WRAN);
	if (rd->wran_client == NULL)
		goto nomem;

	ofono_radio_settings_set_data(rs, rd);

	g_isi_client_verify(rd->gss_client, gss_reachable_cb, rs, NULL);
	g_isi_client_verify(rd->wran_client, wran_reachable_cb, rd, NULL);

	return 0;
nomem:
	g_isi_client_destroy(rd->gss_client);
	g_isi_client_destroy(rd->wran_client);
	g_isi_client_destroy(rd->gpds_client);
	g_free(rd);
	return -ENOMEM;
}

static void isi_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	ofono_radio_settings_set_data(rs, NULL);

	if (rd == NULL)
		return;

	g_isi_client_destroy(rd->gss_client);
	g_isi_client_destroy(rd->wran_client);
	g_isi_client_destroy(rd->gpds_client);
	g_free(rd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= "isimodem",
	.probe			= isi_radio_settings_probe,
	.remove			= isi_radio_settings_remove,
	.query_rat_mode		= isi_query_rat_mode,
	.set_rat_mode		= isi_set_rat_mode,
	.query_fast_dormancy	= isi_query_fast_dormancy,
	.set_fast_dormancy	= isi_set_fast_dormancy,
};

void isi_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void isi_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
