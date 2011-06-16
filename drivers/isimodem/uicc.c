/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) ST-Ericsson SA 2011.
 *  Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
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
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "simutil.h"
#include "isimodem.h"
#include "isiutil.h"
#include "sim.h"
#include "uicc.h"
#include "debug.h"

#define USIM_APP_DEDICATED_FILE		0x7FFF
#define MAX_SIM_APPS			8

enum uicc_flag {
	UICC_FLAG_APP_STARTED =		1 << 0,
	UICC_FLAG_PIN_STATE_RECEIVED =	1 << 1,
	UICC_FLAG_PASSWD_REQUIRED =	1 << 2,
};

struct sim_data {
	GIsiClient *client;
	unsigned flags;
	int app_id;
	int app_type;
	uint8_t client_id;
};

static GHashTable *g_modems;

struct file_info {
	int fileid;
	int length;
	int structure;
	int record_length;
	uint8_t access[3];
	uint8_t file_status;
};

static const struct file_info static_file_info[] = {
	{ SIM_EFSPN_FILEID, 17, 0, 0, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EF_ICCID_FILEID, 10, 0, 10, { 0x0f, 0xff, 0xee }, 1 },
	{ SIM_EFPL_FILEID, 1, 0, 1, { 0x0f, 0xff, 0xff }, 1 },
	{ SIM_EFLI_FILEID, 1, 0, 1, { 0x0f, 0xff, 0xff }, 1 },
	{ SIM_EFMSISDN_FILEID, 28, 1, 28, { 0x01, 0xff, 0xee }, 1 },
	{ SIM_EFAD_FILEID, 20, 0, 20, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EFPHASE_FILEID, 1, 0, 1, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EFPNN_FILEID, 4 * 18, 1, 18, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EFOPL_FILEID, 4 * 24, 1, 24, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EFMBI_FILEID, 5, 1, 5, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EFMWIS_FILEID, 6, 1, 6, { 0x01, 0xff, 0xee }, 1 },
	{ SIM_EFSPDI_FILEID, 64, 0, 64, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EFECC_FILEID, 5 * 3, 0, 3, { 0x0e, 0xff, 0xee }, 1 },
	{ SIM_EFCBMIR_FILEID, 8 * 4, 0, 4, { 0x01, 0xff, 0xee }, 1 },
	{ SIM_EFCBMI_FILEID, 8 * 2, 0, 2, { 0x01, 0xff, 0xee }, 1 },
	{ SIM_EFCBMID_FILEID, 8 * 2, 0, 2, { 0x01, 0xff, 0x11 }, 1 },
	{ SIM_EFSMSP_FILEID, 56, 1, 56, { 0x01, 0xff, 0xee }, 1 },
	{ SIM_EFIMSI_FILEID, 9, 0, 9, { 0x0e, 0xff, 0xee }, 1 },
};

static gboolean check_resp(const GIsiMessage *msg, uint8_t msgid, uint8_t service)
{
	uint8_t type;
	uint8_t cause;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			sim_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 1, &cause) ||
			cause != UICC_STATUS_OK) {
		DBG("Request failed: %s", uicc_status_name(cause));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &type) || type != service) {
		DBG("Unexpected service: 0x%02X (0x%02X)", type, service);
		return FALSE;
	}
	return TRUE;
}

static void uicc_read_file_info(struct ofono_sim *sim, int fileid,
				ofono_sim_file_info_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, 0, data);
}

static void uicc_read_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void uicc_read_file_linear(struct ofono_sim *sim, int fileid, int record,
					int rec_length, ofono_sim_read_cb_t cb,
					void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void uicc_read_file_cyclic(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void uicc_write_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_write_file_linear(struct ofono_sim *sim, int fileid, int record,
					int length, const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_write_file_cyclic(struct ofono_sim *sim, int fileid, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void uicc_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void uicc_send_passwd(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_query_pin_retries(struct ofono_sim *sim,
					ofono_sim_pin_retries_cb_t cb,
					void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void uicc_reset_passwd(struct ofono_sim *sim, const char *puk,
				const char *passwd, ofono_sim_lock_unlock_cb_t cb,
				void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old, const char *new,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_lock(struct ofono_sim *sim, enum ofono_sim_password_type type,
			int enable, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_query_locked(struct ofono_sim *sim,
				enum ofono_sim_password_type type,
				ofono_sim_locked_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void pin_ind_cb(const GIsiMessage *msg, void *data)
{
	DBG("%s", uicc_message_id_name(g_isi_msg_id(msg)));
}

static void card_ind_cb(const GIsiMessage *msg, void *data)
{
	DBG("%s", uicc_message_id_name(g_isi_msg_id(msg)));
}

static void uicc_ind_cb(const GIsiMessage *msg, void *data)
{
	DBG("%s", uicc_message_id_name(g_isi_msg_id(msg)));
}

static void app_ind_cb(const GIsiMessage *msg, void *data)
{
	DBG("%s", uicc_message_id_name(g_isi_msg_id(msg)));
}

static gboolean decode_data_object(uint8_t *buf, size_t len)
{
	DBG("template=0x%02X length=%u bytes AID=0x%02X",
		buf[0], buf[1], buf[2]);

	return TRUE;
}

struct data_object {
	uint16_t filler;
	uint8_t type;
	uint8_t id;
	uint8_t status;
	uint8_t len;
};

static void uicc_app_list_resp(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct sim_data *sd = ofono_sim_get_data(sim);
	GIsiSubBlockIter iter;
	uint8_t sb;
	struct data_object *app;
	size_t len = sizeof(struct data_object);
	uint8_t *obj;

	if (!check_resp(msg, UICC_APPLICATION_RESP, UICC_APPL_LIST))
		goto fail;

	if (!g_isi_msg_data_get_byte(msg, 5, &sb))
		goto fail;

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != UICC_SB_APPL_DATA_OBJECT)
			continue;

		if (!g_isi_sb_iter_get_struct(&iter, (void **) &app, len, 4))
			goto fail;

		if (!g_isi_sb_iter_get_struct(&iter, (void **) &obj, app->len,
				4 + len))
			goto fail;

		DBG("type=0x%02X id=0x%02X status=0x%02X length=%u bytes",
			app->type, app->id, app->status, app->len);

		decode_data_object(obj, app->len);
	}

	ofono_sim_register(sim);
	ofono_sim_inserted_notify(sim, TRUE);
	return;

fail:
	DBG("Decoding application list failed");

	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static void uicc_status_resp(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	const uint8_t req[] = {
		UICC_APPLICATION_REQ,
		UICC_APPL_LIST,
		0,	/* Number of subblocks */
	};

	if (!check_resp(msg, UICC_RESP, UICC_STATUS_GET))
		goto fail;

	DBG("");

	if (g_isi_msg_error(msg) < 0)
		goto fail;

	if (g_isi_client_send(sd->client, req, sizeof(req), uicc_app_list_resp,
				data, NULL))
		return;

fail:
	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static void uicc_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	const uint8_t req[] = {
		UICC_REQ, UICC_STATUS_GET,
	};

	ISI_RESOURCE_DBG(msg);

	if (g_isi_msg_error(msg) < 0)
		goto fail;

	if (g_isi_client_send(sd->client, req, sizeof(req), uicc_status_resp,
				data, NULL))
		return;

fail:
	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static int uicc_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct sim_data *sd;

	sd = g_try_new0(struct sim_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->client = g_isi_client_create(modem, PN_UICC);
	if (sd->client == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	g_hash_table_insert(g_modems, g_isi_client_modem(sd->client), sim);

	ofono_sim_set_data(sim, sd);

	g_isi_client_ind_subscribe(sd->client, UICC_IND, uicc_ind_cb, sim);
	g_isi_client_ind_subscribe(sd->client, UICC_CARD_IND, card_ind_cb,
					sim);
	g_isi_client_ind_subscribe(sd->client, UICC_CARD_READER_IND,
					card_ind_cb, sim);
	g_isi_client_ind_subscribe(sd->client, UICC_PIN_IND, pin_ind_cb, sim);
	g_isi_client_ind_subscribe(sd->client, UICC_APPLICATION_IND,
					app_ind_cb, sim);

	g_isi_client_verify(sd->client, uicc_reachable_cb, sim, NULL);

	return 0;
}

static void uicc_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	if (data == NULL)
		return;

	g_hash_table_remove(g_modems, g_isi_client_modem(data->client));

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_sim_driver driver = {
	.name			= "wgmodem2.5",
	.probe			= uicc_sim_probe,
	.remove			= uicc_sim_remove,
	.read_file_info		= uicc_read_file_info,
	.read_file_transparent	= uicc_read_file_transparent,
	.read_file_linear	= uicc_read_file_linear,
	.read_file_cyclic	= uicc_read_file_cyclic,
	.write_file_transparent	= uicc_write_file_transparent,
	.write_file_linear	= uicc_write_file_linear,
	.write_file_cyclic	= uicc_write_file_cyclic,
	.read_imsi		= uicc_read_imsi,
	.query_passwd_state	= uicc_query_passwd_state,
	.send_passwd		= uicc_send_passwd,
	.query_pin_retries	= uicc_query_pin_retries,
	.reset_passwd		= uicc_reset_passwd,
	.change_passwd		= uicc_change_passwd,
	.lock			= uicc_lock,
	.query_locked		= uicc_query_locked,
};

void isi_uicc_init(void)
{
	g_modems = g_hash_table_new(g_direct_hash, g_direct_equal);
	ofono_sim_driver_register(&driver);
}

void isi_uicc_exit(void)
{
	g_hash_table_destroy(g_modems);
	ofono_sim_driver_unregister(&driver);
}

gboolean isi_uicc_properties(GIsiModem *modem, int *app_id, int *app_type,
				int *client_id)
{
	struct ofono_sim *sim;
	struct sim_data *sd;

	sim = g_hash_table_lookup(g_modems, modem);
	if (sim == NULL)
		return FALSE;

	sd = ofono_sim_get_data(sim);
	if (sd == NULL)
		return FALSE;

	if (app_id != NULL)
		*app_id = sd->app_id;

	if (app_type != NULL)
		*app_type = sd->app_type;

	if (client_id != NULL)
		*client_id = sd->client_id;

	return TRUE;
}
