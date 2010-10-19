/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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

static void update_status_mask(unsigned int *mask, int bsc)
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
		DBG("Unknown BSC value %d, please report\n", bsc);
		break;
	}
}

static gboolean query_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	GIsiSubBlockIter iter;
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	guint32 mask = 0;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 7 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		goto error;

	if (msg[1] != SS_INTERROGATION)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_STATUS_RESULT:
			break;

		case SS_GSM_ADDITIONAL_INFO:
			break;

		case SS_GSM_BSC_INFO: {

			guint8 bsc;
			guint8 count;
			guint8 i;

			if (!g_isi_sb_iter_get_byte(&iter, &count, 2))
				goto error;

			for (i = 0; i < count; i++) {
				if (!g_isi_sb_iter_get_byte(&iter, &bsc, 3 + i))
					goto error;
				update_status_mask(&mask, bsc);
			}
			break;
		}
		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				ss_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	DBG("status_mask %d\n", mask);
	CALLBACK_WITH_SUCCESS(cb, mask, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, 0, cbd->data);

out:
	g_free(cbd);
	return TRUE;

}

static void isi_cw_query(struct ofono_call_settings *cs, int cls,
			ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct isi_cb_data *cbd = isi_cb_data_new(cs, cb, data);

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_ALL_TELE_AND_BEARER,
		SS_GSM_CALL_WAITING >> 8,   /* Supplementary services */
		SS_GSM_CALL_WAITING & 0xFF, /* code */
		SS_SEND_ADDITIONAL_INFO,
		0 /* Subblock count */
	};

	DBG("waiting class %d\n", cls);

	if (!cbd || !sd)
		goto error;

	if (g_isi_request_make(sd->client, msg, sizeof(msg), SS_TIMEOUT,
				query_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static gboolean set_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	GIsiSubBlockIter iter;
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_settings_set_cb_t cb = cbd->cb;

	if (len < 7 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		goto error;

	if (msg[1] != SS_ACTIVATION && msg[1] != SS_DEACTIVATION)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_GSM_ADDITIONAL_INFO:
			break;

		case SS_GSM_DATA: {

			guint8 status;

			if (!g_isi_sb_iter_get_byte(&iter, &status, 2))
				goto error;

			if ((status & SS_GSM_ACTIVE)
				&& (msg[1] == SS_DEACTIVATION))
				goto error;

			if (!(status & SS_GSM_ACTIVE)
				&& (msg[1] == SS_ACTIVATION))
				goto error;

			break;
		}
		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				ss_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;

}

static void isi_cw_set(struct ofono_call_settings *cs, int mode, int cls,
			ofono_call_settings_set_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct isi_cb_data *cbd = isi_cb_data_new(cs, cb, data);

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		mode ? SS_ACTIVATION : SS_DEACTIVATION,
		SS_ALL_TELE_AND_BEARER,
		SS_GSM_CALL_WAITING >> 8,   /* Supplementary services */
		SS_GSM_CALL_WAITING & 0xFF, /* code */
		SS_SEND_ADDITIONAL_INFO,
		0  /* Subblock count */
	};

	DBG("waiting mode %d class %d\n", mode, cls);

	if (!cbd || !sd)
		goto error;

	if (g_isi_request_make(sd->client, msg, sizeof(msg), SS_TIMEOUT,
				set_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static gboolean isi_call_settings_register(gpointer user)
{
	struct ofono_call_settings *cs = user;

	ofono_call_settings_register(cs);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	struct ofono_call_settings *cs = opaque;
	const char *debug = NULL;

	if (!alive) {
		DBG("Unable to bootstrap call settings driver");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	debug = getenv("OFONO_ISI_DEBUG");
	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "ss") == 0))
		g_isi_client_set_debug(client, ss_debug, NULL);

	g_idle_add(isi_call_settings_register, cs);
}


static int isi_call_settings_probe(struct ofono_call_settings *cs,
					unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct settings_data *data;

	data = g_try_new0(struct settings_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SS);

	if (!data->client)
		return -ENOMEM;

	ofono_call_settings_set_data(cs, data);

	if (!g_isi_verify(data->client, reachable_cb, cs))
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_call_settings_remove(struct ofono_call_settings *cs)
{
	struct settings_data *data = ofono_call_settings_get_data(cs);

	if (!data)
		return;

	ofono_call_settings_set_data(cs, NULL);
	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_call_settings_driver driver = {
	.name			= "isimodem",
	.probe			= isi_call_settings_probe,
	.remove			= isi_call_settings_remove,
	.clip_query		= NULL,
	.colp_query		= NULL,
	.clir_query		= NULL,
	.colr_query		= NULL,
	.clir_set		= NULL,
	.cw_query		= isi_cw_query,
	.cw_set			= isi_cw_set
};

void isi_call_settings_init()
{
	ofono_call_settings_driver_register(&driver);
}

void isi_call_settings_exit()
{
	ofono_call_settings_driver_unregister(&driver);
}
