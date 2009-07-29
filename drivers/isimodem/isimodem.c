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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <gisi/netlink.h>
#include <gisi/client.h>

#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include "driver.h"

#include "isi.h"

#define PN_PHONE_INFO		0x1B
#define INFO_TIMEOUT		5

enum return_codes {
	INFO_OK = 0x00,
	INFO_FAIL = 0x01,
	INFO_NO_NUMBER = 0x02,
	INFO_NOT_SUPPORTED = 0x03
};

enum message_ids {
	INFO_SERIAL_NUMBER_READ_REQ = 0x00,
	INFO_SERIAL_NUMBER_READ_RESP = 0x01,
	INFO_VERSION_READ_REQ = 0x07,
	INFO_VERSION_READ_RESP = 0x08,
	INFO_PRODUCT_INFO_READ_REQ = 0x15,
	INFO_PRODUCT_INFO_READ_RESP = 0x16
};

enum sub_block_ids {
	INFO_SB_PRODUCT_INFO_NAME = 0x01,
	INFO_SB_PRODUCT_INFO_MANUFACTURER = 0x07,
	INFO_SB_SN_IMEI_PLAIN = 0x41,
	INFO_SB_MCUSW_VERSION = 0x48
};

enum product_info_types {
	INFO_PRODUCT_NAME = 0x01,
	INFO_PRODUCT_MANUFACTURER = 0x07
};

enum serial_number_types {
	INFO_SN_IMEI_PLAIN = 0x41
};

enum version_types {
	INFO_MCUSW = 0x01
};

static GPhonetNetlink *pn_link = NULL;
static struct isi_data *isi = NULL;
static GIsiClient *client = NULL;
static GSList *pending = NULL;

void dump_msg(const unsigned char *msg, size_t len)
{
	char dumpstr[len * 5 + len / 10 + 1];
	size_t i;

	for (i = 0; i < len; i++)
		sprintf(dumpstr + i * 5, "0x%02x%s",
			msg[i], (i + 1) % 10 == 0 ? "\n" : " ");

	DBG("%zd bytes:\n%s", len, dumpstr);
}

static void clear_pending_reqs()
{
	GSList *l;

	for (l = pending; l; l = l->next)
		g_isi_request_cancel((GIsiRequest *)l->data);
}

static gboolean decode_sb_and_report(const unsigned char *msg, size_t len, int id,
					ofono_modem_attribute_query_cb_t cb,
					void *data)
{
	struct ofono_error err;

	dump_msg(msg, len);

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

		err.type = OFONO_ERROR_TYPE_NO_ERROR;
		err.error = 0;

		cb(&err, str, data);
		return true;
	}

	DBG("Unexpected sub-block: 0x%02x", msg[3]);
	return false;
}

static bool manufacturer_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_modem_attribute_query_cb_t cb = cbd->cb;

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
		DECLARE_FAILURE(e);
		cb(&e, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_manufacturer(struct ofono_modem *modem,
					ofono_modem_attribute_query_cb_t cb,
					void *data)
{
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);
	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_MANUFACTURER
	};

	GIsiRequest *req = NULL;

	if (!cbd)
		goto error;

	req = g_isi_request_make(client, msg, sizeof(msg), INFO_TIMEOUT,
					manufacturer_resp_cb, cbd);

	if (req) {
		pending = g_slist_append(pending, req);
		return;
	}

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
	ofono_modem_attribute_query_cb_t cb = cbd->cb;

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
		DECLARE_FAILURE(e);
		cb(&e, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_model(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb,
				void *data)
{
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);
	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_NAME
	};

	GIsiRequest *req = NULL;

	if (!cbd)
		goto error;

	req = g_isi_request_make(client, msg, sizeof(msg), INFO_TIMEOUT,
				model_resp_cb, cbd);

	if (req) {
		pending = g_slist_append(pending, req);
		return;
	}

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
	ofono_modem_attribute_query_cb_t cb = cbd->cb;

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
		DECLARE_FAILURE(e);
		cb(&e, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_revision(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb,
				void *data)
{
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);
	const unsigned char msg[] = {
		INFO_VERSION_READ_REQ,
		0x00, INFO_MCUSW,
		0x00, 0x00, 0x00, 0x00
	};

	GIsiRequest *req = NULL;

	if (!cbd)
		goto error;

	req = g_isi_request_make(client, msg, sizeof(msg), INFO_TIMEOUT,
					revision_resp_cb, cbd);

	if (req) {
		pending = g_slist_append(pending, req);
		return;
	}

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
	ofono_modem_attribute_query_cb_t cb = cbd->cb;

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
		DECLARE_FAILURE(e);
		cb(&e, "", cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_query_serial(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb,
				void *data)
{
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);
	const unsigned char msg[] = {
		INFO_SERIAL_NUMBER_READ_REQ,
		INFO_SN_IMEI_PLAIN
	};

	GIsiRequest *req = NULL;

	if (!cbd)
		goto error;

	req = g_isi_request_make(client, msg, sizeof(msg), INFO_TIMEOUT,
					serial_resp_cb, cbd);

	if (req) {
		pending = g_slist_append(pending, req);
		return;
	}

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, "", data);
	}
}

static struct ofono_modem_attribute_ops ops = {
	.query_manufacturer = isi_query_manufacturer,
	.query_model = isi_query_model,
	.query_revision = isi_query_revision,
	.query_serial = isi_query_serial
};

static void netlink_status_cb(bool up, uint8_t addr, unsigned idx,
				void *data)
{
	struct isi_data *isi = data;

	DBG("PhoNet is %s, addr=0x%02x, idx=%d",
		up ? "up" : "down", addr, idx);

	if (up) {
		if (!client) {
			client = g_isi_client_create(PN_PHONE_INFO);
			if (!client)
				return;
		}

		if (!isi->modem) {
			isi->modem = ofono_modem_register(&ops);
			if (!isi->modem)
				return;

			ofono_modem_set_userdata(isi->modem, isi);
		}
	} else {
		clear_pending_reqs();

		if (client) {
			g_isi_client_destroy(client);
			client = NULL;
		}

		if (isi->modem) {
			ofono_modem_unregister(isi->modem);
			isi->modem = NULL;
		}
	}
}

static int isimodem_init(void)
{
	isi = g_new0(struct isi_data, 1);

	pn_link = g_pn_netlink_start(netlink_status_cb, isi);
	
	return 0;
}

static void isimodem_exit(void)
{
	clear_pending_reqs();

	if (client) {
		g_isi_client_destroy(client);
		client = NULL;
	}

	if (pn_link) {
		g_pn_netlink_stop(pn_link);
		pn_link = NULL;
	}

	g_free(isi);
}

OFONO_PLUGIN_DEFINE(isimodem, "PhoNet / ISI modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, isimodem_init, isimodem_exit)
