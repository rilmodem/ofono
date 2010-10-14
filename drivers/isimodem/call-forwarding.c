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
#include <ofono/call-forwarding.h>

#include "isimodem.h"
#include "isiutil.h"
#include "ss.h"
#include "debug.h"

struct forw_data {
	GIsiClient *client;
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
		DBG("Unknown forwarding type %d\n", type);
		ss_code = -1;
		break;
	}
	return ss_code;
}

static gboolean decode_gsm_forwarding_info(const void *restrict data,
						size_t len,
						uint8_t *status, uint8_t *ton,
						uint8_t *norply, char **number)
{
	GIsiSubBlockIter iter;

	for (g_isi_sb_iter_init(&iter, data, len, 0);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_GSM_FORWARDING_FEATURE: {

			uint8_t _numlen;
			uint8_t _status;
			uint8_t _norply;
			uint8_t _ton;
			char *_number = NULL;

			if (!g_isi_sb_iter_get_byte(&iter, &_status, 3)
				|| !g_isi_sb_iter_get_byte(&iter, &_ton, 4)
				|| !g_isi_sb_iter_get_byte(&iter, &_norply, 5)
				|| !g_isi_sb_iter_get_byte(&iter, &_numlen, 7)
				|| !g_isi_sb_iter_get_alpha_tag(&iter, &_number,
					_numlen * 2, 10))
				return FALSE;

			if (status)
				*status = _status;
			if (ton)
				*ton = _ton;
			if (norply)
				*norply = _norply;
			if (number)
				*number = _number;
			else
				g_free(_number);

			return TRUE;
		}
		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				ss_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}
	return FALSE;
}

static gboolean registration_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	GIsiSubBlockIter iter;
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_forwarding_set_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 7 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		return FALSE;

	if (msg[1] != SS_REGISTRATION)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_GSM_ADDITIONAL_INFO:
			break;

		case SS_GSM_FORWARDING_INFO: {

			guint8 status;
			void *info = NULL;
			size_t infolen;

			if (!g_isi_sb_iter_get_data(&iter, &info, 4))
				goto error;

			infolen = g_isi_sb_iter_get_len(&iter) - 4;

			if (!decode_gsm_forwarding_info(info, infolen, &status,
							NULL, NULL, NULL))
				goto error;

			if (!(status & SS_GSM_ACTIVE)
				|| !(status & SS_GSM_REGISTERED))
				goto error;

			break;
		}
		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				ss_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;
}

static void isi_registration(struct ofono_call_forwarding *cf,
				int type, int cls,
				const struct ofono_phone_number *number,
				int time,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct isi_cb_data *cbd = isi_cb_data_new(cf, cb, data);
	int ss_code;
	int num_filler;
	char *ucs2 = NULL;

	unsigned char msg[100] = {
		SS_SERVICE_REQ,
		SS_REGISTRATION,
		SS_GSM_TELEPHONY,
		0, 0,  /* Supplementary services code */
		SS_SEND_ADDITIONAL_INFO,
		1,  /* Subblock count */
		SS_FORWARDING,
		0,  /* Variable subblock length, because of phone number */
		number->type,
		time,
		strlen(number->number),
		0  /* Sub address length */
	};
	/* Followed by number in UCS-2, zero sub address bytes, and 0
	 * to 3 bytes of filler */

	DBG("forwarding type %d class %d\n", type, cls);

	if (!cbd || !number->number || strlen(number->number) > 28)
		goto error;

	ss_code = forw_type_to_isi_code(type);
	if (ss_code < 0)
		goto error;

	msg[3] = ss_code >> 8;
	msg[4] = ss_code & 0xFF;

	num_filler = (6 + 2 * strlen(number->number)) % 4;
	if (num_filler != 0)
		num_filler = 4 - num_filler;

	msg[8]  = 6 + 2 * strlen(number->number) + num_filler;

	ucs2 = g_convert(number->number, strlen(number->number), "UCS-2BE",
				"UTF-8//TRANSLIT", NULL, NULL, NULL);
	if (ucs2 == NULL)
		goto error;

	memcpy((char *)msg + 13, ucs2, strlen(number->number) * 2);
	g_free(ucs2);

	if (g_isi_request_make(fd->client, msg, 7 + msg[8], SS_TIMEOUT,
				registration_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static gboolean erasure_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	GIsiSubBlockIter iter;
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_forwarding_set_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 7 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		goto error;

	if (msg[1] != SS_ERASURE)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_GSM_ADDITIONAL_INFO:
			break;

		case SS_GSM_FORWARDING_INFO: {

			guint8 status;
			void *info = NULL;
			size_t infolen;

			if (!g_isi_sb_iter_get_data(&iter, &info, 4))
				goto error;

			infolen = g_isi_sb_iter_get_len(&iter) - 4;

			if (!decode_gsm_forwarding_info(info, infolen, &status,
							NULL, NULL, NULL))
				goto error;

			if (status & (SS_GSM_ACTIVE | SS_GSM_REGISTERED))
				goto error;

			break;
		}
		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				ss_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;
}


static void isi_erasure(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct isi_cb_data *cbd = isi_cb_data_new(cf, cb, data);
	int ss_code;

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		SS_ERASURE,
		SS_GSM_TELEPHONY,
		0, 0,  /* Supplementary services code */
		SS_SEND_ADDITIONAL_INFO,
		0  /* Subblock count */
	};

	DBG("forwarding type %d class %d\n", type, cls);

	if (!cbd)
		goto error;

	ss_code = forw_type_to_isi_code(type);
	if (ss_code < 0)
		goto error;

	msg[3] = ss_code >> 8;
	msg[4] = ss_code & 0xFF;

	if (g_isi_request_make(fd->client, msg, sizeof(msg), SS_TIMEOUT,
				erasure_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static gboolean query_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	GIsiSubBlockIter iter;
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_forwarding_query_cb_t cb = cbd->cb;

	struct ofono_call_forwarding_condition list;
	list.status = 0;
	list.cls = 7;
	list.time = 0;
	list.phone_number.number[0] = 0;
	list.phone_number.type = 0;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 7 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		goto error;

	if (msg[1] != SS_INTERROGATION)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_STATUS_RESULT:
			break;

		case SS_GSM_ADDITIONAL_INFO:
			break;

		case SS_GSM_FORWARDING_INFO: {

			guint8 status;
			void *info = NULL;
			size_t infolen;

			guint8 ton;
			guint8 norply;
			char *number = NULL;

			if (!g_isi_sb_iter_get_data(&iter, &info, 4))
				goto error;

			infolen = g_isi_sb_iter_get_len(&iter) - 4;

			if (!decode_gsm_forwarding_info(info, infolen, &status,
							&ton, &norply, &number))
				goto error;

			/* As in 27.007 section 7.11 */
			list.status = status & SS_GSM_ACTIVE;
			list.time = norply;
			list.phone_number.type = ton | 128;
			strncpy(list.phone_number.number, number,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
			list.phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
			g_free(number);

			break;
		}
		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				ss_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	DBG("forwarding query: %d, %d, %s(%d) - %d sec",
			list.status, list.cls,
			list.phone_number.number,
			list.phone_number.type, list.time);
	CALLBACK_WITH_SUCCESS(cb, 1, &list, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);

out:
	g_free(cbd);
	return TRUE;

}


static void isi_query(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_query_cb_t cb,
				void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct isi_cb_data *cbd = isi_cb_data_new(cf, cb, data);
	int ss_code;

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_GSM_TELEPHONY,
		0, 0,  /* Supplementary services code */
		SS_SEND_ADDITIONAL_INFO,
		0  /* Subblock count */
	};

	DBG("forwarding type %d class %d\n", type, cls);

	if (!cbd || cls != 7)
		goto error;

	ss_code = forw_type_to_isi_code(type);
	if (ss_code < 0)
		goto error;

	msg[3] = ss_code >> 8;
	msg[4] = ss_code & 0xFF;

	if (g_isi_request_make(fd->client, msg, sizeof(msg), SS_TIMEOUT,
				query_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
	g_free(cbd);
}

static gboolean isi_call_forwarding_register(gpointer user)
{
	struct ofono_call_forwarding *cf = user;

	ofono_call_forwarding_register(cf);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	struct ofono_call_forwarding *cf = opaque;
	const char *debug = NULL;

	if (!alive) {
		DBG("Unable to bootstrap call forwarding driver");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	debug = getenv("OFONO_ISI_DEBUG");
	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "ss") == 0))
		g_isi_client_set_debug(client, ss_debug, NULL);

	g_idle_add(isi_call_forwarding_register, cf);
}


static int isi_call_forwarding_probe(struct ofono_call_forwarding *cf,
					unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct forw_data *data;

	data = g_try_new0(struct forw_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SS);
	if (!data->client)
		return -ENOMEM;

	ofono_call_forwarding_set_data(cf, data);

	if (!g_isi_verify(data->client, reachable_cb, cf))
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_call_forwarding_remove(struct ofono_call_forwarding *cf)
{
	struct forw_data *data = ofono_call_forwarding_get_data(cf);

	if (!data)
		return;

	ofono_call_forwarding_set_data(cf, NULL);
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

void isi_call_forwarding_init()
{
	ofono_call_forwarding_driver_register(&driver);
}

void isi_call_forwarding_exit()
{
	ofono_call_forwarding_driver_unregister(&driver);
}
