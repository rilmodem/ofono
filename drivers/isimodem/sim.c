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

#include <gisi/message.h>
#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>
#include "simutil.h"

#include "isimodem.h"
#include "isiutil.h"
#include "sim.h"
#include "debug.h"

#define SIM_MAX_SPN_LENGTH	16

struct sim_data {
	GIsiClient *client;
	gboolean registered;
};

struct sim_imsi {
	uint8_t length;
	uint8_t imsi[8];
};

struct sim_iccid {
	uint8_t id[10];
};

struct sim_spn {
	uint16_t name[SIM_MAX_SPN_LENGTH + 1];
	uint8_t disp_home;
	uint8_t disp_roam;
};

struct file_info {
	int fileid;
	int length;
	int structure;
	int record_length;
	uint8_t access[3];
	uint8_t file_status;
};

/* Returns file info */
static gboolean fake_file_info(gpointer user)
{
	struct isi_cb_data *cbd = user;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct file_info const *fi = cbd->user;

	DBG("Returning static file info for %04X", fi->fileid);
	CALLBACK_WITH_SUCCESS(cb, fi->length, fi->structure, fi->record_length,
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
			cbd = isi_cb_data_new((void *) &info[i], cb, data);
			g_idle_add(fake_file_info, cbd);
			return;
		}
	}

	DBG("Fileid %04X not implemented", fileid);
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, 0, data);
}

static gboolean check_response_status(const GIsiMessage *msg, uint8_t msgid,
					uint8_t service)
{
	uint8_t type;
	uint8_t cause;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			sim_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 1, &cause) || cause != SIM_SERV_OK) {
		DBG("Request failed: %s", sim_isi_cause_name(cause));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &type) || type != service) {
		DBG("Unexpected service: 0x%02X", type);
		return FALSE;
	}
	return TRUE;
}

static void spn_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sim_read_cb_t cb = cbd->cb;

	const struct sim_spn *resp = NULL;
	size_t len = sizeof(struct sim_spn);

	uint8_t spn[SIM_MAX_SPN_LENGTH + 1];
	int i;

	if (!check_response_status(msg, SIM_SERV_PROV_NAME_RESP,
					SIM_ST_READ_SERV_PROV_NAME))
		goto error;

	if (!g_isi_msg_data_get_struct(msg, 2, (const void **) &resp, len))
		goto error;

	/* Set display condition bits */
	spn[0] = (resp->disp_home & 0x01) | ((resp->disp_roam & 0x01) << 1);

	/* Convert from a NULL-terminated UCS-2 string to ASCII */
	for (i = 0; i < SIM_MAX_SPN_LENGTH; i++) {
		uint16_t c = resp->name[i] >> 8 | resp->name[i] << 8;

		if (c == 0)
			c = 0xFF;
		else if (!g_ascii_isprint(c))
			c = '?';

		spn[i + 1] = c;
	}

	CALLBACK_WITH_SUCCESS(cb, spn, sizeof(spn), cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static gboolean isi_read_spn(struct ofono_sim *sim, struct isi_cb_data *cbd)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	const uint8_t msg[] = {
		SIM_SERV_PROV_NAME_REQ,
		SIM_ST_READ_SERV_PROV_NAME,
		0
	};

	if (sd == NULL)
		return FALSE;

	return g_isi_client_send(sd->client, msg, sizeof(msg),
					spn_resp_cb, cbd, g_free);
}

static void read_iccid_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct sim_iccid *icc;
	size_t len = sizeof(struct sim_iccid);

	if (!check_response_status(msg, SIM_READ_FIELD_RESP, ICC))
		goto error;

	if (!g_isi_msg_data_get_struct(msg, 2, (const void **) &icc, len))
		goto error;

	CALLBACK_WITH_SUCCESS(cb, icc->id, 10, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static gboolean isi_read_iccid(struct ofono_sim *sim, struct isi_cb_data *cbd)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	const uint8_t req[] = {
		SIM_READ_FIELD_REQ,
		ICC,
	};

	if (sd == NULL)
		return FALSE;

	return g_isi_client_send(sd->client, req, sizeof(req),
					read_iccid_resp_cb, cbd, g_free);
}

static void isi_read_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	struct isi_cb_data *cbd;
	gboolean done;

	cbd = isi_cb_data_new(sim, cb, data);
	if (cbd == NULL)
		goto error;

	switch (fileid) {
	case SIM_EFSPN_FILEID:
		done = isi_read_spn(sim, cbd);
		break;

	case SIM_EF_ICCID_FILEID:
		done = isi_read_iccid(sim, cbd);
		break;

	default:
		done = FALSE;
	}

	if (done)
		return;

	DBG("Fileid %04X not implemented", fileid);

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	g_free(cbd);
}

static void isi_read_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Fileid %04X not implemented", fileid);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void isi_read_file_cyclic(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Fileid %04X not implemented", fileid);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void isi_write_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Fileid %04X not implemented", fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_write_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Fileid %04X not implemented", fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_write_file_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Fileid %04X not implemented", fileid);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void imsi_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sim_imsi_cb_t cb = cbd->cb;

	struct sim_imsi *resp;
	size_t len = sizeof(struct sim_imsi);

	char imsi[SIM_MAX_IMSI_LENGTH + 1];
	size_t i, j;

	if (!check_response_status(msg, SIM_IMSI_RESP_READ_IMSI, READ_IMSI))
		goto error;

	if (!g_isi_msg_data_get_struct(msg, 2, (const void **) &resp, len))
		goto error;

	/* Ignore the low-order semi-octet of the first byte */
	imsi[0] = ((resp->imsi[0] & 0xF0) >> 4) + '0';

	for (i = 1, j = 1; i < resp->length && j < SIM_MAX_IMSI_LENGTH; i++) {
		char nibble;

		imsi[j++] = (resp->imsi[i] & 0x0F) + '0';
		nibble = (resp->imsi[i] & 0xF0) >> 4;
		if (nibble != 0x0F)
			imsi[j++] = nibble + '0';
	}

	imsi[j] = '\0';
	CALLBACK_WITH_SUCCESS(cb, imsi, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void isi_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);

	const uint8_t msg[] = {
		SIM_IMSI_REQ_READ_IMSI,
		READ_IMSI
	};
	size_t len = sizeof(msg);

	if (cbd == NULL || sd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, len, imsi_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
}

static void isi_sim_register(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	if (sd && !sd->registered) {
		sd->registered = TRUE;
		ofono_sim_register(sim);
		ofono_sim_inserted_notify(sim, TRUE);
	}
}

static void read_hplmn_resp_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;

	if (!check_response_status(msg, SIM_NETWORK_INFO_RESP, READ_HPLMN))
		return;

	isi_sim_register(sim);
}


static void isi_read_hplmn(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	const uint8_t req[] = {
		SIM_NETWORK_INFO_REQ,
		READ_HPLMN, 0
	};
	size_t len = sizeof(req);

	if (sd == NULL)
		return;

	g_isi_client_send(sd->client, req, len, read_hplmn_resp_cb, sim, NULL);
}

static void sim_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct sim_data *sd = ofono_sim_get_data(sim);
	uint8_t status;

	if (sd == NULL || g_isi_msg_id(msg) != SIM_IND || sd->registered)
		return;

	if (!g_isi_msg_data_get_byte(msg, 0, &status))
		return;

	switch (status) {
	case SIM_ST_PIN:
		isi_sim_register(sim);
		break;

	case SIM_ST_INFO:
		isi_read_hplmn(sim);
		break;
	}
}

static void sim_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;

	if (g_isi_msg_error(msg) < 0)
		return;

	ISI_VERSION_DBG(msg);

	/* Check if SIM is ready by reading HPLMN */
	isi_read_hplmn(sim);
}

static int isi_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct sim_data *sd;

	sd = g_try_new0(struct sim_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->client = g_isi_client_create(modem, PN_SIM);
	if (sd->client == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	ofono_sim_set_data(sim, sd);

	g_isi_client_ind_subscribe(sd->client, SIM_IND, sim_ind_cb, sim);
	g_isi_client_verify(sd->client, sim_reachable_cb, sim, NULL);

	return 0;
}

static void isi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	if (data == NULL)
		return;

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

void isi_sim_init(void)
{
	ofono_sim_driver_register(&driver);
}

void isi_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
