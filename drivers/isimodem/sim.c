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

#include "ofono.h"
#include "simutil.h"

#include "isimodem.h"
#include "isiutil.h"
#include "sim.h"
#include "debug.h"

#define SIM_MAX_SPN_LENGTH	16

struct sim_data {
	GIsiClient *client;
	GIsiClient *sec_client;
	enum ofono_sim_password_type passwd_state;
	ofono_bool_t ready;
	ofono_bool_t notify_ready;
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

static int sim_resp_status(const GIsiMessage *msg, uint8_t msgid,
				uint8_t service)
{
	uint8_t type = 0;
	uint8_t status;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return -1;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			sim_message_id_name(g_isi_msg_id(msg)));
		return -1;
	}

	if (!g_isi_msg_data_get_byte(msg, 1, &status) ||
			!g_isi_msg_data_get_byte(msg, 0, &type)) {
		DBG("Runt msg: %s", sim_message_id_name(msgid));
		return -1;
	}

	if (status != SIM_SERV_OK)
		DBG("Request failed: %s", sim_isi_cause_name(status));

	if (type != service) {
		DBG("Unexpected service: 0x%02X", type);
		return -1;
	}

	return status;
}

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
	return sim_resp_status(msg, msgid, service) == SIM_SERV_OK;
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

	const struct sim_imsi *resp;
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

static void isi_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("passwd_state %u", sd->passwd_state);

	sd->notify_ready = TRUE;

	switch (sd->passwd_state) {
	case OFONO_SIM_PASSWORD_NONE:
		if (sd->ready)
			CALLBACK_WITH_SUCCESS(cb, sd->passwd_state, data);
		else
			CALLBACK_WITH_FAILURE(cb, -1, data);
		break;

	case OFONO_SIM_PASSWORD_INVALID:
		CALLBACK_WITH_FAILURE(cb, -1, data);
		break;

	default:
		CALLBACK_WITH_SUCCESS(cb, sd->passwd_state, data);
	}
}

static void sim_set_passwd_state(struct ofono_sim *sim,
					enum ofono_sim_password_type pin_type)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	int inserted;
	int previous;

	if (pin_type == sd->passwd_state)
		return;

	DBG("new state \"%s\"", sim_password_name(pin_type));

	inserted = pin_type != OFONO_SIM_PASSWORD_INVALID;
	previous = sd->passwd_state != OFONO_SIM_PASSWORD_INVALID;

	sd->passwd_state = pin_type;

	if (pin_type != OFONO_SIM_PASSWORD_NONE) {
		sd->ready = FALSE;
		sd->notify_ready = FALSE;
	}

	if (inserted != previous)
		ofono_sim_inserted_notify(sim, inserted);
}

static void check_sec_response(const GIsiMessage *msg, void *opaque,
					uint8_t success, uint8_t failure)
{
	struct isi_cb_data *cbd = opaque;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_sim *sim = cbd->user;
	uint8_t id;
	uint8_t cause;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		goto failure;
	}

	id = g_isi_msg_id(msg);

	if (id == success) {
		DBG("%s", sec_message_id_name(id));
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_NONE);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		return;
	}

	if (id == failure && g_isi_msg_data_get_byte(msg, 0, &cause)) {
		DBG("%s(cause=%02x)", sec_message_id_name(id), cause);

		if (cause == SEC_CAUSE_CODE_BLOCKED)
			sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_SIM_PUK);
	} else
		DBG("Error msg: %s", sec_message_id_name(id));

failure:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void sec_code_verify_resp(const GIsiMessage *msg, void *opaque)
{
	check_sec_response(msg, opaque, SEC_CODE_VERIFY_OK_RESP,
				SEC_CODE_VERIFY_FAIL_RESP);
}

static void isi_send_passwd(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);
	unsigned char msg[2 + SEC_CODE_MAX_LENGTH + 1] = {
		SEC_CODE_VERIFY_REQ,
		SEC_CODE_PIN,
	};
	int len = 2 + strlen(passwd) + 1;

	DBG("");

	if (!cbd)
		goto error;

	strcpy((char *) msg + 2, passwd);

	if (g_isi_client_send(sd->sec_client, msg, len,
				sec_code_verify_resp, cbd, g_free))
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void isi_reset_passwd(struct ofono_sim *sim,
				const char *puk, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);
	enum ofono_sim_password_type passwd_type = OFONO_SIM_PASSWORD_SIM_PIN;
	unsigned char msg[2 + 2 * (SEC_CODE_MAX_LENGTH + 1)] = {
		SEC_CODE_VERIFY_REQ,
	};
	size_t len = sizeof(msg);

	DBG("");

	if (!cbd)
		goto error;

	if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN)
		msg[1] = SEC_CODE_PUK;
	else if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)
		msg[1] = SEC_CODE_PUK2;
	else
		goto error;

	strcpy((char *) &msg[2], puk);
	strcpy((char *) &msg[2 + SEC_CODE_MAX_LENGTH + 1], passwd);

	if (g_isi_client_send(sd->sec_client, msg, len,
			sec_code_verify_resp, cbd, g_free))
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}


/* ISI callback: Enable/disable PIN */
static void pin_enable_resp_cb(const GIsiMessage *msg, void *opaque)
{
	check_sec_response(msg, opaque,
			SEC_CODE_STATE_OK_RESP, SEC_CODE_STATE_FAIL_RESP);
}

static void isi_lock(struct ofono_sim *sim,
		enum ofono_sim_password_type passwd_type,
		int enable, const char *passwd,
		ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);

	unsigned char req[3 + SEC_CODE_MAX_LENGTH + 1] = {
		SEC_CODE_STATE_REQ,
	};

	if (!cbd)
		goto error;

	DBG("enable %d pintype %d pass %s", enable, passwd_type, passwd);

	if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN)
		req[1] = SEC_CODE_PIN;
	else if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)
		req[1] = SEC_CODE_PIN2;
	else
		goto error;

	if (enable)
		req[2] = SEC_CODE_ENABLE;
	else
		req[2] = SEC_CODE_DISABLE;

	strcpy((char *) &req[3], passwd);

	if (g_isi_client_send(sd->sec_client, req, sizeof(req),
			pin_enable_resp_cb, cbd, g_free))
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}


/* ISI callback: PIN state (enabled/disabled) query */
static void sec_code_change_resp(const GIsiMessage *msg, void *opaque)
{
	check_sec_response(msg, opaque,
			SEC_CODE_CHANGE_OK_RESP, SEC_CODE_CHANGE_FAIL_RESP);
}


static void isi_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old, const char *new,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);
	unsigned char msg[2 + 2 * (SEC_CODE_MAX_LENGTH + 1)] = {
		SEC_CODE_CHANGE_REQ,
	};

	DBG("passwd_type %d", passwd_type);

	if (!cbd)
		goto error;

	if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN)
		msg[1] = SEC_CODE_PIN;
	else if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)
		msg[1] = SEC_CODE_PIN2;
	else
		goto error;

	strcpy((char *) &msg[2], old);
	strcpy((char *) &msg[2 + SEC_CODE_MAX_LENGTH + 1], new);

	if (g_isi_client_send(sd->sec_client, msg, sizeof(msg),
			sec_code_change_resp, cbd, g_free))
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}


/* ISI callback: PIN state (enabled/disabled) query */
static void sec_code_state_resp_cb(const GIsiMessage *msg, void *opaque)
{
	check_sec_response(msg, opaque, SEC_CODE_STATE_OK_RESP,
				SEC_CODE_STATE_FAIL_RESP);
}

static void isi_query_locked(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				ofono_sim_locked_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct isi_cb_data *cbd = isi_cb_data_new(sim, cb, data);

	unsigned char msg[] = {
		SEC_CODE_STATE_REQ,
		0,
		SEC_CODE_STATE_QUERY
	};

	DBG("");

	if (!cbd)
		goto error;

	if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN)
		msg[1] = SEC_CODE_PIN;
	else if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)
		msg[1] = SEC_CODE_PIN2;
	else
		goto error;

	if (g_isi_client_send(sd->sec_client, msg, sizeof(msg),
			sec_code_state_resp_cb, cbd, g_free))
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void sim_ind_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_sim *sim = opaque;
	uint8_t service;
	uint8_t status;

	DBG("");

	if (g_isi_msg_id(msg) != SIM_IND ||
			!g_isi_msg_data_get_byte(msg, 0, &service) ||
			!g_isi_msg_data_get_byte(msg, 1, &status))
		return;

	if (status == SIM_SERV_PIN_VERIFY_REQUIRED && service == SIM_ST_PIN)
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_SIM_PIN);
	else if (status == SIM_SERV_SIM_BLOCKED)
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_SIM_PUK);
	else if (status == SIM_SERV_INIT_OK && service == SIM_ST_INFO)
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_NONE);
	else if (status == SIM_SERV_SIM_DISCONNECTED)
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_INVALID);
}

static void sim_server_ready_ind_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_sim *sim = opaque;
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	if (sd == NULL || g_isi_msg_id(msg) != SIM_SERVER_READY_IND)
		return;

	sd->ready = TRUE;

	if (sd->notify_ready)
		__ofono_sim_recheck_pin(sim);
}

static void read_dyn_flags_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_sim *sim = opaque;
	struct sim_data *sd = ofono_sim_get_data(sim);
	int status;

	status = sim_resp_status(msg, SIM_DYNAMIC_FLAGS_RESP, READ_DYN_FLAGS);

	if (status < 0 || status == SIM_SERV_NOTREADY)
		return;

	sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_NONE);

	sd->ready = TRUE;

	if (sd->notify_ready)
		__ofono_sim_recheck_pin(sim);
}

static void read_dyn_flags_req(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	unsigned char req[] = {
		SIM_DYNAMIC_FLAGS_REQ,
		READ_DYN_FLAGS,
		0
	};

	g_isi_client_send(sd->client, req, sizeof(req),
				read_dyn_flags_cb, sim, NULL);
}

static void sec_state_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_sim *sim = opaque;
	uint8_t msgid;
	uint8_t cause;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return;
	}

	msgid = g_isi_msg_id(msg);

	if (msgid != SEC_STATE_RESP) {
		DBG("Unexpected msg: %s", sec_message_id_name(msgid));
		return;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &cause)) {
		DBG("Runt msg: %s", sec_message_id_name(msgid));
		return;
	}

	DBG("%s(cause=0x%0x)", sec_message_id_name(msgid), cause);

	switch (cause) {
	case SEC_STARTUP_OK:
		DBG("SEC_STARTUP_OK");
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_NONE);
		/* Check if SIM server is already ready */
		read_dyn_flags_req(sim);
		break;

	case SEC_CAUSE_PIN_REQUIRED:
		DBG("SEC_CAUSE_PIN_REQUIRED");
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_SIM_PIN);
		break;

	case SEC_CAUSE_PUK_REQUIRED:
		DBG("SEC_CAUSE_PUK_REQUIRED");
		sim_set_passwd_state(sim, OFONO_SIM_PASSWORD_SIM_PIN);
		break;

	case SEC_CAUSE_NO_SIM:
		DBG("SEC_CAUSE_NO_SIM");
		break;

	case SEC_CAUSE_INVALID_SIM:
		DBG("SEC_CAUSE_INVALID_SIM");
		break;

	case SEC_CAUSE_SIM_REJECTED:
		DBG("SEC_CAUSE_SIM_REJECTED");
		break;

	default:
		break;
	}
}

static void isi_sec_state_req(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	unsigned char req[] = {
		SEC_STATE_REQ,
		0,
		0
	};

	g_isi_client_send(sd->sec_client, req, sizeof(req),
			sec_state_resp_cb, sim, NULL);
}

static void sim_status_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_sim *sim = opaque;
	struct sim_data *sd = ofono_sim_get_data(sim);
	int status = sim_resp_status(msg, SIM_STATUS_RESP, SIM_ST_CARD_STATUS);

	if (status < 0 || status == SIM_SERV_SIM_DISCONNECTED)
		return;

	/* We probably have a SIM. */
	if (sd->sec_client)
		isi_sec_state_req(sim);
	else
		read_dyn_flags_req(sim);
}

static void isi_sim_status_req(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	const unsigned char req[] = {
		SIM_STATUS_REQ,
		SIM_ST_CARD_STATUS
	};

	g_isi_client_send(sd->client, req, sizeof(req),
			sim_status_resp_cb, sim, NULL);
}

static void sec_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	if (g_isi_msg_error(msg) < 0) {
		DBG("PN_SECURITY: %s", strerror(-g_isi_msg_error(msg)));
		DBG("PIN code handling not available");
		g_isi_client_destroy(sd->sec_client);
		sd->sec_client = NULL;
	}

	g_isi_client_ind_subscribe(sd->client, SIM_IND, sim_ind_cb, sim);
	g_isi_client_ind_subscribe(sd->client, SIM_SERVER_READY_IND,
					sim_server_ready_ind_cb, sim);
	/* Check if we have a SIM */
	isi_sim_status_req(sim);

	ofono_sim_register(sim);
}

static void sim_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	if (g_isi_msg_error(msg) < 0) {
		DBG("PN_SIM: %s", strerror(-g_isi_msg_error(msg)));
		ofono_sim_remove(sim);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	g_isi_client_verify(sd->sec_client, sec_reachable_cb, sim, NULL);
}

static int isi_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct sim_data *sd;

	sd = g_try_new0(struct sim_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;

	sd->client = g_isi_client_create(modem, PN_SIM);
	if (sd->client == NULL)
		goto error;

	sd->sec_client = g_isi_client_create(modem, PN_SECURITY);
	if (sd->sec_client == NULL)
		goto error;

	g_isi_client_set_timeout(sd->client, SIM_TIMEOUT);
	g_isi_client_set_timeout(sd->sec_client, SIM_TIMEOUT);

	ofono_sim_set_data(sim, sd);

	g_isi_client_ind_subscribe(sd->client, SIM_IND, sim_ind_cb, sim);
	g_isi_client_verify(sd->client, sim_reachable_cb, sim, NULL);

	return 0;

error:
	g_isi_client_destroy(sd->client);
	g_isi_client_destroy(sd->sec_client);

	return -ENOMEM;
}

static void isi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_isi_client_destroy(data->sec_client);
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
	.query_passwd_state	= isi_query_passwd_state,
	.send_passwd		= isi_send_passwd,
	.reset_passwd		= isi_reset_passwd,
	.lock			= isi_lock,
	.change_passwd		= isi_change_passwd,
	.query_locked		= isi_query_locked,
};

void isi_sim_init(void)
{
	ofono_sim_driver_register(&driver);
}

void isi_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
