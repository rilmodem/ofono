/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Alexander Kanavin <alexander.kanavin@nokia.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/phonebook.h>
#include "util.h"

#include "isi.h"

#define PHONEBOOK_TIMEOUT	5
#define PN_SIM			0x09

enum pb_message_id {
	SIM_PB_REQ_SIM_PB_READ = 0xDC,
	SIM_PB_RESP_SIM_PB_READ = 0xDD
};

enum pb_service_types {
	SIM_PB_READ = 0x0F
};

enum pb_sub_block_id {
	SIM_PB_INFO_REQUEST = 0xE4,
	SIM_PB_STATUS = 0xFB,
	SIM_PB_LOCATION = 0xFE,
	SIM_PB_LOCATION_SEARCH = 0xFF
};

enum pb_type {
	SIM_PB_ADN = 0xC8
};

enum pb_tag {
	SIM_PB_ANR = 0xCA,
	SIM_PB_EMAIL = 0xDD,
	SIM_PB_SNE = 0xF7
};

enum pb_status {
	SIM_SERV_OK = 0x01
};

struct pb_data {
	GIsiClient *client;
};

static char *ucs2_to_utf8(const unsigned char *str, long len)
{
	char *utf8;
	utf8 = g_convert((const char *)str, len, "UTF-8//TRANSLIT", "UCS-2BE",
				NULL, NULL, NULL);
	return utf8;
}

static int decode_read_response(const unsigned char *msg, size_t len,
				struct ofono_phonebook *pb)
{
	int retval = -1;

	unsigned int i;
	unsigned int p;

	char *name = NULL;
	char *number = NULL;
	char *adn = NULL;
	char *snr = NULL;
	char *email = NULL;

	unsigned int location = 0;
	unsigned int status = 0;

	unsigned int messageid;
	unsigned int servicetype;
	unsigned int num_subblocks;

	if (len < 3)
		goto cleanup;

	messageid = msg[0];
	servicetype = msg[1];
	num_subblocks = msg[2];

	if (messageid != SIM_PB_RESP_SIM_PB_READ || servicetype != SIM_PB_READ)
		goto cleanup;

	p = 3;
	for (i=0; i < num_subblocks; i++) {
		unsigned int subblock_type;
		unsigned int subblock_len;

		if (p + 4 > len)
			goto cleanup;

		subblock_type = (msg[p] << 8) + msg[p + 1];
		subblock_len = (msg[p + 2] << 8) + msg[p + 3];

		switch (subblock_type) {

		case SIM_PB_ADN: {
			unsigned int namelength;
			unsigned int numberlength;

			if (p + 8 > len)
				goto cleanup;

			location = (msg[p + 4] << 8) + msg[p + 5];
			namelength = msg[p + 6];
			numberlength = msg[p + 7];

			if (p + 8 + namelength * 2 + numberlength * 2 > len)
				goto cleanup;

			name = ucs2_to_utf8(msg + p + 8, namelength * 2);
			number = ucs2_to_utf8(msg + p + 8 + namelength * 2,
						numberlength * 2);
			DBG("ADN subblock: name %s number %s location %i",
				name, number, location);
			break;
		}

		case SIM_PB_SNE: {
			unsigned int locsne;
			unsigned int snelength;
			unsigned int snefiller;

			if (p + 8 > len)
				goto cleanup;

			locsne = (msg[p + 4] << 8) + msg[p + 5];
			snelength = msg[p + 6];
			snefiller = msg[p + 7];

			if (p + 8 + snelength * 2 > len)
				goto cleanup;

			adn = ucs2_to_utf8(msg + p + 8, snelength * 2);
			DBG("SNE subblock: name %s", adn);
			break;
		}

		case SIM_PB_ANR: {
			unsigned int locanr;
			unsigned int anrlength;
			unsigned int anrfiller;

			if (p + 8 > len)
				goto cleanup;

			locanr = (msg[p + 4] << 8) + msg[p + 5];
			anrlength = msg[p + 6];
			anrfiller = msg[p + 7];

			if (p + 8 + anrlength * 2 > len)
				goto cleanup;

			snr = ucs2_to_utf8(msg + p + 8, anrlength * 2);
			DBG("ANR subblock: number %s", snr);
			break;
		}

		case SIM_PB_EMAIL: {
			unsigned int locemail;
			unsigned int emaillength;
			unsigned int emailfiller;

			if (p + 8 > len)
				goto cleanup;

			locemail = (msg[p + 4] << 8) + msg[p + 5];
			emaillength = msg[p + 6];
			emailfiller = msg[p + 7];

			if (p + 8 + emaillength * 2 > len)
				goto cleanup;

			email = ucs2_to_utf8(msg + p + 8, emaillength * 2);
			DBG("EMAIL subblock: email %s", email);
			break;
		}

		case SIM_PB_STATUS:
			if (p + 5 > len)
				goto cleanup;

			status = msg[p + 4];
			DBG("STATUS subblock: status %i", status);
			break;

		default:
			DBG("Unknown subblock in read response: type %i length %i",
				subblock_type, subblock_len);
			break;
		}

		p += subblock_len;
	}

	if (status == SIM_SERV_OK) {
		
		ofono_phonebook_entry(pb, -1, number, -1, name, -1, NULL,
					snr, -1, adn, email, NULL, NULL);
		retval = location;
	}

cleanup:
	g_free(name);
	g_free(number);
	g_free(adn);
	g_free(snr);
	g_free(email);

	return retval;
}

static void read_next_entry(GIsiClient *client, int location, GIsiResponseFunc read_cb, struct isi_cb_data *cbd)
{
	ofono_phonebook_cb_t cb = cbd->cb;
	const unsigned char msg[] = {
		SIM_PB_REQ_SIM_PB_READ,
		SIM_PB_READ,
		2,				/* number of subblocks */
		0, SIM_PB_LOCATION_SEARCH,	/* subblock id */
		0, 8,				/* subblock size */
		0, SIM_PB_ADN,
		location >> 8, location & 0xFF,	/* read next entry after specified by location */
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

	if (g_isi_request_make(client, msg, sizeof(msg), PHONEBOOK_TIMEOUT,
				read_cb, cbd))
		return;

error:
	{
		DECLARE_FAILURE(error);
		cb(&error, cbd->data);
		g_free(cbd);
	}
}

static bool read_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_phonebook_cb_t cb = cbd->cb;
	int location;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	location = decode_read_response(data, len, cbd->user);
	if (location != -1) {
		read_next_entry(client, location, read_resp_cb, cbd);
		return true;
	}

	{
		DECLARE_SUCCESS(error);
		cb(&error, cbd->data);
		goto out;
	}

error:
	{
		DECLARE_FAILURE(error);
		cb(&error, cbd->data);
	}

out:
	g_free(cbd);
	return true;
}

static void isi_export_entries(struct ofono_phonebook *pb, const char *storage,
				ofono_phonebook_cb_t cb, void *data)
{
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);
	const unsigned char msg[] = {
		SIM_PB_REQ_SIM_PB_READ,
		SIM_PB_READ,
		2,				/* number of subblocks */
		0, SIM_PB_LOCATION,		/* subblock id */
		0, 8,				/* subblock size */
		0, SIM_PB_ADN,
		0xFF, 0xFF,			/* read first entry in the phonebook */
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

	cbd->user = pb;

	if (g_isi_request_make(pbd->client, msg, sizeof(msg), PHONEBOOK_TIMEOUT,
				read_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static gboolean isi_phonebook_register(gpointer user)
{
	struct ofono_phonebook *pb = user;

	ofono_phonebook_register(pb);

	return FALSE;
}

static int isi_phonebook_probe(struct ofono_phonebook *pb, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct pb_data *data = g_try_new0(struct pb_data, 1);

	if (!data)
		return -ENOMEM;

	DBG("idx=%p", idx);

	data->client = g_isi_client_create(idx, PN_SIM);
	if (!data->client) {
		g_free(data);
		return -ENOMEM;
	}

	ofono_phonebook_set_data(pb, data);

	/* FIXME: If this is running on a phone itself, phonebook
	 * initialization needs to be done here */

	g_idle_add(isi_phonebook_register, pb);

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
