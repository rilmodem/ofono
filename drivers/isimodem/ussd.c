/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#include <sys/uio.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>
#include <gisi/message.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ussd.h>

#include "smsutil.h"
#include "util.h"

#include "isimodem.h"
#include "isiutil.h"
#include "ss.h"
#include "debug.h"

struct ussd_info {
	uint8_t dcs;
	uint8_t type;
	uint8_t len;
};

struct ussd_data {
	GIsiClient *client;
	GIsiVersion version;
	int mt_session;
};

static gboolean check_response_status(const GIsiMessage *msg, uint8_t msgid)
{
	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			ss_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}
	return TRUE;
}

static void ussd_notify_ack(struct ussd_data *ud)
{
	const uint8_t msg[] = {
		SS_GSM_USSD_SEND_REQ,
		SS_GSM_USSD_NOTIFY,
		0,	/* subblock count */
	};

	g_isi_client_send(ud->client, msg, sizeof(msg), NULL, NULL, NULL);
}

static void ussd_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_ussd *ussd = data;
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	struct ussd_info *info;
	size_t len = sizeof(struct ussd_info);
	uint8_t *string;
	int status;

	if (g_isi_msg_id(msg) != SS_GSM_USSD_RECEIVE_IND)
		return;

	if (!g_isi_msg_data_get_struct(msg, 0, (const void **) &info, len))
		return;

	if (!g_isi_msg_data_get_struct(msg, len, (const void **) &string,
					info->len))
		return;

	switch (info->type) {
	case 0:
		/* Nothing - this is response to NOTIFY_ACK REQ */
		return;

	case SS_GSM_USSD_MT_REPLY:
		/* This never happens, but.. */
		status = OFONO_USSD_STATUS_LOCAL_CLIENT_RESPONDED;
		break;

	case SS_GSM_USSD_COMMAND:

		/* Ignore, we get SS_GSM_USSD_REQUEST, too */
		if (ud->mt_session)
			return;

		status = OFONO_USSD_STATUS_ACTION_REQUIRED;
		break;

	case SS_GSM_USSD_NOTIFY:
		status = OFONO_USSD_STATUS_NOTIFY;
		ussd_notify_ack(ud);
		break;

	case SS_GSM_USSD_END:
		status = OFONO_USSD_STATUS_TERMINATED;
		ud->mt_session = 0;
		break;

	case SS_GSM_USSD_REQUEST:
		ud->mt_session = 1;
		status = OFONO_USSD_STATUS_ACTION_REQUIRED;
		break;

	default:
		status = OFONO_USSD_STATUS_NOT_SUPPORTED;
	}

	DBG("type: %u %s, dcs: 0x%02x, len: %u",
		info->type, ss_ussd_type_name(info->type), info->dcs,
		info->len);

	ofono_ussd_notify(ussd, status, info->dcs, string, info->len);
}

static void ussd_send_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_ussd_cb_t cb = cbd->cb;

	if (check_response_status(msg, SS_GSM_USSD_SEND_RESP))
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void isi_request(struct ofono_ussd *ussd, int dcs,
			const unsigned char *pdu, int len, ofono_ussd_cb_t cb,
			void *data)
{
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	struct isi_cb_data *cbd = isi_cb_data_new(ussd, cb, data);

	size_t sb_len = ALIGN4(4 + len);
	size_t pad_len = sb_len - (4 + len);

	const uint8_t padding[4] = { 0 };
	const uint8_t msg[] = {
		SS_GSM_USSD_SEND_REQ,
		ud->mt_session ? SS_GSM_USSD_MT_REPLY : SS_GSM_USSD_COMMAND,
		1,		/* subblock count */
		SS_GSM_USSD_STRING,
		sb_len,
		dcs,		/* DCS */
		len,		/* string length */
		/* USSD string goes here */
	};
	struct iovec iov[3] = {
		{ (uint8_t *) msg, sizeof(msg) },
		{ (uint8_t *) pdu, len },
		{ (uint8_t *) padding, pad_len },
	};

	if (cbd == NULL || ud == NULL)
		goto error;

	if (g_isi_client_vsend(ud->client, iov, 3, ussd_send_resp_cb, cbd,
				g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void isi_cancel(struct ofono_ussd *ussd, ofono_ussd_cb_t cb, void *data)
{
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	struct isi_cb_data *cbd = isi_cb_data_new(ussd, cb, data);
	const uint8_t msg[] = {
		SS_GSM_USSD_SEND_REQ,
		SS_GSM_USSD_END,
		0, /* subblock count */
	};

	if (cbd == NULL || ud == NULL)
		goto error;

	if (g_isi_client_send(ud->client, msg, sizeof(msg), ussd_send_resp_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void ussd_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_ussd *ussd = data;
	struct ussd_data *ud = ofono_ussd_get_data(ussd);

	if (g_isi_msg_error(msg) < 0) {
		ofono_ussd_remove(ussd);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	g_isi_client_ind_subscribe(ud->client, SS_GSM_USSD_RECEIVE_IND,
					ussd_ind_cb, ussd);

	ofono_ussd_register(ussd);
}

static int isi_ussd_probe(struct ofono_ussd *ussd, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct ussd_data *ud;

	ud = g_try_new0(struct ussd_data, 1);

	if (ud == NULL)
		return -ENOMEM;

	ud->client = g_isi_client_create(modem, PN_SS);
	if (ud->client == NULL) {
		g_free(ud);
		return -ENOMEM;
	}

	ofono_ussd_set_data(ussd, ud);

	g_isi_client_verify(ud->client, ussd_reachable_cb, ussd, NULL);

	return 0;
}

static void isi_ussd_remove(struct ofono_ussd *ussd)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);

	ofono_ussd_set_data(ussd, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_ussd_driver driver = {
	.name			= "isimodem",
	.probe			= isi_ussd_probe,
	.remove			= isi_ussd_remove,
	.request		= isi_request,
	.cancel			= isi_cancel
};

void isi_ussd_init(void)
{
	ofono_ussd_driver_register(&driver);
}

void isi_ussd_exit(void)
{
	ofono_ussd_driver_unregister(&driver);
}
