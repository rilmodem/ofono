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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <gisi/client.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/phonebook.h>
#include "util.h"

#include "isimodem.h"
#include "isiutil.h"
#include "sim.h"
#include "debug.h"

struct pb_data {
	GIsiClient *client;
};

static int decode_read_response(const unsigned char *msg, size_t len,
				struct ofono_phonebook *pb)
{
	GIsiSubBlockIter iter;

	char *name = NULL;
	char *number = NULL;
	char *sne = NULL;
	char *anr = NULL;
	char *email = NULL;

	int location = -1;
	guint8 status = 0;

	if (len < 3 || msg[0] != SIM_PB_RESP_SIM_PB_READ)
		goto error;

	if (msg[1] != SIM_PB_READ)
		goto error;

	for (g_isi_sb_iter_init_full(&iter, msg, len, 3, TRUE, msg[2]);
	     g_isi_sb_iter_is_valid(&iter);
	     g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SIM_PB_ADN: {
			guint16 loc;
			guint8 namelen;
			guint8 numberlen;

			if (!g_isi_sb_iter_get_word(&iter, &loc, 4) ||
				!g_isi_sb_iter_get_byte(&iter, &namelen, 6) ||
				!g_isi_sb_iter_get_byte(&iter, &numberlen, 7))
				goto error;

			if (!g_isi_sb_iter_get_alpha_tag(&iter, &name,
						namelen * 2, 8))
				goto error;

			if (!g_isi_sb_iter_get_alpha_tag(&iter, &number,
						numberlen * 2, 8 + namelen * 2))
				goto error;

			location = loc;
			break;
		}

		case SIM_PB_SNE: {
			guint8 snelen;

			if (!g_isi_sb_iter_get_byte(&iter, &snelen, 6))
				goto error;

			if (!g_isi_sb_iter_get_alpha_tag(&iter, &sne,
						snelen * 2, 8))
				goto error;
			break;
		}

		case SIM_PB_ANR: {
			guint8 anrlen;

			if (!g_isi_sb_iter_get_byte(&iter, &anrlen, 6))
				goto error;

			if (!g_isi_sb_iter_get_alpha_tag(&iter, &anr,
						anrlen * 2, 8))
				goto error;
			break;
		}

		case SIM_PB_EMAIL: {
			guint8 emaillen;

			if (!g_isi_sb_iter_get_byte(&iter, &emaillen, 6))
				goto error;

			if (!g_isi_sb_iter_get_alpha_tag(&iter, &email,
						emaillen * 2, 8))
				goto error;
			break;
		}

		case SIM_PB_STATUS:

			if (!g_isi_sb_iter_get_byte(&iter, &status, 4))
				goto error;

			break;

		default:
			DBG("Skipping sub-block: %s (%zd bytes)",
				sim_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	if (status != SIM_SERV_OK) {
		DBG("Request failed: %s (0x%02X)",
			sim_isi_cause_name(status), status);
		goto error;
	}

	ofono_phonebook_entry(pb, -1, number, -1, name, -1, NULL,
				anr, -1, sne, email, NULL, NULL);

error:
	g_free(name);
	g_free(number);
	g_free(sne);
	g_free(anr);
	g_free(email);

	return location;
}

static void read_next_entry(GIsiClient *client, int location,
				GIsiResponseFunc read_cb,
				struct isi_cb_data *cbd)
{
	ofono_phonebook_cb_t cb = cbd->cb;
	const unsigned char msg[] = {
		SIM_PB_REQ_SIM_PB_READ,
		SIM_PB_READ,
		2,				/* number of subblocks */
		0, SIM_PB_LOCATION_SEARCH,	/* subblock id */
		0, 8,				/* subblock size */
		0, SIM_PB_ADN,
		location >> 8, location & 0xFF,	/* read next entry after
						 * specified by location */
		0, SIM_PB_INFO_REQUEST,		/* subblock id */
		0, 16,				/* subblock size */
		4,				/* number of tags */
		0,				/* filler */
		0, SIM_PB_ADN,			/* tags */
		0, SIM_PB_SNE,
		0, SIM_PB_ANR,
		0, SIM_PB_EMAIL,
		0, 0				/* filler */
	};

	if (location < 0)
		goto error;

	if (!cbd)
		goto error;

	if (g_isi_request_make(client, msg, sizeof(msg), SIM_TIMEOUT,
				read_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static gboolean read_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_phonebook_cb_t cb = cbd->cb;
	int location;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	location = decode_read_response(data, len, cbd->user);
	if (location != -1) {
		read_next_entry(client, location, read_resp_cb, cbd);
		return TRUE;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return TRUE;
}

static void isi_export_entries(struct ofono_phonebook *pb, const char *storage,
				ofono_phonebook_cb_t cb, void *data)
{
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	struct isi_cb_data *cbd = isi_cb_data_new(pb, cb, data);
	const unsigned char msg[] = {
		SIM_PB_REQ_SIM_PB_READ,
		SIM_PB_READ,
		2,				/* number of subblocks */
		0, SIM_PB_LOCATION,		/* subblock id */
		0, 8,				/* subblock size */
		0, SIM_PB_ADN,
		0xFF, 0xFF,			/* read first entry in pb */
		0, SIM_PB_INFO_REQUEST,		/* subblock id */
		0, 16,				/* subblock size */
		4,				/* number of tags */
		0,				/* filler */
		0, SIM_PB_ADN,			/* tags */
		0, SIM_PB_SNE,
		0, SIM_PB_ANR,
		0, SIM_PB_EMAIL,
		0, 0				/* filler */
	};

	if (!cbd)
		goto error;

	if (strcmp(storage, "SM"))
		goto error;

	if (g_isi_request_make(pbd->client, msg, sizeof(msg), SIM_TIMEOUT,
				read_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean isi_phonebook_register(gpointer user)
{
	struct ofono_phonebook *pb = user;

	ofono_phonebook_register(pb);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	struct ofono_phonebook *pb = opaque;
	const char *debug = NULL;

	if (!alive) {
		DBG("Unable to bootstrap phonebook driver");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	debug = getenv("OFONO_ISI_DEBUG");
	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "sim") == 0))
		g_isi_client_set_debug(client, sim_debug, NULL);

	g_idle_add(isi_phonebook_register, pb);
}

static int isi_phonebook_probe(struct ofono_phonebook *pb, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct pb_data *data = g_try_new0(struct pb_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SIM);
	if (!data->client) {
		g_free(data);
		return -ENOMEM;
	}

	ofono_phonebook_set_data(pb, data);

	if (!g_isi_verify(data->client, reachable_cb, pb))
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_phonebook_remove(struct ofono_phonebook *pb)
{
	struct pb_data *data = ofono_phonebook_get_data(pb);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_phonebook_driver driver = {
	.name			= "isimodem",
	.probe			= isi_phonebook_probe,
	.remove			= isi_phonebook_remove,
	.export_entries		= isi_export_entries
};

void isi_phonebook_init()
{
	ofono_phonebook_driver_register(&driver);
}

void isi_phonebook_exit()
{
	ofono_phonebook_driver_unregister(&driver);
}
