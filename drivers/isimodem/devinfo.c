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

#include <gisi/message.h>
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

static void info_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_devinfo_query_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;
	uint8_t msgid;
	uint8_t status;

	msgid = g_isi_msg_id(msg);
	if (msgid != INFO_PRODUCT_INFO_READ_RESP &&
			msgid != INFO_VERSION_READ_RESP &&
			msgid != INFO_SERIAL_NUMBER_READ_RESP)
		goto error;

	if (g_isi_msg_error(msg) < 0)
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 0, &status))
		goto error;

	if (status != INFO_OK)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		uint8_t id = g_isi_sb_iter_get_id(&iter);
		uint8_t chars;
		char *info = NULL;

		if (id != INFO_SB_PRODUCT_INFO_MANUFACTURER &&
				id != INFO_SB_PRODUCT_INFO_NAME &&
				id != INFO_SB_MCUSW_VERSION &&
				id != INFO_SB_SN_IMEI_PLAIN)
			continue;

		if (g_isi_sb_iter_get_len(&iter) < 5)
			goto error;

		if (!g_isi_sb_iter_get_byte(&iter, &chars, 3))
			goto error;

		if (!g_isi_sb_iter_get_latin_tag(&iter, &info, chars, 4))
			goto error;

		CALLBACK_WITH_SUCCESS(cb, info, cbd->data);

		g_free(info);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, "", cbd->data);
}

static void isi_query_manufacturer(struct ofono_devinfo *info,
					ofono_devinfo_query_cb_t cb,
					void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const uint8_t msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_MANUFACTURER
	};
	size_t len = sizeof(msg);

	if (cbd == NULL || dev == NULL)
		goto error;

	if (g_isi_client_send(dev->client, msg, len, info_resp_cb, cbd, g_free))
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

	const uint8_t msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_NAME
	};
	size_t len = sizeof(msg);

	if (cbd == NULL || dev == NULL)
		goto error;

	if (g_isi_client_send(dev->client, msg, len, info_resp_cb, cbd, g_free))
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

	const uint8_t msg[] = {
		INFO_VERSION_READ_REQ,
		0x00, INFO_MCUSW,
		0x00, 0x00, 0x00, 0x00
	};
	size_t len = sizeof(msg);

	if (cbd == NULL || dev == NULL)
		goto error;

	if (g_isi_client_send(dev->client, msg, len, info_resp_cb, cbd, g_free))
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

	const uint8_t msg[] = {
		INFO_SERIAL_NUMBER_READ_REQ,
		INFO_SN_IMEI_PLAIN
	};
	size_t len = sizeof(msg);

	if (cbd == NULL || dev == NULL)
		goto error;

	if (g_isi_client_send(dev->client, msg, len, info_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, "", data);
	g_free(cbd);
}

static void reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_devinfo *info = data;

	if (g_isi_msg_error(msg) < 0) {
		ofono_devinfo_remove(info);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	ofono_devinfo_register(info);
}

static int isi_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct devinfo_data *data = g_try_new0(struct devinfo_data, 1);

	if (data == NULL)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_PHONE_INFO);
	if (data->client == NULL) {
		g_free(data);
		return -ENOMEM;
	}


	ofono_devinfo_set_data(info, data);

	g_isi_client_set_timeout(data->client, INFO_TIMEOUT);
	g_isi_client_verify(data->client, reachable_cb, info, NULL);

	return 0;
}

static void isi_devinfo_remove(struct ofono_devinfo *info)
{
	struct devinfo_data *data = ofono_devinfo_get_data(info);

	ofono_devinfo_set_data(info, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
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

void isi_devinfo_init(void)
{
	ofono_devinfo_driver_register(&driver);
}

void isi_devinfo_exit(void)
{
	ofono_devinfo_driver_unregister(&driver);
}
