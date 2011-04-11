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
#include <gisi/message.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-forwarding.h>

#include "isimodem.h"
#include "isiutil.h"
#include "ss.h"
#include "debug.h"

struct forw_data {
	GIsiClient *client;
};

struct forw_info {
	uint8_t bsc;		/* Basic service code */
	uint8_t status;		/* SS status */
	uint8_t ton;		/* Type of number */
	uint8_t noreply;	/* No reply timeout */
	uint8_t forw_opt;	/* Forwarding option */
	uint8_t numlen;		/* Number length */
	uint8_t sublen;		/* Sub-address length */
	uint8_t filler;
};

static int forw_type_to_isi_code(int type)
{
	int ss_code;

	switch (type) {
	case 0:
		ss_code = SS_GSM_FORW_UNCONDITIONAL;
		break;
	case 1:
		ss_code = SS_GSM_FORW_BUSY;
		break;
	case 2:
		ss_code = SS_GSM_FORW_NO_REPLY;
		break;
	case 3:
		ss_code = SS_GSM_FORW_NO_REACH;
		break;
	case 4:
		ss_code = SS_GSM_ALL_FORWARDINGS;
		break;
	case 5:
		ss_code = SS_GSM_ALL_COND_FORWARDINGS;
		break;
	default:
		DBG("Unknown forwarding type %d", type);
		ss_code = -1;
		break;
	}
	return ss_code;
}

static gboolean check_resp(const GIsiMessage *msg, uint8_t msgid, uint8_t type)
{
	uint8_t service;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			ss_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &service) || service != type) {
		DBG("Unexpected service type: 0x%02X", service);
		return FALSE;
	}

	return TRUE;
}

static gboolean decode_gsm_forwarding_info(GIsiSubBlockIter *parent,
						uint8_t *status, uint8_t *ton,
						uint8_t *noreply, char **number)
{
	GIsiSubBlockIter iter;
	struct forw_info *info;
	size_t len = sizeof(struct forw_info);
	char *tag = NULL;

	for (g_isi_sb_subiter_init(parent, &iter, 4);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SS_GSM_FORWARDING_FEATURE)
			continue;

		if (!g_isi_sb_iter_get_struct(&iter, (void *) &info, len, 2))
			return FALSE;

		if (info->numlen != 0) {
			if (!g_isi_sb_iter_get_alpha_tag(&iter, &tag,
							info->numlen * 2,
							2 + len))
				return FALSE;

			if (number)
				*number = tag;
			else
				g_free(tag);
		} else {
			if (number)
				*number = g_strdup("");
		}

		if (status)
			*status = info->status;

		if (ton)
			*ton = info->ton;

		if (noreply)
			*noreply = info->noreply;

		return TRUE;
	}
	return FALSE;
}

static void registration_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_forwarding_set_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;
	uint8_t status;

	if (!check_resp(msg, SS_SERVICE_COMPLETED_RESP, SS_REGISTRATION))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 6);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SS_GSM_FORWARDING_INFO)
			continue;

		if (!decode_gsm_forwarding_info(&iter, &status, NULL, NULL,
						NULL))
			goto error;

		if (status & (SS_GSM_ACTIVE | SS_GSM_REGISTERED)) {
			CALLBACK_WITH_SUCCESS(cb, cbd->data);
			return;
		}
	}

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void isi_registration(struct ofono_call_forwarding *cf, int type,
				int cls,
				const struct ofono_phone_number *number,
				int time, ofono_call_forwarding_set_cb_t cb,
				void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct isi_cb_data *cbd = isi_cb_data_new(cf, cb, data);
	int ss_code = forw_type_to_isi_code(type);

	char *ucs2 = NULL;

	size_t numlen = strlen(number->number);
	size_t sb_len = ALIGN4(6 + 2 * numlen);
	size_t pad_len = sb_len - (6 + 2 * numlen);

	uint8_t msg[7 + 6 + 28 * 2 + 3] = {
		SS_SERVICE_REQ,
		SS_REGISTRATION,
		SS_GSM_TELEPHONY,
		ss_code >> 8, ss_code & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		1,	/* Subblock count */
		SS_FORWARDING,
		sb_len,
		number->type,
		ss_code == SS_GSM_FORW_NO_REPLY ? time : SS_UNDEFINED_TIME,
		numlen,
		0,	/* Sub address length */
		/*
		 * Followed by number in UCS-2 (no NULL termination),
		 * zero sub address bytes, and 0 to 3 bytes of filler
		 */
	};
	size_t msg_len = 7 + 6 + numlen * 2 + pad_len;

	if (cbd == NULL || fd == NULL || numlen > 28)
		goto error;

	DBG("forwarding type %d class %d number %s", type, cls, number->number);

	if (ss_code < 0)
		goto error;

	ucs2 = g_convert(number->number, numlen, "UCS-2BE", "UTF-8//TRANSLIT",
				NULL, NULL, NULL);
	if (ucs2 == NULL)
		goto error;

	memcpy(msg + 13, ucs2, numlen * 2);
	g_free(ucs2);

	if (g_isi_client_send(fd->client, msg, msg_len, registration_resp_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void erasure_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_forwarding_set_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;
	uint8_t status;

	if (!check_resp(msg, SS_SERVICE_COMPLETED_RESP, SS_ERASURE))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 6);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SS_GSM_FORWARDING_INFO)
			continue;

		if (!decode_gsm_forwarding_info(&iter, &status,	NULL, NULL,
						NULL))
			goto error;

		if (status & (SS_GSM_ACTIVE | SS_GSM_REGISTERED))
			goto error;

	}
	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void isi_erasure(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct isi_cb_data *cbd = isi_cb_data_new(cf, cb, data);
	int ss_code = forw_type_to_isi_code(type);

	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		SS_ERASURE,
		SS_GSM_TELEPHONY,
		ss_code >> 8, ss_code & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		0,		/* Subblock count */
	};

	DBG("forwarding type %d class %d", type, cls);

	if (cbd == NULL || fd == NULL || ss_code < 0)
		goto error;

	if (g_isi_client_send(fd->client, msg, sizeof(msg),
				erasure_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void query_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_forwarding_query_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;

	struct ofono_call_forwarding_condition list = {
		.status = 0,
		.cls = 7,
		.time = 0,
		.phone_number = {
			.number[0] = '\0',
			.type = 0,
		},
	};
	uint8_t status;
	uint8_t ton;
	uint8_t noreply;
	char *number = NULL;

	if (!check_resp(msg, SS_SERVICE_COMPLETED_RESP, SS_INTERROGATION))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 6);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		DBG("Got %s", ss_subblock_name(g_isi_sb_iter_get_id(&iter)));

		if (g_isi_sb_iter_get_id(&iter) != SS_GSM_FORWARDING_INFO)
			continue;

		if (!decode_gsm_forwarding_info(&iter, &status, &ton, &noreply,
						&number))
			goto error;

		list.status = status & (SS_GSM_ACTIVE | SS_GSM_REGISTERED |
					SS_GSM_PROVISIONED);
		list.time = noreply;
		list.phone_number.type = ton | 0x80;

		DBG("Number <%s>", number);

		strncpy(list.phone_number.number, number,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
		list.phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
		g_free(number);

		DBG("forwarding query: %d, %d, %s(%d) - %d sec",
			list.status, list.cls, list.phone_number.number,
			list.phone_number.type, list.time);
	}
	CALLBACK_WITH_SUCCESS(cb, 1, &list, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
}


static void isi_query(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_query_cb_t cb,
				void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct isi_cb_data *cbd = isi_cb_data_new(cf, cb, data);
	int ss_code = forw_type_to_isi_code(type);

	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_GSM_TELEPHONY,
		ss_code >> 8, ss_code & 0xFF,
		SS_SEND_ADDITIONAL_INFO,
		0, /* Subblock count */
	};

	DBG("forwarding type %d class %d", type, cls);

	if (cbd == NULL || fd == NULL || cls != 7 || ss_code < 0)
		goto error;

	if (g_isi_client_send(fd->client, msg, sizeof(msg), query_resp_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
	g_free(cbd);
}

static void reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_call_forwarding *cf = data;

	if (g_isi_msg_error(msg) < 0) {
		ofono_call_forwarding_remove(cf);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	ofono_call_forwarding_register(cf);
}


static int isi_call_forwarding_probe(struct ofono_call_forwarding *cf,
					unsigned int vendor, void *user)
{
	GIsiModem *modem = user;
	struct forw_data *fd;

	fd = g_try_new0(struct forw_data, 1);
	if (fd == NULL)
		return -ENOMEM;

	fd->client = g_isi_client_create(modem, PN_SS);
	if (fd->client == NULL) {
		g_free(fd);
		return -ENOMEM;
	}

	ofono_call_forwarding_set_data(cf, fd);

	g_isi_client_verify(fd->client, reachable_cb, cf, NULL);

	return 0;
}

static void isi_call_forwarding_remove(struct ofono_call_forwarding *cf)
{
	struct forw_data *data = ofono_call_forwarding_get_data(cf);

	ofono_call_forwarding_set_data(cf, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_call_forwarding_driver driver = {
	.name			= "isimodem",
	.probe			= isi_call_forwarding_probe,
	.remove			= isi_call_forwarding_remove,
	.activation		= NULL,
	.registration		= isi_registration,
	.deactivation		= NULL,
	.erasure		= isi_erasure,
	.query			= isi_query
};

void isi_call_forwarding_init(void)
{
	ofono_call_forwarding_driver_register(&driver);
}

void isi_call_forwarding_exit(void)
{
	ofono_call_forwarding_driver_unregister(&driver);
}
