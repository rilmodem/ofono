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
#include <ofono/call-barring.h>
#include "util.h"

#include "isimodem.h"
#include "isiutil.h"
#include "ss.h"
#include "debug.h"

struct barr_data {
	GIsiClient *client;
};

static int lock_code_to_mmi(char const *lock)
{
	if (strcmp(lock, "AO") == 0)
		return SS_GSM_BARR_ALL_OUT;
	else if (strcmp(lock, "OI") == 0)
		return SS_GSM_BARR_OUT_INTER;
	else if (strcmp(lock, "OX") == 0)
		return SS_GSM_BARR_OUT_INTER_EXC_HOME;
	else if (strcmp(lock, "AI") == 0)
		return SS_GSM_BARR_ALL_IN;
	else if (strcmp(lock, "IR") == 0)
		return SS_GSM_BARR_ALL_IN_ROAM;
	else if (strcmp(lock, "AB") == 0)
		return SS_GSM_ALL_BARRINGS;
	else if (strcmp(lock, "AG") == 0)
		return SS_GSM_OUTGOING_BARR_SERV;
	else if (strcmp(lock, "AC") == 0)
		return SS_GSM_INCOMING_BARR_SERV;
	else
		return 0;
}

static gboolean check_response_status(const GIsiMessage *msg, uint8_t msgid)
{
	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			ss_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}
	return TRUE;
}

static void set_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_barring_set_cb_t cb = cbd->cb;
	uint8_t type;

	if (!check_response_status(msg, SS_SERVICE_COMPLETED_RESP))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 0, &type))
		goto error;

	if (type != SS_ACTIVATION && type != SS_DEACTIVATION)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}


static void isi_set(struct ofono_call_barring *barr, const char *lock,
			int enable, const char *passwd, int cls,
			ofono_call_barring_set_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code = lock_code_to_mmi(lock);

	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		enable ? SS_ACTIVATION : SS_DEACTIVATION,
		SS_ALL_TELE_AND_BEARER,
		ss_code >> 8, ss_code & 0xFF,	/* Service code */
		SS_SEND_ADDITIONAL_INFO,
		1,			/* Subblock count */
		SS_GSM_PASSWORD,
		28,			/* Subblock length */
		0, passwd[0], 0, passwd[1],
		0, passwd[2], 0, passwd[3],
		0, 0, 0, 0, 0, 0, 0, 0,	/* Filler */
		0, 0, 0, 0, 0, 0, 0, 0,	/* Filler */
		0, 0			/* Filler */
	};

	DBG("lock code %s enable %d class %d password %s",
		lock, enable, cls, passwd);

	if (cbd == NULL || bd == NULL)
		goto error;

	if (g_isi_client_send(bd->client, msg, sizeof(msg),
				set_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void update_status_mask(unsigned int *mask, int bsc)
{
	switch (bsc) {

	case SS_GSM_TELEPHONY:
		*mask |= 1;
		break;

	case SS_GSM_ALL_DATA_TELE:
		*mask |= 1 << 1;
		break;

	case SS_GSM_FACSIMILE:
		*mask |= 1 << 2;
		break;

	case SS_GSM_SMS:
		*mask |= 1 << 3;
		break;

	case SS_GSM_ALL_DATA_CIRCUIT_SYNC:
		*mask |= 1 << 4;
		break;

	case SS_GSM_ALL_DATA_CIRCUIT_ASYNC:
		*mask |= 1 << 5;
		break;

	case SS_GSM_ALL_DATA_PACKET_SYNC:
		*mask |= 1 << 6;
		break;

	case SS_GSM_ALL_PAD_ACCESS:
		*mask |= 1 << 7;
		break;

	default:
		DBG("Unknown BSC: 0x%04X", bsc);
		break;
	}
}

static void query_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_barring_query_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;
	uint32_t mask = 0;
	uint8_t type;
	uint8_t count = 0;
	uint8_t bsc = 0;
	uint8_t i;

	if (!check_response_status(msg, SS_SERVICE_COMPLETED_RESP))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 0, &type))
		goto error;

	if (type != SS_INTERROGATION)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 6);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SS_GSM_BSC_INFO)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &count, 2))
			goto error;

		for (i = 0; i < count; i++) {

			if (!g_isi_sb_iter_get_byte(&iter, &bsc, 3 + i))
				goto error;

			update_status_mask(&mask, bsc);
		}
	}

	DBG("mask=0x%04X", mask);
	CALLBACK_WITH_SUCCESS(cb, mask, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
}

static void isi_query(struct ofono_call_barring *barr, const char *lock,
			int cls, ofono_call_barring_query_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code = lock_code_to_mmi(lock);

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_ALL_TELE_AND_BEARER,
		ss_code >> 8, ss_code & 0xFF,	/* services code */
		SS_SEND_ADDITIONAL_INFO,	/* Get BER-encoded result */
		0				/* Subblock count */
	};

	DBG("barring query lock code %s", lock);

	if (cbd == NULL || bd == NULL)
		goto error;

	if (g_isi_client_send(bd->client, msg, sizeof(msg),
				query_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static void set_passwd_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_call_barring_set_cb_t cb = cbd->cb;
	uint8_t type;

	if (!check_response_status(msg, SS_SERVICE_COMPLETED_RESP))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 0, &type))
		goto error;

	if (type != SS_GSM_PASSWORD_REGISTRATION)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void isi_set_passwd(struct ofono_call_barring *barr, const char *lock,
				const char *old_passwd, const char *new_passwd,
				ofono_call_barring_set_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code = lock_code_to_mmi(lock);

	const uint8_t msg[] = {
		SS_SERVICE_REQ,
		SS_GSM_PASSWORD_REGISTRATION,
		SS_ALL_TELE_AND_BEARER,
		ss_code >> 8, ss_code & 0xFF,	/* Service code */
		SS_SEND_ADDITIONAL_INFO,
		1,				/* Subblock count */
		SS_GSM_PASSWORD,
		28,				/* Subblock length */
		0, old_passwd[0], 0, old_passwd[1],
		0, old_passwd[2], 0, old_passwd[3],
		0, new_passwd[0], 0, new_passwd[1],
		0, new_passwd[2], 0, new_passwd[3],
		0, new_passwd[0], 0, new_passwd[1],
		0, new_passwd[2], 0, new_passwd[3],
		0, 0				/* Filler */
	};

	DBG("lock code %s (%u) old password %s new password %s",
		lock, ss_code, old_passwd, new_passwd);

	if (cbd == NULL || bd == NULL)
		goto error;

	if (g_isi_client_send(bd->client, msg, sizeof(msg),
				set_passwd_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_call_barring *barr = data;

	if (g_isi_msg_error(msg) < 0)
		return;

	ISI_VERSION_DBG(msg);

	ofono_call_barring_register(barr);
}


static int isi_call_barring_probe(struct ofono_call_barring *barr,
					unsigned int vendor, void *user)
{
	GIsiModem *modem = user;
	struct barr_data *bd;

	bd = g_try_new0(struct barr_data, 1);
	if (bd == NULL)
		return -ENOMEM;

	bd->client = g_isi_client_create(modem, PN_SS);
	if (bd->client == NULL) {
		g_free(bd);
		return -ENOMEM;
	}

	ofono_call_barring_set_data(barr, bd);

	g_isi_client_verify(bd->client, reachable_cb, barr, NULL);

	return 0;
}

static void isi_call_barring_remove(struct ofono_call_barring *barr)
{
	struct barr_data *data = ofono_call_barring_get_data(barr);

	ofono_call_barring_set_data(barr, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_call_barring_driver driver = {
	.name			= "isimodem",
	.probe			= isi_call_barring_probe,
	.remove			= isi_call_barring_remove,
	.set			= isi_set,
	.query			= isi_query,
	.set_passwd		= isi_set_passwd
};

void isi_call_barring_init(void)
{
	ofono_call_barring_driver_register(&driver);
}

void isi_call_barring_exit(void)
{
	ofono_call_barring_driver_unregister(&driver);
}
