/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) ST-Ericsson SA 2011.
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
#include <sys/uio.h>
#include <inttypes.h>

#include <glib.h>

#include <gisi/message.h>
#include <gisi/client.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sms.h>

#include "smsutil.h"
#include "isimodem.h"
#include "isiutil.h"
#include "sms.h"
#include "sim.h"
#include "debug.h"

/* This is a straightforward copy of the EF_smsp structure */
struct sim_efsmsp{
	uint8_t absent;
	uint8_t tp_pid;
	uint8_t tp_dcs;
	uint8_t tp_vp;
	uint8_t dst[12];
	uint8_t sca[12];
	uint8_t alphalen;
	uint8_t filler[3];
	uint16_t alpha[17];
};

/* Sub-block used by PN_SMS */
struct sms_params {
	uint8_t location;
	uint8_t absent;
	uint8_t tp_pid;
	uint8_t tp_dcs;
	uint8_t dst[12];
	uint8_t sca[12];
	uint8_t tp_vp;
	uint8_t alphalen;
	uint8_t filler[2];
	uint16_t alpha[17];
};

struct sms_report {
	uint8_t type;
	uint8_t cause;
	uint8_t ref;
};

struct sms_status {
	uint8_t status;
	uint8_t ref;
	uint8_t route;
	uint8_t cseg;	/* Current segment */
	uint8_t tseg;	/* Total segments */
};

struct sms_addr {
	uint8_t type;
	uint8_t len;
	uint8_t *data;
};

struct sms_common {
	uint8_t len;
	uint8_t *data;
};

struct sms_data {
	GIsiClient *client;
	GIsiClient *sim;
	GIsiVersion version;
	struct sim_efsmsp params;
};

static uint8_t bearer_to_cs_pref(int bearer)
{
	switch (bearer) {
	case 0:
		return SMS_ROUTE_NOT_AVAILABLE;
	case 1:
		return SMS_ROUTE_PRIORITY_1;
	case 2:
		return SMS_ROUTE_PRIORITY_2;
	case 3:
		return SMS_ROUTE_PRIORITY_1;
	}

	return SMS_ROUTE_NOT_AVAILABLE;
}

static uint8_t bearer_to_ps_pref(int bearer)
{
	switch (bearer) {
	case 0:
		return SMS_ROUTE_PRIORITY_1;
	case 1:
		return SMS_ROUTE_NOT_AVAILABLE;
	case 2:
		return SMS_ROUTE_PRIORITY_1;
	case 3:
		return SMS_ROUTE_PRIORITY_2;
	}

	return SMS_ROUTE_NOT_AVAILABLE;
}

static int cs_ps_pref_to_bearer(uint8_t cs, uint8_t ps)
{
	if (cs == SMS_ROUTE_NOT_AVAILABLE && ps == SMS_ROUTE_PRIORITY_1)
		return 0;

	if (cs == SMS_ROUTE_PRIORITY_1 && ps == SMS_ROUTE_NOT_AVAILABLE)
		return 1;

	if (cs == SMS_ROUTE_PRIORITY_2 && ps == SMS_ROUTE_PRIORITY_1)
		return 2;

	if (cs == SMS_ROUTE_PRIORITY_1 && ps == SMS_ROUTE_PRIORITY_2)
		return 3;

	return 0;
}

static gboolean check_sim(const GIsiMessage *msg, uint8_t msgid, uint8_t service)
{
	uint8_t type;
	uint8_t cause;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s", sms_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &type))
		return FALSE;

	if (type != service) {
		DBG("Unexpected service type: 0x%02X", type);
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 1, &cause))
		return FALSE;

	if (cause != SIM_SERV_OK) {
		DBG("Request failed: %s", sim_isi_cause_name(cause));
		return FALSE;
	}

	return TRUE;
}

static gboolean check_sms(const GIsiMessage *msg, uint8_t msgid, int expect)
{
	uint8_t cause;
	int pos;

	/*
	* Quirk for the cause code position in the response. More
	* recent versions of the API use 16bit subblock IDs, causing
	* the cause to be bumped forward by one byte.
	*/
	if (ISI_VERSION_AT_LEAST(msg->version, 9, 1))
		pos = 1;
	else
		pos = 0;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", g_isi_msg_strerror(msg));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			sms_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (expect == -1)
		return TRUE;

	if (!g_isi_msg_data_get_byte(msg, pos, &cause)) {
		DBG("Unable to parse cause");
		return FALSE;
	}

	if (cause == expect)
		return TRUE;

	if (cause == SMS_ERR_PP_RESERVED) {
		DBG("Request failed: 0x%02"PRIx8" (%s).\n\n  Unable to "
			"bootstrap SMS routing.\n  It appears some other "
			"component is already\n  registered as the SMS "
			"routing endpoint.\n  As a consequence, "
			"only sending SMSs is going to work.\n\n",
			cause, sms_isi_cause_name(cause));
		return TRUE;
	}

	DBG("Request failed: %s", sms_isi_cause_name(cause));
	return FALSE;
}

static void sca_sim_query_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	struct ofono_sms *sms = cbd->user;
	struct sms_data *sd = ofono_sms_get_data(sms);
	ofono_sms_sca_query_cb_t cb = cbd->cb;

	struct ofono_phone_number sca;
	struct sms_params *info;
	size_t len = sizeof(struct sms_params);
	uint8_t bcd_len;

	if (!check_sim(msg, SIM_SMS_RESP, READ_PARAMETER))
		goto error;

	if (!g_isi_msg_data_get_struct(msg, 2, (const void **) &info, len))
		goto error;

	if (info->alphalen > 17)
		info->alphalen = 17;
	else if (info->alphalen < 1)
		info->alphalen = 1;

	info->alpha[info->alphalen - 1] = '\0';

	sd->params.absent = info->absent;
	sd->params.tp_pid = info->tp_pid;
	sd->params.tp_dcs = info->tp_dcs;
	sd->params.tp_vp = info->tp_vp;

	memcpy(sd->params.dst, info->dst, sizeof(sd->params.dst));
	memcpy(sd->params.sca, info->sca, sizeof(sd->params.sca));

	sd->params.alphalen = info->alphalen;
	memcpy(sd->params.alpha, info->alpha, sizeof(sd->params.alpha));

	/*
	 * Bitmask indicating absence of parameters --
	 * If second bit is set it indicates that the SCA is absent
	 */
	if (info->absent & 0x2)
		goto error;

	bcd_len = info->sca[0];

	if (bcd_len == 0 || bcd_len > 12)
		goto error;

	extract_bcd_number(info->sca + 2, bcd_len - 1, sca.number);
	sca.type = 0x80 | info->sca[1];

	CALLBACK_WITH_SUCCESS(cb, &sca, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static gboolean sca_sim_query(GIsiClient *client, void *data, GDestroyNotify notify)
{
	const uint8_t msg[] = {
		SIM_SMS_REQ,
		READ_PARAMETER,
		1,	/* Location, default is 1 */
	};

	return g_isi_client_send(client, msg, sizeof(msg), sca_sim_query_resp_cb,
					data, notify);
}

static void isi_sca_query(struct ofono_sms *sms,
				ofono_sms_sca_query_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);

	if (cbd == NULL || sd->sim == NULL)
		goto error;

	if (sca_sim_query(sd->sim, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
}

static void sca_sim_set_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sms_sca_set_cb_t cb = cbd->cb;

	if (!check_sim(msg, SIM_SMS_RESP, UPDATE_PARAMETER)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static gboolean sca_sim_set(GIsiClient *client, struct sim_efsmsp *params,
				const struct ofono_phone_number *sca, void *data,
				GDestroyNotify notify)
{
	uint8_t msg[] = {
		SIM_SMS_REQ,
		UPDATE_PARAMETER,
		1,	/* Location, default is 1 */
	};
	struct iovec iov[2] = {
		{ msg, sizeof(msg) },
		{ params, sizeof(struct sim_efsmsp) },
	};
	uint8_t *bcd;

	bcd = params->sca;
	params->absent &= ~SMS_PI_SERVICE_CENTER_ADDRESS;

	encode_bcd_number(sca->number, bcd + 2);
	bcd[0] = 1 + (strlen(sca->number) + 1) / 2;
	bcd[1] = sca->type & 0xFF;

	return g_isi_client_vsend(client, iov, 2, sca_sim_set_resp_cb,
					data, notify);
}

static void isi_sca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);

	if (cbd == NULL || sd->sim == NULL)
		goto error;

	if (sca_sim_set(sd->sim, &sd->params, sca, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void submit_failure_debug(struct sms_report *report)
{
	const char *cause;

	if (report->type == SMS_CAUSE_TYPE_COMMON)
		cause = sms_isi_cause_name(report->cause);
	else
		cause = sms_gsm_cause_name(report->cause);

	DBG("Message 0x%02"PRIx8" failed: %s", report->ref, cause);
}

static void submit_tpdu_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct sms_report *report;
	size_t len = sizeof(struct sms_report);

	if (!check_sms(msg, SMS_MESSAGE_SEND_RESP, -1))
		goto error;

	if (g_isi_msg_data_len(msg) < len)
		goto error;

	if (!g_isi_msg_data_get_struct(msg, 0, (const void **) &report, len))
		goto error;

	if (report->type == SMS_CAUSE_TYPE_COMMON && report->cause == SMS_OK) {
		CALLBACK_WITH_SUCCESS(cb, report->ref, cbd->data);
		return;
	}

	submit_failure_debug(report);

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void submit_gsm_tpdu_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct sms_report *report;
	size_t len = sizeof(struct sms_report);
	GIsiSubBlockIter iter;

	if (!check_sms(msg, SMS_MESSAGE_SEND_RESP, -1))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SMS_GSM_REPORT)
			continue;

		if (!g_isi_sb_iter_get_struct(&iter, (void **) &report, len, 2))
			goto error;

		if (report->type == SMS_CAUSE_TYPE_COMMON &&
				report->cause == SMS_OK) {
			CALLBACK_WITH_SUCCESS(cb, report->ref, cbd->data);
			return;
		}

		submit_failure_debug(report);
	}

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static gboolean submit_tpdu(GIsiClient *client, unsigned char *pdu, int pdu_len,
				int tpdu_len, int mms, void *data,
				GDestroyNotify notify)
{
	uint8_t use_sca = (pdu_len - tpdu_len) > 1;
	size_t sca_sb_len = use_sca ? 18 : 0;
	size_t tpdu_sb_len = ALIGN4(6 + tpdu_len);
	size_t tpdu_pad_len = tpdu_sb_len - (6 + tpdu_len);

	uint8_t msg[] = {
		SMS_MESSAGE_SEND_REQ,
		mms,			/* More messages to send */
		SMS_ROUTE_ANY,		/* Use any (default) route */
		0,			/* Repeated message */
		0, 0,			/* Filler */
		use_sca ? 3 : 2,	/* Subblock count */
		ISI_16BIT(SMS_SB_SMS_PARAMETERS),
		ISI_16BIT(8),		/* Subblock length */
		SMS_PARAMETER_LOCATION_DEFAULT,
		SMS_PI_SERVICE_CENTER_ADDRESS,
		0, 0,			/* Filler */
		ISI_16BIT(SMS_SB_TPDU),
		ISI_16BIT(tpdu_sb_len),
		tpdu_len,
		0,			/* Filler */
		/* Databytes aligned to next 32bit boundary */
	};
	uint8_t sca_sb[18] = {
		ISI_16BIT(SMS_SB_ADDRESS),
		ISI_16BIT(18),
		SMS_SMSC_ADDRESS,
		0,			/* Filled in later */
	};
	uint8_t padding[4] = { 0 };
	struct iovec iov[4] = {
		{ msg, sizeof(msg) },
		{ pdu + pdu_len - tpdu_len, tpdu_len },
		{ padding, tpdu_pad_len },
		{ sca_sb, sca_sb_len },
	};

	if (use_sca) {
		sca_sb[5] = pdu_len - tpdu_len;
		memcpy(sca_sb + 6, pdu, pdu_len - tpdu_len);
	}

	return g_isi_client_vsend_with_timeout(client, iov, 4, SMS_TIMEOUT,
						submit_tpdu_resp_cb, data,
						notify);
}

static gboolean submit_gsm_tpdu(GIsiClient *client, unsigned char *pdu,
				int pdu_len, int tpdu_len, int mms,
				void *data, GDestroyNotify notify)
{
	uint8_t use_sca = (pdu_len - tpdu_len) > 1;
	size_t sca_sb_len = use_sca ? 16 : 0;
	size_t tpdu_sb_len = ALIGN4(4 + tpdu_len);
	size_t tpdu_pad_len = tpdu_sb_len - (4 + tpdu_len);

	uint8_t msg[] = {
		SMS_MESSAGE_SEND_REQ,
		mms,	/* More messages to send */
		SMS_ROUTE_CS_PREF,
		0,	/* Repeated message */
		SMS_SENDER_ANY,
		SMS_TYPE_TEXT_MESSAGE,
		1,	/* Subblock count */
		SMS_GSM_TPDU,
		tpdu_sb_len + sca_sb_len,
		0,			/* Filler */
		use_sca ? 2 : 1,	/* Sub-sub blocks */
		SMS_COMMON_DATA,
		tpdu_sb_len,
		tpdu_len,
		0,	/* Packing required? */
		/* Databytes aligned to next 32bit boundary */
	};
	uint8_t sca_sb[16] = {
		SMS_ADDRESS,
		16,	/* Subblock length */
		SMS_GSM_0411_ADDRESS,
		0,	/* Filled in later */
	};
	uint8_t padding[4] = { 0 };
	struct iovec iov[4] = {
		{ msg, sizeof(msg) },
		{ pdu + pdu_len - tpdu_len, tpdu_len },
		{ padding, tpdu_pad_len },
		{ sca_sb, sca_sb_len },
	};

	if (use_sca) {
		sca_sb[3] = pdu_len - tpdu_len;
		memcpy(sca_sb + 4, pdu, pdu_len - tpdu_len);
	}

	/*
	 * Modem seems to time out SMS_MESSAGE_SEND_REQ in 5 seconds.
	 * Wait normal timeout plus the modem timeout.
	 */
	return g_isi_client_vsend_with_timeout(client, iov, 4, SMS_TIMEOUT + 5,
						submit_gsm_tpdu_resp_cb, data,
						notify);
}

static void isi_submit(struct ofono_sms *sms, unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);

	if (cbd == NULL)
		goto error;

	if (ISI_VERSION_AT_LEAST(&sd->version, 9, 1)) {
		if (submit_tpdu(sd->client, pdu, pdu_len, tpdu_len, mms,
				cbd, g_free))
			return;
	} else {
		if (submit_gsm_tpdu(sd->client, pdu, pdu_len, tpdu_len, mms,
					cbd, g_free))
			return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static void bearer_query_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sms_bearer_query_cb_t cb = cbd->cb;
	GIsiSubBlockIter iter;
	uint8_t sb, cs, ps;

	if (!check_sms(msg, SMS_SETTINGS_READ_RESP, SMS_OK))
		goto error;

	if (!g_isi_msg_data_get_byte(msg, 1, &sb))
		goto error;

	for (g_isi_sb_iter_init_full(&iter, msg, 2, TRUE, sb);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SMS_SB_ROUTE_INFO)
			continue;

		if (!g_isi_msg_data_get_byte(msg, 5, &cs))
			goto error;

		if (!g_isi_msg_data_get_byte(msg, 6, &ps))
			goto error;

		CALLBACK_WITH_SUCCESS(cb, cs_ps_pref_to_bearer(cs, ps),
					cbd->data);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
}

static void isi_bearer_query(struct ofono_sms *sms,
				ofono_sms_bearer_query_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);
	const uint8_t msg[] = {
		SMS_SETTINGS_READ_REQ,
		SMS_SETTING_TYPE_ROUTE,
		0,
	};

	DBG("");

	if (cbd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, sizeof(msg), bearer_query_resp_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static void bearer_set_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sms_bearer_set_cb_t cb = cbd->cb;

	if (check_sms(msg, SMS_SETTINGS_UPDATE_RESP, SMS_OK))
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void isi_bearer_set(struct ofono_sms *sms, int bearer,
				ofono_sms_bearer_set_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);
	const uint8_t msg[] = {
		SMS_SETTINGS_UPDATE_REQ,
		SMS_SETTING_TYPE_ROUTE,
		1,		/* Subblock count */
		ISI_16BIT(SMS_SB_ROUTE_INFO),
		ISI_16BIT(8),	/* Subblock length */
		bearer_to_cs_pref(bearer),	/* CS priority */
		bearer_to_ps_pref(bearer),	/* PS priority */
		0, 0,
	};

	if (cbd == NULL)
		goto error;

	if (g_isi_client_send(sd->client, msg, sizeof(msg), bearer_set_resp_cb,
				cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void send_status_ind_cb(const GIsiMessage *msg, void *data)
{
	struct sms_status *info;
	size_t len = sizeof(struct sms_status);

	DBG("");

	if (g_isi_msg_id(msg) != SMS_MESSAGE_SEND_STATUS_IND)
		return;

	if (!g_isi_msg_data_get_struct(msg, 0, (const void **) &info, len))
		return;

	DBG("status=0x%"PRIx8", ref=0x%"PRIx8", route=0x%"PRIx8
		", cseg=0x%"PRIx8", tseg=0x%"PRIx8,
		info->status, info->ref, info->route, info->cseg,
		info->tseg);

	DBG("TODO: Status notification");
}

static void gsm_report_resp_cb(const GIsiMessage *msg, void *data)
{
	if (!check_sms(msg, SMS_GSM_RECEIVED_PP_REPORT_RESP, SMS_OK))
		DBG("Sending report failed");
}

static void report_resp_cb(const GIsiMessage *msg, void *data)
{
	if (!check_sms(msg, SMS_RECEIVED_MSG_REPORT_RESP, SMS_OK))
		DBG("Sending report failed");
}

static gboolean send_gsm_deliver_report(GIsiClient *client, gboolean success,
					void *data, GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		SMS_GSM_RECEIVED_PP_REPORT_REQ,
		success ? 0 : SMS_CAUSE_TYPE_GSM,
		success ? SMS_OK : SMS_GSM_ERR_MEMORY_CAPACITY_EXC,
		0, 0, 0,	/* Filler */
		1,		/* Sub blocks */
		SMS_GSM_DELIVER_REPORT,
		8,		/* Subblock length */
		0,		/* Message parameters */
		0,		/* Cause type */
		0, 0, 0,	/* Filler */
		0,		/* Sub blocks */
	};

	return g_isi_client_send(client, msg, sizeof(msg), gsm_report_resp_cb,
					data, destroy);
}

static gboolean send_deliver_report(GIsiClient *client, gboolean success,
					void *data, GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		SMS_RECEIVED_MSG_REPORT_REQ,
		success ? 0 : SMS_CAUSE_TYPE_GSM,
		success ? SMS_OK : SMS_GSM_ERR_MEMORY_CAPACITY_EXC,
		0, 0, 0,	/* Filler */
		0,		/* Subblocks */
	};

	return g_isi_client_send(client, msg, sizeof(msg), report_resp_cb,
					data, destroy);
}

static gboolean parse_sms_address(GIsiSubBlockIter *iter, unsigned offset,
					struct sms_addr *add)
{
	add->data = NULL;

	if (!g_isi_sb_iter_get_byte(iter, &add->type, offset))
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &add->len, offset + 1))
		return FALSE;

	if (add->len == 0)
		return FALSE;

	if (!g_isi_sb_iter_get_struct(iter, (void **) &add->data, add->len,
					offset + 2))
		return FALSE;

	return TRUE;
}

static gboolean parse_sms_tpdu(GIsiSubBlockIter *iter, unsigned offset,
				struct sms_common *com)
{
	com->data = NULL;

	if (!g_isi_sb_iter_get_byte(iter, &com->len, offset))
		return FALSE;

	if (com->len == 0)
		return FALSE;

	if (!g_isi_sb_iter_get_struct(iter, (void **) &com->data, com->len,
					offset + 2))
		return FALSE;

	return TRUE;
}

static gboolean parse_gsm_tpdu(GIsiSubBlockIter *parent, struct sms_addr *add,
				struct sms_common *com)
{
	GIsiSubBlockIter iter;

	for (g_isi_sb_subiter_init(parent, &iter, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case SMS_ADDRESS:

			if (!parse_sms_address(&iter, 2, add))
				return FALSE;

			if (add->type != SMS_GSM_0411_ADDRESS)
				return FALSE;

			break;

		case SMS_COMMON_DATA:

			if (!parse_sms_tpdu(&iter, 2, com))
				return FALSE;

			break;
		}
	}

	return TRUE;
}

static void routing_ntf_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct sms_common tpdu;
	struct sms_addr addr;
	GIsiSubBlockIter iter;

	uint8_t pdu[176];

	if (g_isi_msg_id(msg) != SMS_PP_ROUTING_NTF)
		return;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SMS_GSM_TPDU)
			continue;

		if (!parse_gsm_tpdu(&iter, &addr, &tpdu))
			return;
	}

	if (tpdu.data == NULL || addr.data == NULL ||
			tpdu.len + addr.len > sizeof(pdu))
		return;

	memcpy(pdu, addr.data, addr.len);
	memcpy(pdu + addr.len, tpdu.data, tpdu.len);

	/* 23.040 9.2.3.1 */
	if ((tpdu.data[0] & 0x03) == 0x02)
		ofono_sms_status_notify(sms, pdu, tpdu.len + addr.len, tpdu.len);
	else
		ofono_sms_deliver_notify(sms, pdu, tpdu.len + addr.len, tpdu.len);

	send_gsm_deliver_report(sd->client, TRUE, NULL, NULL);
}

static void received_msg_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct sms_common tpdu;
	struct sms_addr addr;
	GIsiSubBlockIter iter;

	uint8_t pdu[176];
	uint8_t sbcount;

	DBG("");

	if (g_isi_msg_id(msg) != SMS_RECEIVED_MSG_IND)
		return;

	if (!g_isi_msg_data_get_byte(msg, 1, &sbcount))
		return;

	for (g_isi_sb_iter_init_full(&iter, msg, 2, TRUE, sbcount);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case SMS_ADDRESS:

			if (!parse_sms_address(&iter, 4, &addr))
				return;

			if (addr.type != SMS_SMSC_ADDRESS)
				return;

			break;

		case SMS_SB_TPDU:

			if (!parse_sms_tpdu(&iter, 4, &tpdu))
				return;

			break;
		}
	}

	if (tpdu.data == NULL || addr.data == NULL ||
			tpdu.len + addr.len > sizeof(pdu))
		return;

	memcpy(pdu, addr.data, addr.len);
	memcpy(pdu + addr.len, tpdu.data, tpdu.len);

	/* 23.040 9.2.3.1 */
	if ((tpdu.data[0] & 0x03) == 0x02)
		ofono_sms_status_notify(sms, pdu, tpdu.len + addr.len, tpdu.len);
	else
		ofono_sms_deliver_notify(sms, pdu, tpdu.len + addr.len, tpdu.len);

	send_deliver_report(sd->client, TRUE, NULL, NULL);
}

static void reception_resp_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;

	if (sms == NULL)
		return;

	if (!check_sms(msg, SMS_RECEIVE_MESSAGE_RESP, SMS_RECEPTION_ACTIVE))
		return;

	ofono_sms_register(sms);
}

static void routing_resp_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;

	if (sms == NULL)
		return;

	if (!check_sms(msg, SMS_PP_ROUTING_RESP, SMS_OK))
		return;

	ofono_sms_register(sms);
}

static void sim_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;
	struct sms_data *sd = ofono_sms_get_data(sms);

	if (sd == NULL)
		return;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Unable to bootstrap SIM service");

		g_isi_client_destroy(sd->sim);
		sd->sim = NULL;
		return;
	}

	ISI_RESOURCE_DBG(msg);
}

static gboolean set_routing(GIsiClient *client, void *data,
				GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		SMS_PP_ROUTING_REQ,
		SMS_ROUTING_SET,
		1,		/* Sub-block count */
		SMS_GSM_ROUTING,
		8,		/* Sub-block length */
		SMS_GSM_TPDU_ROUTING,
		SMS_GSM_MT_ALL_TYPE,
		0, 0, 0,	/* Filler */
		0,		/* Sub-sub-block count */
	};

	return g_isi_client_send(client, msg, sizeof(msg), routing_resp_cb,
					data, destroy);
}

static gboolean unset_routing(GIsiClient *client)
{
	const uint8_t msg[] = {
		SMS_PP_ROUTING_REQ,
		SMS_ROUTING_RELEASE,
		0x01,		/* Sub-block count */
		SMS_GSM_ROUTING,
		0x08,		/* Sub-block length */
		SMS_GSM_TPDU_ROUTING,
		SMS_GSM_MT_ALL_TYPE,
		0, 0, 0,	/* Filler */
		0,		/* Sub-sub-block count */
	};

	return g_isi_client_send(client, msg, sizeof(msg), NULL, NULL, NULL);
}

static gboolean activate_reception(GIsiClient *client, void *data,
					GDestroyNotify destroy)
{
	const uint8_t msg[] = {
		SMS_RECEIVE_MESSAGE_REQ,
		SMS_RECEPTION_ACTIVATE,
		0,
	};

	return g_isi_client_send(client, msg, sizeof(msg), reception_resp_cb,
					data, destroy);
}

static gboolean deactivate_reception(GIsiClient *client)
{
	const uint8_t msg[] = {
		SMS_RECEIVE_MESSAGE_REQ,
		SMS_RECEPTION_DEACTIVATE,
		0,
	};

	return g_isi_client_send(client, msg, sizeof(msg), NULL, NULL, NULL);
}

static void sms_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;
	struct sms_data *sd = ofono_sms_get_data(sms);

	if (g_isi_msg_error(msg) < 0) {
		DBG("unable to find SMS resource");
		ofono_sms_remove(sms);
		return;
	}

	if (sd == NULL)
		return;

	ISI_RESOURCE_DBG(msg);

	sd->version.major = g_isi_msg_version_major(msg);
	sd->version.minor = g_isi_msg_version_minor(msg);

	if (ISI_VERSION_AT_LEAST(&sd->version, 9, 1))
		activate_reception(sd->client, sms, NULL);
	else
		set_routing(sd->client, sms, NULL);

	g_isi_client_verify(sd->sim, sim_reachable_cb, sms, NULL);
}

static int isi_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct sms_data *sd = g_try_new0(struct sms_data, 1);

	if (sd == NULL)
		return -ENOMEM;

	sd->params.absent = 0xFF;
	sd->params.alphalen = 1; /* Includes final UCS2-coded NUL */

	sd->client = g_isi_client_create(modem, PN_SMS);
	if (sd->client == NULL)
		goto nomem;

	sd->sim = g_isi_client_create(modem, PN_SIM);
	if (sd->sim == NULL)
		goto nomem;

	ofono_sms_set_data(sms, sd);

	g_isi_client_ind_subscribe(sd->client, SMS_MESSAGE_SEND_STATUS_IND,
					send_status_ind_cb, sms);
	g_isi_client_ind_subscribe(sd->client, SMS_RECEIVED_MSG_IND,
					received_msg_ind_cb, sms);
	g_isi_client_ntf_subscribe(sd->client, SMS_PP_ROUTING_NTF,
					routing_ntf_cb, sms);
	g_isi_client_verify(sd->client, sms_reachable_cb, sms, NULL);

	return 0;

nomem:
	g_isi_client_destroy(sd->client);
	g_free(sd);
	return -ENOMEM;
}

static void isi_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);

	if (sd == NULL)
		return;

	ofono_sms_set_data(sms, NULL);

	/*
	 * Send a promiscuous routing release, so as not to
	 * hog resources unnecessarily after being removed
	 */
	if (ISI_VERSION_AT_LEAST(&sd->version, 9, 1))
		deactivate_reception(sd->client);
	else
		unset_routing(sd->client);

	g_isi_client_destroy(sd->client);
	g_isi_client_destroy(sd->sim);
	g_free(sd);
}

static struct ofono_sms_driver driver = {
	.name			= "isimodem",
	.probe			= isi_sms_probe,
	.remove			= isi_sms_remove,
	.sca_query		= isi_sca_query,
	.sca_set		= isi_sca_set,
	.submit			= isi_submit,
	.bearer_query		= isi_bearer_query,
	.bearer_set		= isi_bearer_set,
};

void isi_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void isi_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
