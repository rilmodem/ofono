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
#include <ofono/phonebook.h>
#include "util.h"

#include "isimodem.h"
#include "isiutil.h"
#include "sim.h"
#include "debug.h"

struct pb_data {
	GIsiClient *client;
};

struct read_resp {
	uint8_t service_type;
	uint8_t sb_count;
	uint8_t data[];
};

static gboolean parse_adn(GIsiSubBlockIter *iter, uint16_t *location,
				char **name, char **number)
{
	uint8_t namelen;
	uint8_t numlen;

	if (!g_isi_sb_iter_get_word(iter, location, 4) ||
			!g_isi_sb_iter_get_byte(iter, &namelen, 6) ||
			!g_isi_sb_iter_get_byte(iter, &numlen, 7))
		return FALSE;

	if (!g_isi_sb_iter_get_alpha_tag(iter, name, namelen * 2, 8))
		return FALSE;

	if (!g_isi_sb_iter_get_alpha_tag(iter, number, numlen * 2,
			8 + namelen * 2)) {
		g_free(*name);
		return FALSE;
	}
	return TRUE;
}

static gboolean parse_sne(GIsiSubBlockIter *iter, char **sne)
{
	uint8_t len;

	if (!g_isi_sb_iter_get_byte(iter, &len, 6))
		return FALSE;

	if (!g_isi_sb_iter_get_alpha_tag(iter, sne, len * 2, 8))
		return FALSE;

	return TRUE;
}

static gboolean parse_anr(GIsiSubBlockIter *iter, char **anr)
{
	uint8_t len;

	if (!g_isi_sb_iter_get_byte(iter, &len, 6))
		return FALSE;

	if (!g_isi_sb_iter_get_alpha_tag(iter, anr, len * 2, 8))
		return FALSE;

	return TRUE;
}

static gboolean parse_email(GIsiSubBlockIter *iter, char **email)
{
	uint8_t len;

	if (!g_isi_sb_iter_get_byte(iter, &len, 6))
		return FALSE;

	if (!g_isi_sb_iter_get_alpha_tag(iter, email, len * 2, 8))
		return FALSE;

	return TRUE;
}

static gboolean decode_response(const GIsiMessage *msg, uint16_t *location,
				void *data)
{
	struct ofono_phonebook *pb = data;
	const struct read_resp *resp = g_isi_msg_data(msg);
	size_t len = g_isi_msg_data_len(msg);

	GIsiSubBlockIter iter;

	char *name = NULL;
	char *number = NULL;
	char *sne = NULL;
	char *anr = NULL;
	char *email = NULL;

	uint8_t status = 0;
	gboolean success = FALSE;

	if (g_isi_msg_id(msg) != SIM_PB_RESP_SIM_PB_READ ||
			resp == NULL || len < sizeof(struct read_resp) ||
			resp->service_type != SIM_PB_READ)
		return FALSE;

	for (g_isi_sb_iter_init_full(&iter, msg, 2, TRUE, resp->sb_count);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case SIM_PB_ADN:

			if (!parse_adn(&iter, location, &name, &number))
				goto error;
			success = TRUE;
			break;

		case SIM_PB_SNE:

			if (!parse_sne(&iter, &sne))
				goto error;
			break;

		case SIM_PB_ANR:

			if (!parse_anr(&iter, &anr))
				goto error;
			break;

		case SIM_PB_EMAIL:

			if (!parse_email(&iter, &email))
				goto error;
			break;

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

	if (status == SIM_SERV_OK)
		ofono_phonebook_entry(pb, -1, number, -1, name, -1, NULL,
					anr, -1, sne, email, NULL, NULL);
error:
	g_free(name);
	g_free(number);
	g_free(sne);
	g_free(anr);
	g_free(email);

	return success;
}

static void read_next_entry(GIsiClient *client, uint16_t location,
				GIsiNotifyFunc notify, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_phonebook_cb_t cb = cbd->cb;
	const uint8_t msg[] = {
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

	if (cbd == NULL)
		goto error;

	if (g_isi_client_send(client, msg, sizeof(msg), notify, cbd, NULL))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void read_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	struct ofono_phonebook *pb = cbd->user;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);

	ofono_phonebook_cb_t cb = cbd->cb;
	uint16_t location;

	if (g_isi_msg_error(msg) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
		return;
	}

	if (decode_response(msg, &location, cbd->user)) {
		read_next_entry(pbd->client, location, read_resp_cb, cbd);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	g_free(cbd);
}

static void isi_export_entries(struct ofono_phonebook *pb, const char *storage,
				ofono_phonebook_cb_t cb, void *data)
{
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	struct isi_cb_data *cbd = isi_cb_data_new(pb, cb, data);
	const uint8_t msg[] = {
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
	size_t len = sizeof(msg);

	if (cbd == NULL || pbd == NULL || strcmp(storage, "SM") != 0)
		goto error;

	if (g_isi_client_send(pbd->client, msg, len, read_resp_cb, cbd, NULL))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_phonebook *pb = data;

	if (g_isi_msg_error(msg) < 0) {
		ofono_phonebook_remove(pb);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	ofono_phonebook_register(pb);
}

static int isi_phonebook_probe(struct ofono_phonebook *pb, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct pb_data *data;

	data = g_try_new0(struct pb_data, 1);
	if (data == NULL)
		return -ENOMEM;

	data->client = g_isi_client_create(modem, PN_SIM);
	if (data->client == NULL) {
		g_free(data);
		return -ENOMEM;
	}

	ofono_phonebook_set_data(pb, data);

	g_isi_client_verify(data->client, reachable_cb, pb, NULL);

	return 0;
}

static void isi_phonebook_remove(struct ofono_phonebook *pb)
{
	struct pb_data *data = ofono_phonebook_get_data(pb);

	ofono_phonebook_set_data(pb, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_phonebook_driver driver = {
	.name			= "isimodem",
	.probe			= isi_phonebook_probe,
	.remove			= isi_phonebook_remove,
	.export_entries		= isi_export_entries
};

void isi_phonebook_init(void)
{
	ofono_phonebook_driver_register(&driver);
}

void isi_phonebook_exit(void)
{
	ofono_phonebook_driver_unregister(&driver);
}
