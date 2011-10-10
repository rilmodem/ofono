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
#include <inttypes.h>

#include <glib.h>

#include <gisi/client.h>
#include <gisi/message.h>
#include <gisi/iter.h>

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

struct cbs_info {
	uint8_t pdu[88];
};

static gboolean check_resp(const GIsiMessage *msg, uint8_t msgid)
{
	uint8_t cause;
	uint8_t reason;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			sms_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &cause))
		return FALSE;

	if (cause == SMS_OK)
		return TRUE;

	if (!g_isi_msg_data_get_byte(msg, 1, &reason))
		return FALSE;

	if (reason == SMS_ERR_PP_RESERVED) {
		DBG("Request failed: 0x%02"PRIx8" (%s).\n\n  Unable to "
			"bootstrap CBS routing.\n  It appears some other "
			"component is already\n  registered as the CBS "
			"routing endpoint.\n  As a consequence, "
			"receiving CBSs is not going to work.\n\n",
			reason, sms_isi_cause_name(reason));
	}
	return FALSE;
}

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

static void routing_ntf_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_cbs *cbs = data;
	struct cbs_info *info;
	size_t len = sizeof(struct cbs_info);
	GIsiSubBlockIter iter;

	if (!check_resp(msg, SMS_GSM_CB_ROUTING_NTF))
		return;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SMS_GSM_CB_MESSAGE)
			continue;

		if (!g_isi_sb_iter_get_struct(&iter, (void *) &info, len, 2))
			return;

		ofono_cbs_notify(cbs, info->pdu, len);
		return;
	}
}

static void routing_resp_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_cbs *cbs = data;
	struct cbs_data *cd = ofono_cbs_get_data(cbs);

	if (!check_resp(msg, SMS_GSM_CB_ROUTING_RESP)) {
		ofono_cbs_remove(cbs);
		return;
	}

	g_isi_client_ntf_subscribe(cd->client, SMS_GSM_CB_ROUTING_NTF,
					routing_ntf_cb, cbs);

	ofono_cbs_register(cbs);
}

static void cbs_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_cbs *cbs = data;
	struct cbs_data *cd = ofono_cbs_get_data(cbs);

	const uint8_t req[] = {
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

	if (g_isi_msg_error(msg) < 0) {
		DBG("Unable to find CBS resource");
		ofono_cbs_remove(cbs);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	g_isi_client_send(cd->client, req, sizeof(req), routing_resp_cb,
				cbs, NULL);
}

static int isi_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct cbs_data *cd = g_try_new0(struct cbs_data, 1);

	if (cd == NULL)
		return -ENOMEM;

	cd->client = g_isi_client_create(modem, PN_SMS);
	if (cd->client == NULL) {
		g_free(cd);
		return -ENOMEM;
	}

	ofono_cbs_set_data(cbs, cd);

	g_isi_client_verify(cd->client, cbs_reachable_cb, cbs, NULL);

	return 0;
}

static void isi_cbs_remove(struct ofono_cbs *cbs)
{
	struct cbs_data *cd = ofono_cbs_get_data(cbs);
	const uint8_t msg[] = {
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

	ofono_cbs_set_data(cbs, NULL);

	if (cd == NULL)
		return;

	/*
	 * Send a promiscuous routing release, so as not to hog
	 * resources unnecessarily after being removed.
	 */
	g_isi_client_send(cd->client, msg, sizeof(msg), NULL, NULL, NULL);

	g_isi_client_destroy(cd->client);
	g_free(cd);
}

static struct ofono_cbs_driver driver = {
	.name			= "isimodem",
	.probe			= isi_cbs_probe,
	.remove			= isi_cbs_remove,
	.set_topics		= isi_set_topics,
	.clear_topics		= isi_clear_topics
};

void isi_cbs_init(void)
{
	ofono_cbs_driver_register(&driver);
}

void isi_cbs_exit(void)
{
	ofono_cbs_driver_unregister(&driver);
}
