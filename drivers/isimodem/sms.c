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
#include <ofono/sms.h>

#include "isi.h"

#define PN_SMS			0x02
#define SMS_TIMEOUT		5

struct sms_data {
	GIsiClient *client;
	struct isi_version version;
};

enum message_id {
	SMS_MESSAGE_SEND_REQ = 0x02,
	SMS_MESSAGE_SEND_RESP = 0x03,
	SMS_PP_ROUTING_REQ = 0x06,
	SMS_PP_ROUTING_RESP = 0x07,
	SMS_PP_ROUTING_NTF = 0x08
};

enum sub_block_id {
	SMS_GSM_DELIVER = 0x00,
	SMS_GSM_STATUS_REPORT = 0x01,
	SMS_GSM_SUBMIT = 0x02,
	SMS_GSM_COMMAND = 0x03,
	SMS_GSM_ROUTING = 0x0D
};

enum routing_command {
	SMS_ROUTING_RELEASE = 0x00,
	SMS_ROUTING_SET = 0x01,
	SMS_ROUTING_SUSPEND = 0x02,
	SMS_ROUTING_RESUME = 0x03,
	SMS_ROUTING_UPDATE = 0x04
};

enum routing_mode {
	SMS_GSM_ROUTING_MODE_ALL = 0x0B
};

enum routing_type {
	SMS_GSM_TPDU_ROUTING = 0x06
};

enum message_type {
	SMS_GSM_MT_ALL_TYPE = 0x06
};

enum route_preference {
	SMS_ROUTE_GPRS_PREF = 0x00,
	SMS_ROUTE_CS = 0x01,
	SMS_ROUTE_GPRS = 0x02,
	SMS_ROUTE_CS_PREF = 0x03,
	SMS_ROUTE_DEFAULT = 0x04
};

enum sender_type {
	SMS_SENDER_ANY = 0x00,
	SMS_SENDER_SIM_ATK = 0x01
};

enum content_type {
	SMS_TYPE_DEFAULT = 0x00,
	SMS_TYPE_TEXT_MESSAGE = 0x01
};

enum cause {
	SMS_OK = 0x00,
	SMS_ERR_ROUTING_RELEASED = 0x01,
	SMS_ERR_INVALID_PARAMETER = 0x02
};

static void sms_debug(const void *restrict buf, size_t len, void *data)
{
	DBG("");
	dump_msg(buf, len);
}

static void isi_sca_query(struct ofono_sms *sms, ofono_sms_sca_query_cb_t cb,
				void *data)
{
	DBG("Not implemented.");
	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void isi_sca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *data)
{
	DBG("Not implemented.");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_submit(struct ofono_sms *sms, unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *data)
{
	DBG("Not implemented.");
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void routing_ntf_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	DBG("Not implemented.");
}

static bool routing_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_sms *sms = opaque;

	DBG("");

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SMS_PP_ROUTING_RESP)
		goto error;

	if (msg[1] != SMS_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	ofono_sms_register(sms);
	return true;

error:
	DBG("Unable to bootstrap SMS routing.");
	return true;
}

static int isi_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct sms_data *data = g_try_new0(struct sms_data, 1);

	const unsigned char msg[] = {
		SMS_PP_ROUTING_REQ,
		SMS_ROUTING_SET,
		0x01,  /* Sub-block count */
		SMS_GSM_ROUTING,
		0x08,  /* Sub-block length */
		SMS_GSM_TPDU_ROUTING,
		SMS_GSM_MT_ALL_TYPE,
		0x00, 0x00, 0x00,  /* Filler */
		0x00  /* Sub-sub-block count */
	};

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SMS);
	if (!data->client)
		return -ENOMEM;

	ofono_sms_set_data(sms, data);

	g_isi_client_set_debug(data->client, sms_debug, NULL);
	g_isi_subscribe(data->client, SMS_PP_ROUTING_NTF, routing_ntf_cb, sms);

	if (!g_isi_request_make(data->client, msg, sizeof(msg), SMS_TIMEOUT,
				routing_resp_cb, sms))
		DBG("Failed to set SMS routing.");

	return 0;
}

static void isi_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_sms_driver driver = {
	.name			= "isimodem",
	.probe			= isi_sms_probe,
	.remove			= isi_sms_remove,
	.sca_query		= isi_sca_query,
	.sca_set		= isi_sca_set,
	.submit			= isi_submit
};

void isi_sms_init()
{
	ofono_sms_driver_register(&driver);
}

void isi_sms_exit()
{
	ofono_sms_driver_unregister(&driver);
}
