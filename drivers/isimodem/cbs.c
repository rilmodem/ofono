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
#include <ofono/cbs.h>

#include "isi.h"

#define PN_SMS			0x02
#define CBS_TIMEOUT		5

enum message_id {
	SMS_GSM_CB_ROUTING_REQ = 0x0B,
	SMS_GSM_CB_ROUTING_RESP = 0x0C,
	SMS_GSM_CB_ROUTING_NTF = 0x0D
};

enum routing_command {
	SMS_ROUTING_RELEASE = 0x00,
	SMS_ROUTING_SET = 0x01,
	SMS_ROUTING_SUSPEND = 0x02,
	SMS_ROUTING_RESUME = 0x03,
	SMS_ROUTING_UPDATE = 0x04
};

enum routing_mode {
	SMS_GSM_ROUTING_MODE_ALL = 0x0B,
	SMS_GSM_ROUTING_MODE_CB_DDL = 0x0C
};

enum cause {
	SMS_OK = 0x00,
	SMS_ERR_ROUTING_RELEASED = 0x01,
	SMS_ERR_INVALID_PARAMETER = 0x02,
	SMS_ERR_DEVICE_FAILURE = 0x03,
	SMS_ERR_PP_RESERVED = 0x04
};

enum subject_list_type {
	SMS_CB_ALLOWED_IDS_LIST = 0x00,
	SMS_CB_NOT_ALLOWED_IDS_LIST = 0x01
};

struct cbs_data {
	GIsiClient *client;
	struct isi_version version;
};

static void cbs_debug(const void *restrict buf, size_t len, void *data)
{
	DBG("");
	dump_msg(buf, len);
}

static void isi_set_topics(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *data)
{
	DBG("Not implemented (topics=%s)", topics);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_clear_topics(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void routing_ntf_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_cbs *cbs = opaque;

	DBG("");

	if (!msg || len < 3 || msg[0] != SMS_GSM_CB_ROUTING_NTF)
		return;

	ofono_cbs_notify(cbs, msg+3, len-3);
}

static bool routing_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_cbs *cbs = opaque;

	DBG("");

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SMS_GSM_CB_ROUTING_RESP)
		goto error;

	if (msg[1] != SMS_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	ofono_cbs_register(cbs);
	return true;

error:
	DBG("Unable to bootstrap CB routing.");
	return true;
}

static int isi_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct cbs_data *cd = g_try_new0(struct cbs_data, 1);

	unsigned char msg[] = {
		SMS_GSM_CB_ROUTING_REQ,
		SMS_ROUTING_SET,
		SMS_GSM_ROUTING_MODE_ALL,
		SMS_CB_NOT_ALLOWED_IDS_LIST,
		0x00,  /* Subject count */
		0x00,  /* Language count */
		0x00,  /* CB range */
		0x00,  /* Subject list MSBS */
		0x00,  /* Subject list LSBS */
		0x00   /* Languages */
	};

	if (!cd)
		return -ENOMEM;

	cd->client = g_isi_client_create(idx, PN_SMS);
	if (!cd->client)
		return -ENOMEM;

	ofono_cbs_set_data(cbs, cd);

	g_isi_client_set_debug(cd->client, cbs_debug, NULL);
	g_isi_subscribe(cd->client, SMS_GSM_CB_ROUTING_NTF, routing_ntf_cb, cbs);

	if (!g_isi_request_make(cd->client, msg, sizeof(msg), CBS_TIMEOUT,
				routing_resp_cb, cbs))
		DBG("Failed to set CBS routing.");

	return 0;
}

static void isi_cbs_remove(struct ofono_cbs *cbs)
{
	struct cbs_data *data = ofono_cbs_get_data(cbs);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_cbs_driver driver = {
	.name			= "isimodem",
	.probe			= isi_cbs_probe,
	.remove			= isi_cbs_remove,
	.set_topics		= isi_set_topics,
	.clear_topics		= isi_clear_topics
};

void isi_cbs_init()
{
	ofono_cbs_driver_register(&driver);
}

void isi_cbs_exit()
{
	ofono_cbs_driver_unregister(&driver);
}
