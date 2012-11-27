/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
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

#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "qmi.h"
#include "uim.h"

#include "qmimodem.h"
#include "simutil.h"

#define EF_STATUS_INVALIDATED 0
#define EF_STATUS_VALID 1

struct sim_data {
	struct qmi_service *uim;
	uint32_t event_mask;
	uint8_t card_state;
	uint8_t app_type;
	uint8_t passwd_state;
	int retries[OFONO_SIM_PASSWORD_INVALID];
};

static int create_fileid_data(uint8_t app_type, int fileid,
					const unsigned char *path,
					unsigned int path_len,
					unsigned char *fileid_data)
{
	unsigned char db_path[6];
	unsigned int len;

	if (path_len > 0) {
		memcpy(db_path, path, path_len);
		len = path_len;
	} else {
		switch (app_type) {
		case 0x01:	/* SIM card */
			len = sim_ef_db_get_path_2g(fileid, db_path);
			break;
		case 0x02:	/* USIM application */
			len = sim_ef_db_get_path_3g(fileid, db_path);
			break;
		default:
			len = 0;
			break;
		}
	}

	/* Minimum length of path is 2 bytes */
	if (len < 2)
		return -1;

	fileid_data[0] = fileid & 0xff;
	fileid_data[1] = (fileid & 0xff00) >> 8;
	fileid_data[2] = len;
	fileid_data[3] = db_path[1];
	fileid_data[4] = db_path[0];
	fileid_data[5] = db_path[3];
	fileid_data[6] = db_path[2];
	fileid_data[7] = db_path[5];
	fileid_data[8] = db_path[4];

	return len + 3;
}

static void get_file_attributes_cb(struct qmi_result *result, void *user_data)
{
        struct cb_data *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct sim_data *data = ofono_sim_get_data(cbd->user);
	const struct qmi_uim_file_attributes *attr;
	uint16_t len, raw_len;
	int flen, rlen, str;
	unsigned char access[3];
	unsigned char file_status;
	gboolean ok;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	attr = qmi_result_get(result, 0x11, &len);
	if (!attr)
		goto error;

	raw_len = GUINT16_FROM_LE(attr->raw_len);

	switch (data->app_type) {
	case 0x01:	/* SIM card */
		ok = sim_parse_2g_get_response(attr->raw_value, raw_len,
				&flen, &rlen, &str, access, &file_status);
		break;
	case 0x02:	/* USIM application */
		ok = sim_parse_3g_get_response(attr->raw_value, raw_len,
					 &flen, &rlen, &str, access, NULL);
		file_status = EF_STATUS_VALID;
		break;
	default:
		ok = FALSE;
		break;
	}

	if (ok) {
		CALLBACK_WITH_SUCCESS(cb, flen, str, rlen, access,
						file_status, cbd->data);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
					EF_STATUS_INVALIDATED, cbd->data);
}

static void qmi_read_attributes(struct ofono_sim *sim, int fileid,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	unsigned char aid_data[2] = { 0x06, 0x00 };
	unsigned char fileid_data[9];
	int fileid_len;
	struct qmi_param *param;

	DBG("file id 0x%04x path len %d", fileid, path_len);

	cbd->user = sim;

	fileid_len = create_fileid_data(data->app_type, fileid,
						path, path_len, fileid_data);
	if (fileid_len < 0)
		goto error;

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, 0x01, sizeof(aid_data), aid_data);
	qmi_param_append(param, 0x02, fileid_len, fileid_data);

	if (qmi_service_send(data->uim, QMI_UIM_GET_FILE_ATTRIBUTES, param,
				get_file_attributes_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
					EF_STATUS_INVALIDATED, cbd->data);

	g_free(cbd);
}

static void read_generic_cb(struct qmi_result *result, void *user_data)
{
        struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	const unsigned char *content;
	uint16_t len;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	content = qmi_result_get(result, 0x11, &len);
	if (!content) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, content + 2, len - 2, cbd->data);
}

static void qmi_read_transparent(struct ofono_sim *sim,
				int fileid, int start, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	unsigned char aid_data[2] = { 0x06, 0x00 };
	unsigned char read_data[4];
	unsigned char fileid_data[9];
	int fileid_len;
	struct qmi_param *param;

	DBG("file id 0x%04x path len %d", fileid, path_len);

	fileid_len = create_fileid_data(data->app_type, fileid,
						path, path_len, fileid_data);
	if (fileid_len < 0)
		goto error;

	read_data[0] = start & 0xff;
	read_data[1] = (start & 0xff00) >> 8;
	read_data[2] = length & 0xff;
	read_data[3] = (length & 0xff00) >> 8;

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, 0x01, sizeof(aid_data), aid_data);
	qmi_param_append(param, 0x02, fileid_len, fileid_data);
	qmi_param_append(param, 0x03, sizeof(read_data), read_data);

	if (qmi_service_send(data->uim, QMI_UIM_READ_TRANSPARENT, param,
					read_generic_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, user_data);

	g_free(cbd);
}

static void qmi_read_record(struct ofono_sim *sim,
				int fileid, int record, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	unsigned char aid_data[2] = { 0x06, 0x00 };
	unsigned char read_data[4];
	unsigned char fileid_data[9];
	int fileid_len;
	struct qmi_param *param;

	DBG("file id 0x%04x path len %d", fileid, path_len);

	fileid_len = create_fileid_data(data->app_type, fileid,
						path, path_len, fileid_data);
	if (fileid_len < 0)
		goto error;

	read_data[0] = record & 0xff;
	read_data[1] = (record & 0xff00) >> 8;
	read_data[2] = length & 0xff;
	read_data[3] = (length & 0xff00) >> 8;

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, 0x01, sizeof(aid_data), aid_data);
	qmi_param_append(param, 0x02, fileid_len, fileid_data);
	qmi_param_append(param, 0x03, sizeof(read_data), read_data);

	if (qmi_service_send(data->uim, QMI_UIM_READ_RECORD, param,
					read_generic_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, user_data);

	g_free(cbd);
}

static void qmi_query_passwd_state(struct ofono_sim *sim,
				ofono_sim_passwd_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("passwd state %d", data->passwd_state);

	if (data->passwd_state == OFONO_SIM_PASSWORD_INVALID) {
		CALLBACK_WITH_FAILURE(cb, -1, user_data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, data->passwd_state, user_data);
}

static void qmi_query_pin_retries(struct ofono_sim *sim,
				ofono_sim_pin_retries_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("passwd state %d", data->passwd_state);

	if (data->passwd_state == OFONO_SIM_PASSWORD_INVALID) {
		CALLBACK_WITH_FAILURE(cb, NULL, user_data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, data->retries, user_data);
}

static void card_setup(const struct qmi_uim_slot_info *slot,
					const struct qmi_uim_app_info1 *info1,
					const struct qmi_uim_app_info2 *info2,
							struct sim_data *data)
{
	data->card_state = slot->card_state;
	data->app_type = info1->app_type;

	switch (info1->app_state) {
	case 0x02:	/* PIN1 or UPIN is required */
		data->passwd_state = OFONO_SIM_PASSWORD_SIM_PIN;
		break;
	case 0x03:	/* PUK1 or PUK for UPIN is required */
		data->passwd_state = OFONO_SIM_PASSWORD_SIM_PUK;
		break;
	case 0x07:	/* Ready */
		data->passwd_state = OFONO_SIM_PASSWORD_NONE;
		break;
	default:
		data->passwd_state = OFONO_SIM_PASSWORD_INVALID;
		break;
	}

	data->retries[OFONO_SIM_PASSWORD_SIM_PIN] = info2->pin1_retries;
	data->retries[OFONO_SIM_PASSWORD_SIM_PUK] = info2->puk1_retries;

	data->retries[OFONO_SIM_PASSWORD_SIM_PIN2] = info2->pin2_retries;
	data->retries[OFONO_SIM_PASSWORD_SIM_PUK2] = info2->puk2_retries;
}

static void get_card_status_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);
	const void *ptr;
	const struct qmi_uim_card_status *status;
	uint16_t len, offset;
	uint8_t i;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto done;

	ptr = qmi_result_get(result, QMI_UIM_RESULT_CARD_STATUS, &len);
	if (!ptr)
		goto done;

	status = ptr;
	offset = sizeof(struct qmi_uim_card_status);

	for (i = 0; i < status->num_slot; i++) {
		const struct qmi_uim_slot_info *slot;
		uint8_t n;

		slot = ptr + offset;
		offset += sizeof(struct qmi_uim_slot_info);

		for (n = 0; n < slot->num_app; n++) {
			const struct qmi_uim_app_info1 *info1;
			const struct qmi_uim_app_info2 *info2;
			uint16_t index;

			info1 = ptr + offset;
			offset += sizeof(struct qmi_uim_app_info1);
			offset += info1->aid_len;

			info2 = ptr + offset;
			offset += sizeof(struct qmi_uim_app_info2);

			index = GUINT16_FROM_LE(status->index_gw_pri);

			if ((index & 0xff) == i && (index >> 8) == n)
				card_setup(slot, info1, info2, data);
		}
	}

done:
	ofono_sim_register(sim);

	switch (data->card_state) {
	case 0x00:	/* Absent */
	case 0x02:	/* Error */
		break;
	case 0x01:	/* Present */
		ofono_sim_inserted_notify(sim, TRUE);
		break;
	}
}

static void event_registration_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	if (!qmi_result_get_uint32(result, QMI_UIM_RESULT_EVENT_MASK,
							&data->event_mask))
		goto error;

	DBG("event mask 0x%04x", data->event_mask);

	if (qmi_service_send(data->uim, QMI_UIM_GET_CARD_STATUS, NULL,
					get_card_status_cb, sim, NULL) > 0)
		return;

error:
	ofono_sim_remove(sim);
}


static void create_uim_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);
	struct qmi_param *param;
	uint32_t mask = 0x0003;

	DBG("");

	if (!service) {
		ofono_error("Failed to request UIM service");
		goto error;
	}

	data->uim = qmi_service_ref(service);

	param = qmi_param_new_uint32(QMI_UIM_PARAM_EVENT_MASK, mask);
	if (!param)
		goto error;

	if (qmi_service_send(data->uim, QMI_UIM_EVENT_REGISTRATION, param,
					event_registration_cb, sim, NULL) > 0)
		return;

error:
	qmi_service_unref(data->uim);

	ofono_sim_remove(sim);
}

static int qmi_sim_probe(struct ofono_sim *sim,
				unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct sim_data *data;
	int i;

	DBG("");

	data = g_new0(struct sim_data, 1);

	data->passwd_state = OFONO_SIM_PASSWORD_INVALID;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		data->retries[i] = -1;

	ofono_sim_set_data(sim, data);

	qmi_service_create(device, QMI_SERVICE_UIM, create_uim_cb, sim, NULL);

	return 0;
}

static void qmi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	ofono_sim_set_data(sim, NULL);

	qmi_service_unregister_all(data->uim);

	qmi_service_unref(data->uim);

	g_free(data);
}

static struct ofono_sim_driver driver = {
	.name			= "qmimodem",
	.probe			= qmi_sim_probe,
	.remove			= qmi_sim_remove,
	.read_file_info		= qmi_read_attributes,
	.read_file_transparent	= qmi_read_transparent,
	.read_file_linear	= qmi_read_record,
	.read_file_cyclic	= qmi_read_record,
	.query_passwd_state	= qmi_query_passwd_state,
	.query_pin_retries	= qmi_query_pin_retries,
};

void qmi_sim_init(void)
{
	ofono_sim_driver_register(&driver);
}

void qmi_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
