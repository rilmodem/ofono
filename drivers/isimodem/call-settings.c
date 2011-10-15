/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2011  Nokia Corporation and/or its subsidiary(-ies).
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
#include <ofono/call-settings.h>

#include "isimodem.h"
#include "isiutil.h"
#include "ss.h"
#include "debug.h"

struct settings_data {
	GIsiClient *client;
};

static void update_status_mask(uint32_t *mask, uint8_t bsc)
{
	switch (bsc) {
	case SS_GSM_TELEPHONY:
		*mask |= 1;
		break;

	case SS_GSM_ALL_DATA_TELE:
		*mask |= 1 << 1;
		break;

	case SS_GSM_FACSIMILE:
		*mask |= 1 << 2;
		break;

	case SS_GSM_SMS:
		*mask |= 1 << 3;
		break;

	case SS_GSM_ALL_DATA_CIRCUIT_SYNC:
		*mask |= 1 << 4;
		break;

	case SS_GSM_ALL_DATA_CIRCUIT_ASYNC:
		*mask |= 1 << 5;
		break;

	case SS_GSM_ALL_DATA_PACKET_SYNC:
		*mask |= 1 << 6;
		break;

	case SS_GSM_ALL_PAD_ACCESS:
		*mask |= 1 << 7;
		break;

	default:
		DBG("Unknown BSC value %d, please report", bsc);
		break;
	}
}

static gboolean check_resp(const GIsiMessage *msg, uint8_t msgid, uint8_t type)
{
	uint8_t service;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			ss_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &service) || service != type) {
		DBG("Unexpected service type: 0x%02X", service);
		return FALSE;
	}

	return TRUE;
}

static gboolean decode_gsm_bsc_info(GIsiSubBlockIter *iter, uint32_t *mask)
{
	uint8_t *bsc;
	uint8_t num, i;

	if (!g_isi_sb_iter_get_byte(iter, &num, 2))
		return FALSE;

	if (!g_isi_sb_iter_get_struct(iter, (void **) &bsc, num, 3))
		return FALSE;

	for (i = 0; i < num; i++)
		update_status_mask(mask, bsc[i]);

	return TRUE;
}

static void status_query_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;

	uint32_t mask = 0;
	uint8_t status;

	if (!check_resp(msg, SS_SERVICE_COMPLETED_RESP, SS_INTERROGATION))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 6);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case SS_STATUS_RESULT:

			if (!g_isi_sb_iter_get_byte(&iter, &status, 2))
				goto error;

			if (status & SS_GSM_PROVISIONED)
				mask = 1;

			break;

		case SS_GSM_ADDITIONAL_INFO:
			break;

		case SS_GSM_BSC_INFO:

			if (!decode_gsm_bsc_info(&iter, &mask))
				goto error;

			break;
		}
	}

	CALLBACK_WITH_SUCCESS(cb, mask, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
}

static void isi_clip_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct isi_cb_data *cbd = isi_cb_data_new(cs, cb, data);
	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_ALL_TELE_AND_BEARER,
		SS_GSM_CLIP >> 8, SS_GSM_CLIP & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		0,
	};

	if (sd == NULL || cbd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, sizeof(msg), status_query_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static void isi_colp_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct isi_cb_data *cbd = isi_cb_data_new(cs, cb, data);
	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_ALL_TELE_AND_BEARER,
		SS_GSM_COLP >> 8, SS_GSM_COLP & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		0,
	};

	if (sd == NULL || cbd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, sizeof(msg), status_query_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static void isi_colr_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct isi_cb_data *cbd = isi_cb_data_new(cs, cb, data);
	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_ALL_TELE_AND_BEARER,
		SS_GSM_COLR >> 8, SS_GSM_COLR & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		0,
	};

	if (sd == NULL || cbd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, sizeof(msg), status_query_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static void isi_cw_query(struct ofono_call_settings *cs, int cls,
			ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct isi_cb_data *cbd = isi_cb_data_new(cs, cb, data);
	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_ALL_TELE_AND_BEARER,
		SS_GSM_CALL_WAITING >> 8, SS_GSM_CALL_WAITING & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		0,
	};

	if (sd == NULL || cbd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, sizeof(msg), status_query_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static gboolean check_cw_resp(const GIsiMessage *msg, uint8_t type)
{
	GIsiSubBlockIter iter;
	uint8_t status;

	if (!check_resp(msg, SS_SERVICE_COMPLETED_RESP, type))
		return FALSE;

	for (g_isi_sb_iter_init(&iter, msg, 6);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SS_GSM_DATA)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &status, 2))
			return FALSE;

		if (status & SS_GSM_ACTIVE)
			return type == SS_ACTIVATION;
		else
			return type == SS_DEACTIVATION;
	}

	return FALSE;
}

static void cw_set_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_settings_set_cb_t cb = cbd->cb;

	if (check_cw_resp(msg, SS_ACTIVATION))
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void cw_unset_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_settings_set_cb_t cb = cbd->cb;

	if (check_cw_resp(msg, SS_DEACTIVATION))
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void isi_cw_set(struct ofono_call_settings *cs, int mode, int cls,
			ofono_call_settings_set_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct isi_cb_data *cbd = isi_cb_data_new(cs, cb, data);
	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		mode ? SS_ACTIVATION : SS_DEACTIVATION,
		SS_ALL_TELE_AND_BEARER,
		SS_GSM_CALL_WAITING >> 8, SS_GSM_CALL_WAITING & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		0, /* Subblock count */
	};

	if (cbd == NULL || sd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, sizeof(msg),
				mode ? cw_set_resp_cb : cw_unset_resp_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_call_settings *cs = data;

	if (g_isi_msg_error(msg) < 0) {
		ofono_call_settings_remove(cs);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	ofono_call_settings_register(cs);
}

static int isi_call_settings_probe(struct ofono_call_settings *cs,
					unsigned int vendor, void *user)
{
	GIsiModem *modem = user;
	struct settings_data *sd;

	sd = g_try_new0(struct settings_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->client = g_isi_client_create(modem, PN_SS);

	if (sd->client == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	ofono_call_settings_set_data(cs, sd);

	g_isi_client_verify(sd->client, reachable_cb, cs, NULL);

	return 0;
}

static void isi_call_settings_remove(struct ofono_call_settings *cs)
{
	struct settings_data *data = ofono_call_settings_get_data(cs);

	ofono_call_settings_set_data(cs, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_call_settings_driver driver = {
	.name			= "isimodem",
	.probe			= isi_call_settings_probe,
	.remove			= isi_call_settings_remove,
	.clip_query		= isi_clip_query,
	.colp_query		= isi_colp_query,
	.colr_query		= isi_colr_query,
	.clir_query		= NULL,
	.clir_set		= NULL,
	.cw_query		= isi_cw_query,
	.cw_set			= isi_cw_set
};

void isi_call_settings_init(void)
{
	ofono_call_settings_driver_register(&driver);
}

void isi_call_settings_exit(void)
{
	ofono_call_settings_driver_unregister(&driver);
}
