/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "isi.h"

#define PN_PHONE_INFO		0x1B
#define INFO_TIMEOUT		5

enum return_code {
	INFO_OK = 0x00,
	INFO_FAIL = 0x01,
	INFO_NO_NUMBER = 0x02,
	INFO_NOT_SUPPORTED = 0x03
};

enum message_id {
	INFO_SERIAL_NUMBER_READ_REQ = 0x00,
	INFO_SERIAL_NUMBER_READ_RESP = 0x01,
	INFO_VERSION_READ_REQ = 0x07,
	INFO_VERSION_READ_RESP = 0x08,
	INFO_PRODUCT_INFO_READ_REQ = 0x15,
	INFO_PRODUCT_INFO_READ_RESP = 0x16
};

enum sub_block_id {
	INFO_SB_PRODUCT_INFO_NAME = 0x01,
	INFO_SB_PRODUCT_INFO_MANUFACTURER = 0x07,
	INFO_SB_SN_IMEI_PLAIN = 0x41,
	INFO_SB_MCUSW_VERSION = 0x48
};

enum product_info_type {
	INFO_PRODUCT_NAME = 0x01,
	INFO_PRODUCT_MANUFACTURER = 0x07
};

enum serial_number_type {
	INFO_SN_IMEI_PLAIN = 0x41
};

enum version_type {
	INFO_MCUSW = 0x01
};

struct devinfo_data {
	GIsiClient *client;
};

static gboolean decode_sb_and_report(const unsigned char *msg, size_t len, int id,
					ofono_devinfo_query_cb_t cb,
					void *data)
{
	if (msg[1] != INFO_OK) {
		DBG("Query failed: 0x%02x", msg[1]);
		return false;
	}

	if (msg[2] == 0 || len < 8 || msg[6] == 0 || len < (size_t)(msg[6] + 7)) {
		DBG("Truncated message");
		return false;
	}

	if (msg[3] == id) {
		char str[msg[6] + 1];

		memcpy(str, msg + 7, msg[6]);
		str[msg[6]] = '\0';
		DBG("<%s>", str);

		{
			DECLARE_SUCCESS(error);
			cb(&error, str, data);
			return true;
		}
	}

	DBG("Unexpected sub-block: 0x%02x", msg[3]);
	return false;
}

static bool manufacturer_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_devinfo_query_cb_t cb = cbd->cb;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (msg[0] != INFO_PRODUCT_INFO_READ_RESP) {
		DBG("Unexpected message ID: 0x%02x", msg[0]);
		goto error;
	}

	if (decode_sb_and_report(msg, len, INFO_SB_PRODUCT_INFO_MANUFACTURER,
					cb, cbd->data))
		goto out;

error:
	{
		DECLARE_FAILURE(error);
		cb(&error, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_manufacturer(struct ofono_devinfo *info,
					ofono_devinfo_query_cb_t cb,
					void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);

	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_MANUFACTURER
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				manufacturer_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, "", data);
	}
}

static bool model_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_devinfo_query_cb_t cb = cbd->cb;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (msg[0] != INFO_PRODUCT_INFO_READ_RESP) {
		DBG("Unexpected message ID: 0x%02x", msg[0]);
		goto error;
	}

	if (decode_sb_and_report(msg, len, INFO_SB_PRODUCT_INFO_NAME,
					cb, cbd->data))
		goto out;

error:
	{
		DECLARE_FAILURE(error);
		cb(&error, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_model(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);

	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_NAME
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				model_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, "", data);
	}
}

static bool revision_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_devinfo_query_cb_t cb = cbd->cb;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (msg[0] != INFO_VERSION_READ_RESP) {
		DBG("Unexpected message ID: 0x%02x", msg[0]);
		goto error;
	}

	if (decode_sb_and_report(msg, len, INFO_SB_MCUSW_VERSION,
					cb, cbd->data))
		goto out;

error:
	{
		DECLARE_FAILURE(error);
		cb(&error, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_revision(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);

	const unsigned char msg[] = {
		INFO_VERSION_READ_REQ,
		0x00, INFO_MCUSW,
		0x00, 0x00, 0x00, 0x00
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				revision_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, "", data);
	}
}

static bool serial_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_devinfo_query_cb_t cb = cbd->cb;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (msg[0] != INFO_SERIAL_NUMBER_READ_RESP) {
		DBG("Unexpected message ID: 0x%02x", msg[0]);
		goto error;
	}

	if (decode_sb_and_report(msg, len, INFO_SB_SN_IMEI_PLAIN,
					cb, cbd->data))
		goto out;

error:
	{
		DECLARE_FAILURE(error);
		cb(&error, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);

	const unsigned char msg[] = {
		INFO_SERIAL_NUMBER_READ_REQ,
		INFO_SN_IMEI_PLAIN
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				serial_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, "", data);
	}
}

static gboolean isi_devinfo_register(gpointer user)
{
	struct ofono_devinfo *info = user;

	ofono_devinfo_register(info);

	return FALSE;
}

static int isi_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct devinfo_data *data = g_try_new0(struct devinfo_data, 1);

	if (!data)
		return -ENOMEM;

	DBG("idx=%p", idx);

	data->client = g_isi_client_create(idx, PN_PHONE_INFO);
	if (!data->client) {
		g_free(data);
		return -ENOMEM;
	}

	ofono_devinfo_set_data(info, data);

	g_idle_add(isi_devinfo_register, info);

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
