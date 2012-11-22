/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  ST-Ericsson AB.
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
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
#include "uicc-util.h"
#include "debug.h"

/* File info parameters */
#define FCP_TEMPLATE			0x62
#define FCP_FILE_SIZE			0x80
#define FCP_FILE_DESC			0x82
#define FCP_FILE_ID			0x83
#define FCP_FILE_LIFECYCLE		0x8A
#define FCP_FILE_SECURITY_ARR		0x8B
#define FCP_FILE_SECURITY_COMPACT	0x8C
#define FCP_FILE_SECURITY_EXPANDED	0xAB
#define FCP_PIN_STATUS			0xC6
#define SIM_EFARR_FILEID		0x6f06
#define MAX_SIM_APPS			10
#define MAX_IMSI_LENGTH			15

enum uicc_flag {
	UICC_FLAG_APP_STARTED =		1 << 0,
	UICC_FLAG_PIN_STATE_RECEIVED =	1 << 1,
	UICC_FLAG_PASSWD_REQUIRED =	1 << 2,
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

struct uicc_file_info_cb_data {
	void *cb;
	void *data;
	void *user;
	struct ofono_sim *sim;
};

static gboolean decode_uicc_usim_type(GIsiSubBlockIter *iter, uint16_t *length,
					uint16_t *file_id,
					uint16_t *record_length,
					uint8_t *records, uint8_t *structure)
{
	uint8_t fcp = 0;
	uint8_t desc = 0;
	uint8_t coding = 0;
	uint8_t fcp_len = 0;
	uint8_t read = 0;
	uint8_t item_len = 0;

	if (!g_isi_sb_iter_get_byte(iter, &fcp, 8))
		return FALSE;

	if (fcp != FCP_TEMPLATE)
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &fcp_len, 9))
		return FALSE;

	for (read = 0; read < fcp_len; read += item_len + 2) {

		uint8_t id;

		if (!g_isi_sb_iter_get_byte(iter, &id, read + 10))
			return FALSE;

		if (!g_isi_sb_iter_get_byte(iter, &item_len, read + 11))
			return FALSE;

		switch (id) {
		case FCP_FILE_SIZE:

			if (item_len != 2)
				return FALSE;

			if (!g_isi_sb_iter_get_word(iter, length, read + 10 + 2))
				return FALSE;

			break;

		case FCP_FILE_ID:

			if (item_len != 2)
				return FALSE;

			if (!g_isi_sb_iter_get_word(iter, file_id, read + 10 + 2))
				return FALSE;

			break;

		case FCP_FILE_DESC:

			if (item_len < 2)
				return FALSE;

			if (!g_isi_sb_iter_get_byte(iter, &desc, read + 10 + 2))
				return FALSE;

			if (!g_isi_sb_iter_get_byte(iter, &coding, read + 10 + 3))
				return FALSE;

			if (item_len < 4)
				break;

			if (!g_isi_sb_iter_get_word(iter, record_length,
							read + 10 + 4))
				return FALSE;

			if (!g_isi_sb_iter_get_byte(iter, records, read + 10 + 6))
				return FALSE;

			break;

		/*
		 * Not implemented, using static access rules
		 * as these are used only for cacheing See
		 * ETSI TS 102 221, ch 11.1.1.4.7 and Annexes
		 * E, F and G.
		 */
		case FCP_FILE_SECURITY_ARR:
		case FCP_FILE_SECURITY_COMPACT:
		case FCP_FILE_SECURITY_EXPANDED:
		case FCP_FILE_LIFECYCLE:
		default:
			DBG("FCP id %02X not supported", id);
			break;
		}
	}

	if ((desc & 7) == 1)
		*structure = OFONO_SIM_FILE_STRUCTURE_TRANSPARENT;
	else if ((desc & 7) == 2)
		*structure = OFONO_SIM_FILE_STRUCTURE_FIXED;
	else if ((desc & 7) == 6)
		*structure = OFONO_SIM_FILE_STRUCTURE_CYCLIC;

	return TRUE;
}

static void uicc_file_info_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct uicc_file_info_cb_data *cbd = opaque;
	struct uicc_sim_data *sd = ofono_sim_get_data(cbd->sim);
	struct file_info const *info = cbd->user;
	ofono_sim_file_info_cb_t cb = cbd->cb;

	GIsiSubBlockIter iter;

	uint16_t length = 0;
	uint16_t record_length = 0;
	uint8_t structure = 0xFF;
	uint8_t records = 0;
	uint16_t file_id = 0;
	uint8_t access[3] = {0, 0, 0};
	uint8_t item_len = 0;

	uint8_t message_id = 0;
	uint8_t service_type = 0;
	uint8_t status = 0;
	uint8_t details = 0;
	uint8_t num_subblocks = 0;
	uint8_t file_status = 1;

	message_id = g_isi_msg_id(msg);

	DBG("uicc_file_info_resp_cb: msg_id=%d, msg len=%zu", message_id,
		g_isi_msg_data_len(msg));

	if (message_id != UICC_APPL_CMD_RESP)
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 0, &service_type) ||
			!g_isi_msg_data_get_byte(msg, 1, &status) ||
			!g_isi_msg_data_get_byte(msg, 2, &details) ||
			!g_isi_msg_data_get_byte(msg, 5, &num_subblocks))
		goto error;

	DBG("%s, service %s, status %s, details %s, nm_sb %d",
		uicc_message_id_name(message_id),
		uicc_service_type_name(service_type),
		uicc_status_name(status), uicc_details_name(details),
		num_subblocks);

	if (info) {
		access[0] = info->access[0];
		access[1] = info->access[1];
		access[2] = info->access[2];
		file_status = info->file_status;
	}

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_subblocks);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		uint8_t sb_id = g_isi_sb_iter_get_id(&iter);

		DBG("Subblock %s", uicc_subblock_name(sb_id));

		if (sb_id != UICC_SB_FCI)
			continue;

		DBG("Decoding UICC_SB_FCI");

		switch (sd->app_type) {
		case UICC_APPL_TYPE_UICC_USIM:
			DBG("UICC_APPL_TYPE_UICC_USIM");

			if (!decode_uicc_usim_type(&iter, &length, &file_id,
							&record_length,
							&records,
							&structure))
				goto error;

			break;

		case UICC_APPL_TYPE_ICC_SIM:
			DBG("UICC_APPL_TYPE_ICC_SIM");

			if (!g_isi_sb_iter_get_word(&iter, &length, 10))
				goto error;

			if (!g_isi_sb_iter_get_word(&iter, &file_id, 12))
				goto error;

			if (!g_isi_sb_iter_get_byte(&iter, &access[0], 16))
				goto error;

			if (!g_isi_sb_iter_get_byte(&iter, &access[0], 17))
				goto error;

			if (!g_isi_sb_iter_get_byte(&iter, &access[0], 18))
				goto error;

			if (!g_isi_sb_iter_get_byte(&iter, &item_len, 20))
				goto error;

			if (!g_isi_sb_iter_get_byte(&iter, &structure, 21))
				goto error;

			if (item_len == 2) {
				uint8_t byte;

				if (!g_isi_sb_iter_get_byte(&iter, &byte, 22))
					goto error;

				record_length = byte;
			}
			break;

		default:
			DBG("Application type %d not supported", sd->app_type);
			break;
		}

		DBG("fileid=%04X, filelen=%d, records=%d, reclen=%d, structure=%d",
			file_id, length, records, record_length, structure);

		CALLBACK_WITH_SUCCESS(cb, length, structure, record_length,
					access, file_status, cbd->data);
		return;
	}

error:
	DBG("Error reading file info");
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, 0, cbd->data);
}

static gboolean send_uicc_read_file_info(GIsiClient *client, uint8_t app_id,
						int fileid, uint8_t df_len,
						int mf_path, int df1_path,
						int df2_path,
						GIsiNotifyFunc notify, void *data,
						GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		UICC_APPL_CMD_REQ,
		UICC_APPL_FILE_INFO,	/* Service type */
		app_id,
		UICC_SESSION_ID_NOT_USED,
		0, 0,			/* Filler */
		1,			/* Number of subblocks */
		ISI_16BIT(UICC_SB_APPL_PATH),
		ISI_16BIT(16),		/* Subblock length */
		ISI_16BIT(fileid),
		uicc_get_sfi(fileid),	/* Elementary file short file id */
		0,			/* Filler */
		df_len,
		0,			/* Filler */
		ISI_16BIT(mf_path),
		ISI_16BIT(df1_path),
		ISI_16BIT(df2_path),
	};

	return g_isi_client_send(client, msg, sizeof(msg), notify, data, destroy);
}

static void uicc_read_file_info(struct ofono_sim *sim, int fileid,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb, void *data)
{
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	struct uicc_file_info_cb_data *cbd;

	/* Prepare for static file info used for access rights */
	int i;
	int N = sizeof(static_file_info) / sizeof(static_file_info[0]);
	int mf_path = 0;
	int df1_path = 0;
	int df2_path = 0;
	uint8_t df_len = 0;

	cbd = g_try_new0(struct uicc_file_info_cb_data, 1);
	if (!cbd)
		goto error;

	cbd->cb = cb;
	cbd->data = data;
	cbd->sim = sim;
	cbd->user = NULL;

	DBG("File info for ID=%04X app id %d", fileid, sd->app_id);

	for (i = 0; i < N; i++) {
		if (fileid == static_file_info[i].fileid) {
			cbd->user = (void *) &static_file_info[i];
			break;
		}
	}

	DBG("File info for ID=%04X: %p", fileid, cbd->user);

	if (!uicc_get_fileid_path(sd, &mf_path, &df1_path, &df2_path,
					&df_len, fileid))
		goto error;

	if (send_uicc_read_file_info(sd->client, sd->app_id, fileid, df_len,
					mf_path, df1_path, df2_path,
					uicc_file_info_resp_cb,
					cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, 0, data);
	g_free(cbd);
}

static void uicc_read_file_transp_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_sim_read_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;

	uint32_t filelen = 0;
	uint8_t *filedata = NULL;
	uint8_t num_sb = 0;

	DBG("");

	if (!check_resp(msg, UICC_APPL_CMD_RESP, UICC_APPL_READ_TRANSPARENT))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 5, &num_sb))
		goto error;

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		int sb_id = g_isi_sb_iter_get_id(&iter);

		DBG("Subblock %s", uicc_subblock_name(sb_id));

		if (sb_id != UICC_SB_FILE_DATA)
			continue;

		if (!g_isi_sb_iter_get_dword(&iter, &filelen, 4))
			goto error;

		if (!g_isi_sb_iter_get_struct(&iter, (void **) &filedata,
						filelen, 8))
			goto error;

		DBG("Transparent EF read: 1st byte %02x, len %d",
			filedata[0], filelen);
		CALLBACK_WITH_SUCCESS(cb, filedata, filelen, cbd->data);
		return;
	}

error:
	DBG("Error reading transparent EF");
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static gboolean send_uicc_read_file_transparent(GIsiClient *client,
						uint8_t app_id, uint8_t client_id,
						int fileid, uint8_t df_len,
						int mf_path, int df1_path,
						int df2_path,
						GIsiNotifyFunc notify,
						void *data,
						GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		UICC_APPL_CMD_REQ,
		UICC_APPL_READ_TRANSPARENT,
		app_id,
		UICC_SESSION_ID_NOT_USED,
		0, 0,		/* Filler */
		3,		/* Number of subblocks */
		ISI_16BIT(UICC_SB_CLIENT),
		ISI_16BIT(8),	/* Subblock length*/
		0, 0, 0,	/* Filler */
		client_id,
		ISI_16BIT(UICC_SB_TRANSPARENT),
		ISI_16BIT(8),	/* Subblock length */
		ISI_16BIT(0),	/* File offset */
		ISI_16BIT(0),	/* Data amount (0=all) */
		ISI_16BIT(UICC_SB_APPL_PATH),
		ISI_16BIT(16),	/* Subblock length */
		ISI_16BIT(fileid),
		uicc_get_sfi(fileid),	/* Elementary file short file id */
		0,	/* Filler */
		df_len,
		0,
		ISI_16BIT(mf_path),
		ISI_16BIT(df1_path),
		ISI_16BIT(df2_path),
	};

	return g_isi_client_send(client, msg, sizeof(msg), notify, data, destroy);
}

static void uicc_read_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_read_cb_t cb, void *data)
{
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);
	int mf_path = 0;
	int df1_path = 0;
	int df2_path = 0;
	uint8_t df_len = 0;

	if (!cbd || !sd)
		goto error;

	DBG("File ID=%04X, client %d, AID %d", fileid, sd->client_id,
		sd->app_id);

	if (!uicc_get_fileid_path(sd, &mf_path, &df1_path,
					&df2_path, &df_len, fileid))
		goto error;

	if (send_uicc_read_file_transparent(sd->client, sd->app_id, sd->client_id,
						fileid, df_len, mf_path,
						df1_path, df2_path,
						uicc_read_file_transp_resp_cb,
						cbd, g_free))
		return;

error:
	DBG("Read file transparent failed");
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	g_free(cbd);
}

static void read_file_linear_resp(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_sim_read_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;
	uint8_t num_sb = 0;
	uint8_t *filedata = NULL;
	uint32_t filelen = 0;

	DBG("");

	if (!check_resp(msg, UICC_APPL_CMD_RESP, UICC_APPL_READ_LINEAR_FIXED))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 5, &num_sb))
		goto error;

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		uint8_t sb_id = g_isi_sb_iter_get_id(&iter);

		DBG("Subblock %s", uicc_subblock_name(sb_id));

		if (sb_id != UICC_SB_FILE_DATA)
			continue;

		if (!g_isi_sb_iter_get_dword(&iter, &filelen, 4))
			goto error;

		if (!g_isi_sb_iter_get_struct(&iter, (void **) &filedata,
						filelen, 8))
			goto error;

		DBG("Linear fixed EF read: 1st byte %02x, len %d", filedata[0],
			filelen);

		CALLBACK_WITH_SUCCESS(cb, filedata, filelen, cbd->data);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static gboolean send_uicc_read_file_linear(GIsiClient *client, uint8_t app_id,
						uint8_t client_id,
						int fileid, int record,
						int rec_length,
						unsigned char df_len,
						int mf_path, int df1_path,
						int df2_path,
						GIsiNotifyFunc notify,
						void *data,
						GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		UICC_APPL_CMD_REQ,
		UICC_APPL_READ_LINEAR_FIXED,
		app_id,
		UICC_SESSION_ID_NOT_USED,
		0, 0,		/* Filler */
		3,		/* Number of subblocks */
		ISI_16BIT(UICC_SB_CLIENT),
		ISI_16BIT(8),	/*Subblock length */
		0, 0, 0,	/* Filler */
		client_id,
		ISI_16BIT(UICC_SB_LINEAR_FIXED),
		ISI_16BIT(8),	/*Subblock length */
		record,
		0,		/* Record offset */
		rec_length & 0xff,	/*Data amount (0=all)*/
		0,
		ISI_16BIT(UICC_SB_APPL_PATH),
		ISI_16BIT(16),	/* Subblock length */
		ISI_16BIT(fileid),
		uicc_get_sfi(fileid),	/* Elementary file short file id */
		0,		/* Filler */
		df_len,
		0,
		ISI_16BIT(mf_path),
		ISI_16BIT(df1_path),
		ISI_16BIT(df2_path),
	};

	return g_isi_client_send(client, msg, sizeof(msg), notify, data, destroy);
}

static void uicc_read_file_linear(struct ofono_sim *sim, int fileid, int record,
					int rec_length,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_read_cb_t cb,	void *data)
{
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);
	int mf_path = 0;
	int df1_path = 0;
	int df2_path = 0;
	uint8_t df_len = 0;

	if (!sd || !cbd)
		goto error;

	DBG("File ID=%04X, record %d, client %d AID %d", fileid, record,
		sd->client_id, sd->app_id);

	if (!uicc_get_fileid_path(sd, &mf_path, &df1_path, &df2_path,
					&df_len, fileid))
		goto error;

	if (send_uicc_read_file_linear(sd->client, sd->app_id, sd->client_id,
					fileid, record, rec_length, df_len,
					mf_path, df1_path, df2_path,
					read_file_linear_resp, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	g_free(cbd);
}

static void uicc_read_file_cyclic(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_read_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void uicc_write_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_write_file_linear(struct ofono_sim *sim, int fileid, int record,
					int length, const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_write_file_cyclic(struct ofono_sim *sim, int fileid,
					int length,
					const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean decode_imsi(uint8_t *data, int len, char *imsi)
{
	int i = 1; /* Skip first byte, the length field */
	int j = 0;

	if (data == NULL || len == 0)
		return FALSE;

	if (data[0] != 8 || data[0] > len)
		return FALSE;

	/* Ignore low-order semi-octet of the first byte */
	imsi[j] = ((data[i] & 0xF0) >> 4) + '0';

	for (i++, j++; i - 1 < data[0] && j < MAX_IMSI_LENGTH; i++) {
		char nibble;

		imsi[j++] = (data[i] & 0x0F) + '0';
		nibble = (data[i] & 0xF0) >> 4;

		if (nibble != 0x0F)
			imsi[j++] = nibble + '0';
	}

	imsi[j] = '\0';
	return TRUE;
}

static void uicc_read_imsi_resp(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;

	uint32_t filelen = 0;
	uint8_t *filedata = NULL;
	uint8_t num_sb = 0;

	char imsi[MAX_IMSI_LENGTH + 1] = { 0 };

	DBG("");

	if (!check_resp(msg, UICC_APPL_CMD_RESP, UICC_APPL_READ_TRANSPARENT))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 5, &num_sb))
		goto error;

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		int sb_id = g_isi_sb_iter_get_id(&iter);

		DBG("Subblock %s", uicc_subblock_name(sb_id));

		if (sb_id != UICC_SB_FILE_DATA)
			continue;

		if (!g_isi_sb_iter_get_dword(&iter, &filelen, 4))
			goto error;

		if (!g_isi_sb_iter_get_struct(&iter, (void **) &filedata,
						filelen, 8))
			goto error;

		DBG("Transparent EF read: 1st byte %02x, len %d",
			filedata[0], filelen);

		if (!decode_imsi(filedata, filelen, imsi))
			goto error;

		DBG("IMSI %s", imsi);
		CALLBACK_WITH_SUCCESS(cb, imsi, cbd->data);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void uicc_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
				void *data)
{
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);

	int mf_path = 0;
	int df1_path = 0;
	int df2_path = 0;
	uint8_t df_len = 0;

	if (!cbd)
		goto error;

	DBG("Client %d, AID %d", sd->client_id, sd->app_id);

	if (!uicc_get_fileid_path(sd, &mf_path, &df1_path, &df2_path, &df_len,
					SIM_EFIMSI_FILEID))
		goto error;

	if (send_uicc_read_file_transparent(sd->client, sd->app_id, sd->client_id,
						SIM_EFIMSI_FILEID, df_len,
						mf_path, df1_path, df2_path,
						uicc_read_imsi_resp,
						cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
}

static void uicc_query_passwd_state_resp(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	uint8_t type;
	uint8_t cause;

	DBG("");

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		goto error;
	}

	if (g_isi_msg_id(msg) != UICC_PIN_RESP) {
		DBG("Unexpected msg: %s", sim_message_id_name(g_isi_msg_id(msg)));
		goto error;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &type) ||
			type != UICC_PIN_PROMPT_VERIFY) {
		DBG("Unexpected service: 0x%02X (0x%02X)", type,
			UICC_PIN_PROMPT_VERIFY);
		goto error;
	}

	if (!g_isi_msg_data_get_byte(msg, 1, &cause))
		goto error;

	DBG("Status: %d %s", cause, uicc_status_name(cause));

	if (cause == UICC_STATUS_PIN_DISABLED) {
		CALLBACK_WITH_SUCCESS(cb, OFONO_SIM_PASSWORD_NONE, cbd->data);
		return;
	}

	DBG("Request failed or not implemented: %s", uicc_status_name(cause));

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void uicc_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);

	const uint8_t req[] = {
		UICC_PIN_REQ,
		UICC_PIN_PROMPT_VERIFY,
		sd->app_id,
		0, 0, 0,	/* Filler */
		1,		/* Number of subblocks */
		ISI_16BIT(UICC_SB_PIN_REF),
		ISI_16BIT(8),	/*Sub block length*/
		sd->pin1_id,	/* Pin ID */
		0, 0, 0,	/* Filler */
	};

	DBG("");

	if (g_isi_client_send(sd->client, req, sizeof(req),
				uicc_query_passwd_state_resp, cbd, g_free))
		return;

	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static void uicc_send_passwd(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
}

static void uicc_query_pin_retries_resp(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	GIsiSubBlockIter iter;

	uint8_t num_sb = 0;
	uint8_t pins = 0;
	uint8_t pina = 0;
	uint8_t puka = 0;

	DBG("");

	if (!check_resp(msg, UICC_PIN_RESP, UICC_PIN_INFO))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 5, &num_sb))
		goto error;

	DBG("Subblock count %d", num_sb);

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		uint8_t sb_id = g_isi_sb_iter_get_id(&iter);

		DBG("Sub-block %s", uicc_subblock_name(sb_id));

		if (sb_id != UICC_SB_PIN_INFO)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &pins, 4))
			goto error;

		if (!g_isi_sb_iter_get_byte(&iter, &pina, 5))
			goto error;

		if (!g_isi_sb_iter_get_byte(&iter, &puka, 6))
			goto error;

		DBG("PIN status %X PIN Attrib %d PUK attrib %d", pins,
			pina, puka);

		retries[OFONO_SIM_PASSWORD_SIM_PIN] = pina;
		retries[OFONO_SIM_PASSWORD_SIM_PUK] = puka;

		CALLBACK_WITH_SUCCESS(cb, retries, cbd->data);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void uicc_query_pin_retries(struct ofono_sim *sim,
					ofono_sim_pin_retries_cb_t cb,
					void *data)
{
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);

	const uint8_t req[] = {
		UICC_PIN_REQ,
		UICC_PIN_INFO,
		sd->app_id,
		0, 0, 0,	/* Filler */
		1,		/* Number of subblocks */
		ISI_16BIT(UICC_SB_PIN_REF),
		ISI_16BIT(8),	/* Subblock length */
		sd->pin1_id,	/* Pin ID */
		0, 0, 0,	/* Filler */
	};

	DBG("");

	if (g_isi_client_send(sd->client, req, sizeof(req),
				uicc_query_pin_retries_resp, cbd, g_free))
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
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

static gboolean decode_fcp_pin_status(const GIsiSubBlockIter *iter, uint8_t read,
					uint8_t *pin1, uint8_t *pin2)
{
	uint8_t do_len;
	uint8_t len;
	uint8_t tag;
	uint8_t id;
	uint8_t tag_pos;

	DBG("Decoding PIN status");

	if (!g_isi_sb_iter_get_byte(iter, &do_len, read))
		return FALSE;

	tag_pos = read + 1 + do_len;

	if (!g_isi_sb_iter_get_byte(iter, &tag, tag_pos))
		return FALSE;

	while (tag == 0x83) {

		if (!g_isi_sb_iter_get_byte(iter, &len, tag_pos + 1))
			return FALSE;

		if (!g_isi_sb_iter_get_byte(iter, &id, tag_pos + 2))
			return FALSE;

		tag_pos += 2 + len;

		if (!g_isi_sb_iter_get_byte(iter, &tag, tag_pos))
			return FALSE;

		DBG("PIN_len %d, PIN id %02x, PIN tag %02x", len, id, tag);

		if (id >= 0x01 && id <= 0x08)
			*pin1 = id;
		else if (id >= 0x81 && id <= 0x88)
			*pin2 = id;
	}
	return TRUE;
}

static gboolean decode_fci_sb(const GIsiSubBlockIter *iter, int app_type,
				uint8_t *pin1, uint8_t *pin2)
{
	uint8_t fcp = 0;
	uint8_t fcp_len = 0;
	uint8_t read = 0;
	uint8_t item_len = 0;

	DBG("Decoding UICC_SB_FCI");

	if (app_type != UICC_APPL_TYPE_UICC_USIM)
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &fcp, 8))
		return FALSE;

	if (fcp != FCP_TEMPLATE)
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &fcp_len, 9))
		return FALSE;

	for (read = 0; read < fcp_len; read += item_len + 2) {
		uint8_t id;

		if (!g_isi_sb_iter_get_byte(iter, &id, read + 10))
			return FALSE;

		if (!g_isi_sb_iter_get_byte(iter, &item_len, read + 11))
			return FALSE;

		if (id != FCP_PIN_STATUS)
			continue;

		if (!decode_fcp_pin_status(iter, read + 13, pin1, pin2))
			return FALSE;
	}
	return TRUE;
}

static gboolean decode_chv_sb(const GIsiSubBlockIter *iter, int app_type,
				uint8_t *pin1, uint8_t *pin2)
{
	uint8_t chv_id = 0;
	uint8_t pin_id = 0;

	DBG("Decoding UICC_SB_CHV");

	if (app_type != UICC_APPL_TYPE_ICC_SIM)
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &chv_id, 4))
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &pin_id, 5))
		return FALSE;

	switch (chv_id) {
	case 1:
		*pin1 = pin_id;
		break;

	case 2:
		*pin2 = pin_id;
		break;

	default:
		return FALSE;
	}

	DBG("CHV=%d, pin_id=%2x, PIN1 %02x, PIN2 %02x", chv_id, pin_id, *pin1,
		*pin2);

	return TRUE;
}

static void uicc_application_activate_resp(const GIsiMessage *msg, void *opaque)
{
	struct ofono_sim *sim = opaque;
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	GIsiSubBlockIter iter;
	uint8_t cause, num_sb;

	DBG("");

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		return;
	}

	if (g_isi_msg_id(msg) != UICC_APPLICATION_RESP) {
		DBG("Unexpected msg: %s",
			sim_message_id_name(g_isi_msg_id(msg)));
		return;
	}

	if (!g_isi_msg_data_get_byte(msg, 1, &cause))
		return;

	if (cause != UICC_STATUS_OK && cause != UICC_STATUS_APPL_ACTIVE) {
		DBG("TODO: handle application activation");
		return;
	}

	if (!sd->uicc_app_started) {
		sd->app_id = sd->trying_app_id;
		sd->app_type = sd->trying_app_type;
		sd->uicc_app_started = TRUE;

		DBG("UICC application activated");

		ofono_sim_inserted_notify(sim, TRUE);
		ofono_sim_register(sim);

		g_hash_table_remove_all(sd->app_table);
	}

	if (!g_isi_msg_data_get_byte(msg, 5, &num_sb))
		return;

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		uint8_t sb_id = g_isi_sb_iter_get_id(&iter);

		DBG("Subblock %s", uicc_subblock_name(sb_id));

		switch (sb_id) {
		case UICC_SB_CLIENT:

			if (!g_isi_sb_iter_get_byte(&iter, &sd->client_id, 7))
				return;

			DBG("Client id %d", sd->client_id);
			break;

		case UICC_SB_FCI:

			if (!decode_fci_sb(&iter, sd->app_type, &sd->pin1_id,
						&sd->pin2_id))
				return;

			DBG("PIN1 %02x, PIN2 %02x", sd->pin1_id, sd->pin2_id);
			break;

		case UICC_SB_CHV:

			if (!decode_chv_sb(&iter, sd->app_type, &sd->pin1_id,
						&sd->pin2_id))
				return;

			DBG("PIN1 %02x, PIN2 %02x", sd->pin1_id, sd->pin2_id);
			break;

		default:
			DBG("Skipping sub-block: %s (%zu bytes)",
				uicc_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}
}

static gboolean send_application_activate_req(GIsiClient *client,
						uint8_t app_type,
						uint8_t app_id,
						GIsiNotifyFunc notify,
						void *data,
						GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		UICC_APPLICATION_REQ,
		UICC_APPL_HOST_ACTIVATE,
		2,		/* Number of subblocks */
		ISI_16BIT(UICC_SB_APPLICATION),
		ISI_16BIT(8),	/* Subblock length */
		0, 0,		/* Filler */
		app_type,
		app_id,
		ISI_16BIT(UICC_SB_APPL_INFO),
		ISI_16BIT(8),	/* Subblock length */
		0, 0, 0,	/* Filler */
		/*
		 * Next field indicates whether the application
		 * initialization procedure will follow the activation
		 * or not
		 */
		UICC_APPL_START_UP_INIT_PROC,
	};

	DBG("App type %d, AID %d", app_type, app_id);

	return g_isi_client_send(client, msg, sizeof(msg), notify, data, destroy);
}

static void uicc_application_list_resp(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	GIsiSubBlockIter iter;
	uint8_t num_sb;
	struct uicc_sim_application *sim_app;

	/* Throw away old app table */
	g_hash_table_remove_all(sd->app_table);

	if (!check_resp(msg, UICC_APPLICATION_RESP, UICC_APPL_LIST))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 5, &num_sb))
		goto error;

	/* Iterate through the application list */
	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {
		uint8_t app_type;
		uint8_t app_id;
		uint8_t app_status;
		uint8_t app_len;

		if (g_isi_sb_iter_get_id(&iter) != UICC_SB_APPL_DATA_OBJECT)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &app_type, 6))
			goto error;

		if (!g_isi_sb_iter_get_byte(&iter, &app_id, 7))
			goto error;

		if (!g_isi_sb_iter_get_byte(&iter, &app_status, 8))
			goto error;

		if (!g_isi_sb_iter_get_byte(&iter, &app_len, 9))
			goto error;

		if (app_type != UICC_APPL_TYPE_ICC_SIM &&
				app_type != UICC_APPL_TYPE_UICC_USIM)
			continue;

		sim_app = g_try_new0(struct uicc_sim_application, 1);
		if (!sim_app) {
			DBG("out of memory!");
			goto error;
		}

		sim_app->type = app_type;
		sim_app->id = app_id;
		sim_app->status = app_status;
		sim_app->length = app_len;
		sim_app->sim = sd;

		g_hash_table_replace(sd->app_table, &sim_app->id, sim_app);
	}

	if (!sd->uicc_app_started) {
		GHashTableIter app_iter;
		struct uicc_sim_application *app;

		gpointer key;
		gpointer value;

		g_hash_table_iter_init(&app_iter, sd->app_table);

		if (!g_hash_table_iter_next(&app_iter, &key, &value))
			return;

		app = value;
		sd->trying_app_type = app->type;
		sd->trying_app_id = app->id;

		g_hash_table_remove(sd->app_table, &app->id);

		if (!send_application_activate_req(sd->client, app->type, app->id,
						uicc_application_activate_resp,
						data, NULL)) {
			DBG("Failed to activate: 0x%02X (type=0x%02X)",
				app->id, app->type);
			return;
		}
	}
	return;

error:
	DBG("Decoding application list failed");

	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static void uicc_card_status_resp(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	GIsiSubBlockIter iter;
	uint8_t card_status = 0;
	uint8_t num_sb = 0;

	DBG("");

	if (!sd->server_running)
		return;

	if (!check_resp(msg, UICC_CARD_RESP, UICC_CARD_STATUS_GET))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 1, &card_status))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 5, &num_sb))
		goto error;

	DBG("Subblock count %d", num_sb);

	for (g_isi_sb_iter_init_full(&iter, msg, 6, TRUE, num_sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != UICC_SB_CARD_STATUS)
			continue;

		if (!g_isi_sb_iter_get_byte(&iter, &card_status, 7))
			goto error;

		DBG("card_status = 0x%X", card_status);

		/* Check if card is ready */
		if (card_status == 0x21) {
			const uint8_t req[] = {
				UICC_APPLICATION_REQ,
				UICC_APPL_LIST,
				0,	/* Number of subblocks */
			};

			DBG("card is ready");
			ofono_sim_inserted_notify(sim, TRUE);

			if (g_isi_client_send(sd->client, req, sizeof(req),
						uicc_application_list_resp,
						data, NULL))
				return;

			DBG("Failed to query application list");
			goto error;

		} else {
			DBG("card not ready");
			ofono_sim_inserted_notify(sim, FALSE);
			return;
		}
	}

error:
	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static void uicc_card_status_req(struct ofono_sim *sim,
					struct uicc_sim_data *sd)
{
	const uint8_t req[] = {
		UICC_CARD_REQ,
		UICC_CARD_STATUS_GET,
		0,
	};

	DBG("");

	if (g_isi_client_send(sd->client, req, sizeof(req),
				    uicc_card_status_resp, sim, NULL))
		return;

	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static void uicc_card_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	if (g_isi_msg_id(msg) != UICC_CARD_IND)
		return;

	/* We're not interested in card indications if server isn't running */
	if (!sd->server_running)
		return;

	/* Request card status */
	uicc_card_status_req(sim, sd);
}

static void uicc_status_resp(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	uint8_t status = 0, server_status = 0;
	gboolean server_running = FALSE;

	if (!check_resp(msg, UICC_RESP, UICC_STATUS_GET))
		goto error;

	if (g_isi_msg_error(msg) < 0)
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 1, &status) ||
			!g_isi_msg_data_get_byte(msg, 3, &server_status))
		goto error;

	DBG("status=0x%X, server_status=0x%X", status, server_status);

	if (status == UICC_STATUS_OK &&
			server_status == UICC_STATUS_START_UP_COMPLETED) {
		DBG("server is up!");
		server_running = TRUE;
	}


	if (!server_running) {
		sd->server_running = FALSE;

		/* TODO: Remove SIM etc... */
		return;
	}

	if (sd->server_running && server_running) {
		DBG("Server status didn't change...");
		return;
	}

	/* Server is running */
	sd->server_running = TRUE;

	/* Request card status */
	uicc_card_status_req(sim, sd);
	return;

error:
	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static void uicc_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);
	const uint8_t req[] = { UICC_REQ, UICC_STATUS_GET, 0 };

	int msg_id = g_isi_msg_id(msg);
	DBG("%s", uicc_message_id_name(msg_id));

	if (msg_id != UICC_IND)
		return;

	/* Request status */
	if (g_isi_client_send(sd->client, req, sizeof(req), uicc_status_resp,
				data, NULL))
		return;

	DBG("status request failed!");

	g_isi_client_destroy(sd->client);
	sd->client = NULL;
	ofono_sim_remove(sim);
}

static void uicc_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct uicc_sim_data *sd = ofono_sim_get_data(sim);

	const uint8_t req[] = {
		UICC_REQ,
		UICC_STATUS_GET,
		0,	/* Number of Sub Blocks (only from version 4.0) */
	};

	ISI_RESOURCE_DBG(msg);

	if (g_isi_msg_error(msg) < 0)
		goto error;

	sd->version.major = g_isi_msg_version_major(msg);
	sd->version.minor = g_isi_msg_version_minor(msg);

	/* UICC server is reachable: request indications */
	g_isi_client_ind_subscribe(sd->client, UICC_IND, uicc_ind_cb, sim);
	g_isi_client_ind_subscribe(sd->client, UICC_CARD_IND, uicc_card_ind_cb,
					sim);

	/* Update status */
	if (g_isi_client_send(sd->client, req,
				sizeof(req) - ((sd->version.major < 4) ? 1 : 0),
				uicc_status_resp, data, NULL))
		return;

error:
	g_isi_client_destroy(sd->client);
	sd->client = NULL;

	ofono_sim_remove(sim);
}

static void sim_app_destroy(gpointer p)
{
	struct uicc_sim_application *app = p;
	if (!app)
		return;

	g_free(app);
}

static int uicc_sim_probe(struct ofono_sim *sim, unsigned int vendor,
					void *user)
{
	GIsiModem *modem = user;
	struct uicc_sim_data *sd;

	sd = g_try_new0(struct uicc_sim_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	/* Create hash table for the UICC applications */
	sd->app_table = g_hash_table_new_full(g_int_hash, g_int_equal, NULL,
						sim_app_destroy);
	if (sd->app_table == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	sd->client = g_isi_client_create(modem, PN_UICC);
	if (sd->client == NULL) {
		g_hash_table_destroy(sd->app_table);
		g_free(sd);
		return -ENOMEM;
	}

	g_hash_table_insert(g_modems, g_isi_client_modem(sd->client), sim);

	sd->server_running = FALSE;
	sd->uicc_app_started = FALSE;
	sd->pin_state_received = FALSE;
	sd->passwd_required = TRUE;
	ofono_sim_set_data(sim, sd);

	g_isi_client_verify(sd->client, uicc_reachable_cb, sim, NULL);

	return 0;
}

static void uicc_sim_remove(struct ofono_sim *sim)
{
	struct uicc_sim_data *data = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	if (data == NULL)
		return;

	g_hash_table_remove(g_modems, g_isi_client_modem(data->client));

	g_hash_table_destroy(data->app_table);
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
	struct uicc_sim_data *sd;

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
