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
#include <inttypes.h>

#include <glib.h>

#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/cbs.h>

#include "isimodem.h"
#include "isiutil.h"
#include "sms.h"
#include "debug.h"

struct cbs_data {
	GIsiClient *client;
};

static void isi_set_topics(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *data)
{
	DBG("Not implemented (topics=%s), all topics accepted", topics);
	CALLBACK_WITH_SUCCESS(cb, data);
}

static void isi_clear_topics(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_SUCCESS(cb, data);
}

static void routing_ntf_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_cbs *cbs = opaque;

	if (!msg || len < 3 || msg[0] != SMS_GSM_CB_ROUTING_NTF)
		return;

	/* Skipping header(s) */
	msg += 5;
	len -= 5;

	/*
	 * The next 88 bytes of the sub-block are the actual CBS PDU,
	 * followed by an informational data length field, and filler.
	 */
	ofono_cbs_notify(cbs, msg, len - 2);
}

static gboolean routing_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_cbs *cbs = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return TRUE;
	}

	if (len < 3 || msg[0] != SMS_GSM_CB_ROUTING_RESP)
		return FALSE;

	if (msg[1] != SMS_OK) {
		if (msg[1] == SMS_ERR_PP_RESERVED)
			DBG("Request failed: 0x%02"PRIx8" (%s).\n\n  "
				"Unable to bootstrap CBS routing.\n  "
				"It appears some other component is "
				"already\n  registered as the CBS "
				"routing endpoint.\n  As a consequence, "
				"receiving CBSs is NOT going to work.\n\n",
				msg[1], sms_isi_cause_name(msg[1]));
		return TRUE;
	}

	g_isi_subscribe(client, SMS_GSM_CB_ROUTING_NTF, routing_ntf_cb,
			cbs);

	ofono_cbs_register(cbs);
	return TRUE;
}

static int isi_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct cbs_data *cd = g_try_new0(struct cbs_data, 1);
	const char *debug = NULL;

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

	debug = getenv("OFONO_ISI_DEBUG");
	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "cbs") == 0))
		g_isi_client_set_debug(cd->client, sms_debug, NULL);

	if (!g_isi_request_make(cd->client, msg, sizeof(msg), CBS_TIMEOUT,
				routing_resp_cb, cbs))
		DBG("Failed to set CBS routing.");

	return 0;
}

static void isi_cbs_remove(struct ofono_cbs *cbs)
{
	struct cbs_data *data = ofono_cbs_get_data(cbs);

	uint8_t msg[] = {
		SMS_GSM_CB_ROUTING_REQ,
		SMS_ROUTING_RELEASE,
		SMS_GSM_ROUTING_MODE_ALL,
		SMS_CB_NOT_ALLOWED_IDS_LIST,
		0x00,  /* Subject count */
		0x00,  /* Language count */
		0x00,  /* CB range */
		0x00,  /* Subject list MSBS */
		0x00,  /* Subject list LSBS */
		0x00   /* Languages */
	};

	if (!data)
		return;

	if (data->client) {
		/* Send a promiscuous routing release, so as not to
		 * hog resources unnecessarily after being removed */
		g_isi_request_make(data->client, msg, sizeof(msg),
					CBS_TIMEOUT, NULL, NULL);
		g_isi_client_destroy(data->client);
	}

	g_free(data);
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
