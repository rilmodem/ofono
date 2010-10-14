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

static gboolean set_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_barring_set_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		return FALSE;

	if (msg[1] != SS_ACTIVATION && msg[1] != SS_DEACTIVATION)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;
}


static void isi_set(struct ofono_call_barring *barr, const char *lock,
			int enable, const char *passwd, int cls,
			ofono_call_barring_set_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code = lock_code_to_mmi(lock);

	unsigned char msg[] = {
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

	DBG("lock code %s enable %d class %d password %s\n",
		lock, enable, cls, passwd);

	if (cbd && g_isi_request_make(bd->client, msg, sizeof(msg), SS_TIMEOUT,
					set_resp_cb, cbd))
		return;

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
		DBG("Unknown BSC: 0x%04X\n", bsc);
		break;
	}
}

static gboolean query_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	GIsiSubBlockIter iter;
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_barring_query_cb_t cb = cbd->cb;

	guint32 mask = 0;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 7 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		return FALSE;

	if (msg[1] != SS_INTERROGATION)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_STATUS_RESULT:
			break;

		case SS_GSM_BSC_INFO: {

			guint8 count = 0;
			guint8 i;

			if (!g_isi_sb_iter_get_byte(&iter, &count, 2))
				goto error;

			for (i = 0; i < count; i++) {

				guint8 bsc = 0;

				if (!g_isi_sb_iter_get_byte(&iter, &bsc, 3 + i))
					goto error;

				update_status_mask(&mask, bsc);
			}
			break;
		}

		case SS_GSM_ADDITIONAL_INFO:
			break;

		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				ss_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	DBG("mask=0x%04X\n", mask);
	CALLBACK_WITH_SUCCESS(cb, mask, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, 0, cbd->data);

out:
	g_free(cbd);
	return TRUE;

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

	DBG("barring query lock code %s\n", lock);

	if (cbd && g_isi_request_make(bd->client, msg, sizeof(msg), SS_TIMEOUT,
					query_resp_cb, cbd))
		return;

	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static gboolean set_passwd_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_barring_set_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		return FALSE;

	if (msg[1] != SS_GSM_PASSWORD_REGISTRATION)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;
}

static void isi_set_passwd(struct ofono_call_barring *barr, const char *lock,
				const char *old_passwd, const char *new_passwd,
				ofono_call_barring_set_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code = lock_code_to_mmi(lock);

	unsigned char msg[] = {
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

	DBG("lock code %s (%u) old password %s new password %s\n",
		lock, ss_code, old_passwd, new_passwd);

	if (cbd &&
		g_isi_request_make(bd->client, msg, sizeof(msg), SS_TIMEOUT,
				set_passwd_resp_cb, cbd))
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static gboolean isi_call_barring_register(gpointer user)
{
	struct ofono_call_barring *cb = user;

	ofono_call_barring_register(cb);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	struct ofono_call_barring *barr = opaque;
	const char *debug = NULL;

	if (!alive) {
		DBG("Unable to bootstrap call barring driver");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	debug = getenv("OFONO_ISI_DEBUG");
	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "ss") == 0))
		g_isi_client_set_debug(client, ss_debug, NULL);

	g_idle_add(isi_call_barring_register, barr);
}


static int isi_call_barring_probe(struct ofono_call_barring *barr,
					unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct barr_data *data = g_try_new0(struct barr_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SS);
	if (!data->client)
		return -ENOMEM;

	ofono_call_barring_set_data(barr, data);
	if (!g_isi_verify(data->client, reachable_cb, barr))
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_call_barring_remove(struct ofono_call_barring *barr)
{
	struct barr_data *data = ofono_call_barring_get_data(barr);

	if (!data)
		return;

	ofono_call_barring_set_data(barr, NULL);
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

void isi_call_barring_init()
{
	ofono_call_barring_driver_register(&driver);
}

void isi_call_barring_exit()
{
	ofono_call_barring_driver_unregister(&driver);
}
