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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>
#include "simutil.h"

#include "isimodem.h"
#include "isiutil.h"
#include "sim.h"
#include "debug.h"

struct sim_data {
	GIsiClient *client;
	gboolean registered;
};

struct file_info {
	int fileid;
	int length;
	int structure;
	int record_length;
	unsigned char access[3];
	unsigned char file_status;
};

/* Returns file info */
static gboolean fake_file_info(gpointer user)
{
	struct isi_cb_data *cbd = user;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct file_info const *fi = cbd->user;

	DBG("Returning static file_info for %04x", fi->fileid);
	CALLBACK_WITH_SUCCESS(cb,
				fi->length, fi->structure, fi->record_length,
				fi->access, fi->file_status, cbd->data);
	g_free(cbd);
	return FALSE;
}

static void isi_read_file_info(struct ofono_sim *sim, int fileid,
				ofono_sim_file_info_cb_t cb, void *data)
{
	int i;
	static struct file_info const info[] = {
		{ SIM_EFSPN_FILEID, 17, 0, 0, { 0x0f, 0xff, 0xff }, 1 },
		{ SIM_EF_ICCID_FILEID, 10, 0, 0, { 0x0f, 0xff, 0xff }, 1 },
	};
	int N = sizeof(info) / sizeof(info[0]);
	struct isi_cb_data *cbd;

	for (i = 0; i < N; i++) {
		if (fileid == info[i].fileid) {
			cbd = isi_cb_data_new((void *)&info[i], cb, data);
			g_idle_add(fake_file_info, cbd);
			return;
		}
	}

	DBG("Not implemented (fileid = %04x)", fileid);
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, 0, data);
}

static gboolean spn_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_sim_read_cb_t cb = cbd->cb;
	unsigned char *spn = NULL;
	unsigned char buffer[17];
	int i;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto done;
	}

	if (len < 39 || msg[0] != SIM_SERV_PROV_NAME_RESP
		|| msg[1] != SIM_ST_READ_SERV_PROV_NAME)
		return FALSE;

	if (msg[2] != SIM_SERV_OK) {
		DBG("Request failed: %s (0x%02X)",
			sim_isi_cause_name(msg[2]), msg[2]);
		goto done;
	}

	spn = buffer;

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

done:
	if (spn)
		CALLBACK_WITH_SUCCESS(cb, spn, 17, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);

	g_free(cbd);
	return TRUE;
}

static gboolean isi_read_spn(struct ofono_sim *sim, struct isi_cb_data *cbd)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	const unsigned char msg[] = {
		SIM_SERV_PROV_NAME_REQ,
		SIM_ST_READ_SERV_PROV_NAME,
		0
	};

	return g_isi_request_make(sd->client, msg, sizeof(msg),
					SIM_TIMEOUT, spn_resp_cb, cbd) != NULL;
}

static gboolean read_iccid_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *user)
{
	struct isi_cb_data *cbd = user;
	ofono_sim_read_cb_t cb = cbd->cb;
	const unsigned char *msg = data;
	const unsigned char *iccid = NULL;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto done;
	}

	if (len < 3 || msg[0] != SIM_READ_FIELD_RESP || msg[1] != ICC)
		return FALSE;

	if (msg[2] == SIM_SERV_OK && len >= 13)
		iccid = msg + 3;
	else
		DBG("Error reading ICC ID");

done:
	if (iccid)
		CALLBACK_WITH_SUCCESS(cb, iccid, 10, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);

	g_free(cbd);
	return TRUE;
}

static gboolean isi_read_iccid(struct ofono_sim *sim, struct isi_cb_data *cbd)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	const unsigned char req[] = { SIM_READ_FIELD_REQ, ICC };

	return g_isi_request_make(sd->client, req, sizeof(req), SIM_TIMEOUT,
					read_iccid_resp_cb, cbd) != NULL;
}

static void isi_read_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);

	DBG("fileid = %04x", fileid);

	switch (fileid) {
	case SIM_EFSPN_FILEID:

		if (isi_read_spn(sim, cbd))
			return;
		break;

	case SIM_EF_ICCID_FILEID:

		if (isi_read_iccid(sim, cbd))
			return;
		break;

	default:
		DBG("Not implemented (fileid = %04x)", fileid);
	}

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	g_free(cbd);
}

static void isi_read_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)", fileid);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void isi_read_file_cyclic(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)", fileid);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void isi_write_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)", fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_write_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)", fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_write_file_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented (fileid = %04x)", fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean imsi_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_sim_imsi_cb_t cb = cbd->cb;

	char imsi[SIM_MAX_IMSI_LENGTH + 1];
	size_t i = 0;
	size_t j = 0;
	size_t octets = 0;

	if (!msg) {
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
	return TRUE;
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
	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
}

static void isi_sim_register(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	if (!sd->registered) {
		sd->registered = TRUE;
		ofono_sim_register(sim);
		ofono_sim_inserted_notify(sim, TRUE);
	}
}

static gboolean read_hplmn_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_sim *sim = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return TRUE;
	}

	if (len < 3 || msg[0] != SIM_NETWORK_INFO_RESP || msg[1] != READ_HPLMN)
		return FALSE;

	if (msg[2] != SIM_SERV_NOTREADY)
		isi_sim_register(sim);

	return TRUE;
}


static void isi_read_hplmn(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	const unsigned char req[] = {
		SIM_NETWORK_INFO_REQ,
		READ_HPLMN, 0
	};

	g_isi_request_make(sd->client, req, sizeof(req), SIM_TIMEOUT,
				read_hplmn_resp_cb, sim);
}

static void sim_ind_cb(GIsiClient *client,
			const void *restrict data, size_t len,
			uint16_t object, void *opaque)
{
	struct ofono_sim *sim = opaque;
	struct sim_data *sd = ofono_sim_get_data(sim);
	const unsigned char *msg = data;

	if (sd->registered)
		return;

	switch (msg[1]) {
	case SIM_ST_PIN:
		isi_sim_register(sim);
		break;
	case SIM_ST_INFO:
		isi_read_hplmn(sim);
		break;
	}
}

static void sim_reachable_cb(GIsiClient *client, gboolean alive,
				uint16_t object, void *opaque)
{
	struct ofono_sim *sim = opaque;

	if (!alive) {
		DBG("SIM client: %s", strerror(-g_isi_client_error(client)));
		ofono_sim_remove(sim);
		return;
	}

	DBG("%s (v.%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_isi_subscribe(client, SIM_IND, sim_ind_cb, opaque);

	/* Check if SIM is ready. */
	isi_read_hplmn(sim);
}

static int isi_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct sim_data *sd = g_try_new0(struct sim_data, 1);
	const char *debug = getenv("OFONO_ISI_DEBUG");

	if (!sd)
		return -ENOMEM;

	sd->client = g_isi_client_create(idx, PN_SIM);
	if (!sd->client)
		return -ENOMEM;

	ofono_sim_set_data(sim, sd);

	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "sim") == 0))
		g_isi_client_set_debug(sd->client, sim_debug, NULL);

	g_isi_verify(sd->client, sim_reachable_cb, sim);

	return 0;
}

static void isi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	if (!data)
		return;

	ofono_sim_set_data(sim, NULL);
	g_isi_client_destroy(data->client);
	g_free(data);
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
	.read_imsi		= isi_read_imsi,
};

void isi_sim_init()
{
	ofono_sim_driver_register(&driver);
}

void isi_sim_exit()
{
	ofono_sim_driver_unregister(&driver);
}
