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
#include <ofono/devinfo.h>

#include "isimodem.h"
#include "isiutil.h"
#include "debug.h"
#include "info.h"

struct devinfo_data {
	GIsiClient *client;
};

static gboolean info_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_devinfo_query_cb_t cb = cbd->cb;

	GIsiSubBlockIter iter;
	char *info = NULL;
	guint8 chars;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3) {
		DBG("truncated message");
		return FALSE;
	}

	if (msg[0] != INFO_PRODUCT_INFO_READ_RESP
		&& msg[0] != INFO_VERSION_READ_RESP
		&& msg[0] != INFO_SERIAL_NUMBER_READ_RESP)
		return FALSE;

	if (msg[1] != INFO_OK) {
		DBG("request failed: %s", info_isi_cause_name(msg[1]));
		goto error;
	}

	for (g_isi_sb_iter_init(&iter, msg, len, 3);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case INFO_SB_PRODUCT_INFO_MANUFACTURER:
		case INFO_SB_PRODUCT_INFO_NAME:
		case INFO_SB_MCUSW_VERSION:
		case INFO_SB_SN_IMEI_PLAIN:

			if (g_isi_sb_iter_get_len(&iter) < 5
				|| !g_isi_sb_iter_get_byte(&iter, &chars, 3)
				|| !g_isi_sb_iter_get_latin_tag(&iter,
							&info, chars, 4))
				goto error;

			CALLBACK_WITH_SUCCESS(cb, info, cbd->data);
			g_free(info);

			g_free(cbd);
			return TRUE;

		default:
			DBG("skipping: %s (%zu bytes)",
				info_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

error:
	CALLBACK_WITH_FAILURE(cb, "", cbd->data);
	g_free(cbd);
	return TRUE;
}

static void isi_query_manufacturer(struct ofono_devinfo *info,
					ofono_devinfo_query_cb_t cb,
					void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_MANUFACTURER
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg),
				INFO_TIMEOUT, info_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, "", data);
	g_free(cbd);
}

static void isi_query_model(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_NAME
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg),
				INFO_TIMEOUT, info_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, "", data);
	g_free(cbd);
}

static void isi_query_revision(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_VERSION_READ_REQ,
		0x00, INFO_MCUSW,
		0x00, 0x00, 0x00, 0x00
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg),
				INFO_TIMEOUT, info_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, "", data);
	g_free(cbd);
}

static void isi_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_SERIAL_NUMBER_READ_REQ,
		INFO_SN_IMEI_PLAIN
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg),
				INFO_TIMEOUT, info_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, "", data);
	g_free(cbd);
}

static gboolean isi_devinfo_register(gpointer user)
{
	struct ofono_devinfo *info = user;
	struct devinfo_data *dd = ofono_devinfo_get_data(info);

	const char *debug = getenv("OFONO_ISI_DEBUG");

	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "info") == 0))
		g_isi_client_set_debug(dd->client, info_debug, NULL);

	ofono_devinfo_register(info);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	struct ofono_devinfo *info = opaque;

	if (!alive) {
		DBG("devinfo driver bootstrap failed");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_idle_add(isi_devinfo_register, info);
}

static int isi_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct devinfo_data *data = g_try_new0(struct devinfo_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_PHONE_INFO);
	if (!data->client) {
		g_free(data);
		return -ENOMEM;
	}

	ofono_devinfo_set_data(info, data);

	g_isi_verify(data->client, reachable_cb, info);

	return 0;
}

static void isi_devinfo_remove(struct ofono_devinfo *info)
{
	struct devinfo_data *data = ofono_devinfo_get_data(info);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_devinfo_driver driver = {
	.name			= "isimodem",
	.probe			= isi_devinfo_probe,
	.remove			= isi_devinfo_remove,
	.query_manufacturer	= isi_query_manufacturer,
	.query_model		= isi_query_model,
	.query_revision		= isi_query_revision,
	.query_serial		= isi_query_serial
};

void isi_devinfo_init()
{
	ofono_devinfo_driver_register(&driver);
}

void isi_devinfo_exit()
{
	ofono_devinfo_driver_unregister(&driver);
}
