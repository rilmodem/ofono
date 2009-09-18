/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "isi.h"
#include "simutil.h"

#define PN_SIM			0x09
#define SIM_TIMEOUT		5
#define SIM_MAX_IMSI_LENGTH	15

enum return_code {
	SIM_SERV_OK = 0x01,
};

enum message_id {
	SIM_IMSI_REQ_READ_IMSI = 0x1D,
	SIM_IMSI_RESP_READ_IMSI = 0x1E,
	SIM_SERV_PROV_NAME_REQ = 0x21,
	SIM_SERV_PROV_NAME_RESP = 0x22
};

enum service_types {
	SIM_ST_READ_SERV_PROV_NAME = 0x2C,
	READ_IMSI = 0x2D,
};

struct sim_data {
	GIsiClient *client;
};

static void sim_debug(const void *restrict buf, size_t len, void *data)
{
	DBG("");
	dump_msg(buf, len);
}

/* Returns fake (static) file info for EFSPN */
static gboolean efspn_file_info(gpointer user)
{
	struct isi_cb_data *cbd = user;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	unsigned char access[3] = { 0x0f, 0xff, 0xff };

	DBG("Returning dummy file_info for EFSPN");
	CALLBACK_WITH_SUCCESS(cb, 17, 0, 0, access, cbd->data);

	g_free(cbd);
	return FALSE;
}

static void isi_read_file_info(struct ofono_sim *sim, int fileid,
				ofono_sim_file_info_cb_t cb, void *data)
{
	if (fileid == SIM_EFSPN_FILEID) {
		/* Fake response for EFSPN  */
		struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);
		g_idle_add(efspn_file_info, cbd);
		return;
	}

	DBG("Not implemented (fileid = %04x)",fileid);
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, data);
}

static bool spn_resp_cb(GIsiClient * client, const void *restrict data,
			size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_sim_read_cb_t cb = cbd->cb;
	unsigned char spn[17] = { 0xff };
	int i;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 39 || msg[0] != SIM_SERV_PROV_NAME_RESP)
		 goto error;

	if (msg[1] != SIM_ST_READ_SERV_PROV_NAME || msg[2] != SIM_SERV_OK)
		goto error;

	/* Set display condition bits */
	spn[0] = ((msg[38] & 1) << 1) + (msg[37] & 1);
	/* Dirty conversion from 16bit unicode to ascii */
	for (i = 0; i < 16; i++) {
		unsigned char c = msg[3 + i * 2 + 1];
		if (c == 0)
			c = 0xff;
		else if (!g_ascii_isprint(c))
			c = '?';
		spn[i + 1] = c;
	}
	DBG("SPN read successfully");
	CALLBACK_WITH_SUCCESS(cb, spn, 17, cbd->data);
	goto out;

error:
	DBG("Error reading SPN");
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_read_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	struct isi_cb_data *cbd = NULL;

	if (fileid == SIM_EFSPN_FILEID) {
		/* Hack support for EFSPN reading */
		struct sim_data *simd = ofono_sim_get_data(sim);
		const unsigned char msg[] = {
			SIM_SERV_PROV_NAME_REQ,
			SIM_ST_READ_SERV_PROV_NAME,
			0
		};
		cbd = isi_cb_data_new(NULL, cb, data);

		if (!simd)
			goto error;

		cbd->user = sim;

		if (g_isi_request_make(simd->client, msg, sizeof(msg),
				       SIM_TIMEOUT, spn_resp_cb, cbd))
			return;
	}
error:
	if (cbd)
		g_free(cbd);

	DBG("Not implemented (fileid = %04x)",fileid);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void isi_read_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)",fileid);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void isi_read_file_cyclic(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)",fileid);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void isi_write_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)",fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_write_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)",fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_write_file_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)",fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static bool imsi_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_sim_imsi_cb_t cb = cbd->cb;

	char imsi[SIM_MAX_IMSI_LENGTH + 1];
	size_t i = 0;
	size_t j = 0;
	size_t octets = 0;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 5 || msg[0] != SIM_IMSI_RESP_READ_IMSI)
		 goto error;

	if (msg[1] != READ_IMSI || msg[2] != SIM_SERV_OK)
		goto error;

	octets = msg[3];
	if (octets != 8 || octets > len)
		goto error;
	
	msg += 4;

	/* Ignore the low-order semi-octet of the first byte */
	imsi[j] = ((msg[i] & 0xF0) >> 4) + '0';

	for (i++, j++; i < octets && j < SIM_MAX_IMSI_LENGTH; i++) {

		char nibble;
		imsi[j++] = (msg[i] & 0x0F) + '0';

		nibble = (msg[i] & 0xF0) >> 4;
		if (nibble != 0x0F)
			imsi[j++] = nibble + '0';
	}

	imsi[j] = '\0';
	CALLBACK_WITH_SUCCESS(cb, imsi, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);
	const unsigned char msg[] = {
		SIM_IMSI_REQ_READ_IMSI,
		READ_IMSI
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(sd->client, msg, sizeof(msg), 
				SIM_TIMEOUT,
				imsi_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static gboolean isi_sim_register(gpointer user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);

	g_isi_client_set_debug(sd->client, sim_debug, NULL);

	ofono_sim_register(sim);

	return FALSE;
}

static int isi_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct sim_data *sd = g_try_new0(struct sim_data, 1);

	if (!sd)
		return -ENOMEM;

	sd->client = g_isi_client_create(idx, PN_SIM);
	if (!sd->client)
		return -ENOMEM;

	ofono_sim_set_data(sim, sd);

	g_idle_add(isi_sim_register, sim);

	return 0;
}

static void isi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_sim_driver driver = {
	.name			= "isimodem",
	.probe			= isi_sim_probe,
	.remove			= isi_sim_remove,
	.read_file_info		= isi_read_file_info,
	.read_file_transparent	= isi_read_file_transparent,
	.read_file_linear	= isi_read_file_linear,
	.read_file_cyclic	= isi_read_file_cyclic,
	.write_file_transparent	= isi_write_file_transparent,
	.write_file_linear	= isi_write_file_linear,
	.write_file_cyclic	= isi_write_file_cyclic,
	.read_imsi		= isi_read_imsi
};

void isi_sim_init()
{
	ofono_sim_driver_register(&driver);
}

void isi_sim_exit()
{
	ofono_sim_driver_unregister(&driver);
}
