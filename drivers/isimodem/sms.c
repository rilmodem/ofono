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
	struct sim_efsmsp params;
};

static gboolean check_sim_status(const GIsiMessage *msg, uint8_t msgid,
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
			sms_message_id_name(g_isi_msg_id(msg)));
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

static gboolean check_sms_status(const GIsiMessage *msg, uint8_t msgid)
{
	uint8_t cause;

	if (g_isi_msg_error(msg) < 0) {
		DBG("Error: %s", strerror(-g_isi_msg_error(msg)));
		return FALSE;
	}

	if (g_isi_msg_id(msg) != msgid) {
		DBG("Unexpected msg: %s",
			sms_message_id_name(g_isi_msg_id(msg)));
		return FALSE;
	}

	if (!g_isi_msg_data_get_byte(msg, 0, &cause)) {
		DBG("Unable to parse cause");
		return FALSE;
	}

	if (cause == SMS_OK)
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

static void sca_query_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	struct ofono_sms *sms = cbd->user;
	struct sms_data *sd = ofono_sms_get_data(sms);
	ofono_sms_sca_query_cb_t cb = cbd->cb;

	struct ofono_phone_number sca;
	struct sms_params *info;
	size_t len = sizeof(struct sms_params);
	uint8_t bcd_len;

	if (!check_sim_status(msg, SIM_SMS_RESP, READ_PARAMETER))
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
	 * Bitmask indicating absense of parameters --
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

static void isi_sca_query(struct ofono_sms *sms,
				ofono_sms_sca_query_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);

	const uint8_t msg[] = {
		SIM_SMS_REQ,
		READ_PARAMETER,
		1,	/* Location, default is 1 */
	};

	if (cbd == NULL || sd == NULL)
		goto error;

	if (g_isi_client_send(sd->sim, msg, sizeof(msg),
				sca_query_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
}

static void sca_set_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sms_sca_set_cb_t cb = cbd->cb;

	if (!check_sim_status(msg, SIM_SMS_RESP, UPDATE_PARAMETER)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void isi_sca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);
	uint8_t *bcd;

	uint8_t msg[] = {
		SIM_SMS_REQ,
		UPDATE_PARAMETER,
		1,	/* Location, default is 1 */
	};

	struct iovec iov[2] = {
		{ msg, sizeof(msg) },
		{ &sd->params, sizeof(sd->params) },
	};

	if (cbd == NULL || sd == NULL)
		goto error;

	bcd = sd->params.sca;
	sd->params.absent &= ~0x02;

	encode_bcd_number(sca->number, bcd + 2);
	bcd[0] = 1 + (strlen(sca->number) + 1) / 2;
	bcd[1] = sca->type & 0xFF;

	if (g_isi_client_vsend(sd->sim, iov, 2, sca_set_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void submit_resp_cb(const GIsiMessage *msg, void *data)
{
	struct isi_cb_data *cbd = data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct sms_report *report;
	size_t len = sizeof(struct sms_report);
	GIsiSubBlockIter iter;

	if (!check_sms_status(msg, SMS_MESSAGE_SEND_RESP))
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		if (g_isi_sb_iter_get_id(&iter) != SMS_GSM_REPORT)
			continue;

		if (!g_isi_sb_iter_get_struct(&iter, (void **) &report, len, 2))
			goto error;

		if (report->cause != SMS_OK)
			goto error;

		CALLBACK_WITH_SUCCESS(cb, report->ref, cbd->data);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void isi_submit(struct ofono_sms *sms, unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);

	uint8_t use_sca = pdu_len - tpdu_len != 1 || pdu[0] == 0;

	uint8_t *tpdu = pdu + pdu_len - tpdu_len;
	uint8_t filler_len = (-tpdu_len) & 3;
	uint8_t tpdu_sb_len = 4 + tpdu_len + filler_len;

	uint8_t sca_sb_len = use_sca ? 16 : 0;

	uint8_t msg[] = {
		SMS_MESSAGE_SEND_REQ,
		mms,
		SMS_ROUTE_CS_PREF,
		0,	/* Is this a re-send? */
		SMS_SENDER_ANY,
		SMS_TYPE_TEXT_MESSAGE,
		1,	/* Sub blocks */
		SMS_GSM_TPDU,
		4 + tpdu_sb_len + sca_sb_len,
		0,	/* Filler */
		use_sca ? 2 : 1,	/* Sub-sub blocks */
		SMS_COMMON_DATA,
		tpdu_sb_len,
		tpdu_len,
		0,	/* Packing required? */
		/* TPDU */
	};

	static uint8_t filler[4];

	uint8_t sca_sb[16] = {
		SMS_ADDRESS,
		16,
		SMS_GSM_0411_ADDRESS,
		0,
	};

	struct iovec iov[4] = {
		{ msg, sizeof(msg) },
		{ tpdu, tpdu_len },
		{ filler, filler_len },
		{ sca_sb, sca_sb_len },
	};

	if (cbd == NULL || sd == NULL)
		goto error;

	if (use_sca) {
		sca_sb[3] = pdu_len - tpdu_len;
		memcpy(sca_sb + 4, pdu, sca_sb[3]);
	}

	/*
	 * Modem seems to time out SMS_MESSAGE_SEND_REQ in 5 seconds.
	 * Wait normal timeout plus the modem timeout.
	 */
	if (g_isi_client_vsend_with_timeout(sd->client, iov, 4,
				SMS_TIMEOUT + 5,
				submit_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static void isi_bearer_query(struct ofono_sms *sms,
				ofono_sms_bearer_query_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void isi_bearer_set(struct ofono_sms *sms, int bearer,
				ofono_sms_bearer_set_cb_t cb, void *data)
{
	DBG("Not implemented");
	CALLBACK_WITH_FAILURE(cb, data);
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

static void report_resp_cb(const GIsiMessage *msg, void *data)
{
	uint8_t cause;

	if (g_isi_msg_error(msg) < 0)
		return;

	if (g_isi_msg_id(msg) != SMS_GSM_RECEIVED_PP_REPORT_RESP)
		return;

	if (!g_isi_msg_data_get_byte(msg, 0, &cause))
		return;

	DBG("Report resp cause=0x%"PRIx8, cause);
}

static gboolean send_deliver_report(GIsiClient *client, gboolean success)
{
	uint8_t cause_type = !success ? SMS_CAUSE_TYPE_GSM : 0;
	uint8_t cause = !success ? SMS_GSM_ERR_MEMORY_CAPACITY_EXC : 0;

	uint8_t msg[] = {
		SMS_GSM_RECEIVED_PP_REPORT_REQ,
		cause_type,	/* Cause type */
		cause,		/* SMS cause */
		0, 0, 0,	/* Filler */
		1,		/* Sub blocks */
		SMS_GSM_DELIVER_REPORT,
		8,
		0,		/* Message parameters */
		0,		/* Cause type */
		0, 0, 0,	/* Filler */
		0,		/* Sub blocks */
	};
	size_t len = sizeof(msg);

	return g_isi_client_send(client, msg, len, report_resp_cb, NULL, NULL);
}

static gboolean parse_sms_address(GIsiSubBlockIter *iter, struct sms_addr *add)
{
	add->data = NULL;

	if (!g_isi_sb_iter_get_byte(iter, &add->type, 2))
		return FALSE;

	if (!g_isi_sb_iter_get_byte(iter, &add->len, 3))
		return FALSE;

	if (add->len == 0)
		return FALSE;

	if (!g_isi_sb_iter_get_struct(iter, (void **) &add->data, add->len, 4))
		return FALSE;

	return TRUE;
}

static gboolean parse_sms_tpdu(GIsiSubBlockIter *iter, struct sms_common *com)
{
	com->data = NULL;

	if (!g_isi_sb_iter_get_byte(iter, &com->len, 2))
		return FALSE;

	if (com->len == 0)
		return FALSE;

	if (!g_isi_sb_iter_get_struct(iter, (void **) &com->data, com->len, 4))
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

			if (!parse_sms_address(&iter, add))
				return FALSE;

			if (add->type != SMS_GSM_0411_ADDRESS)
				return FALSE;

			break;

		case SMS_COMMON_DATA:

			if (!parse_sms_tpdu(&iter, com))
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

	ofono_sms_deliver_notify(sms, pdu, tpdu.len + addr.len, tpdu.len);

	/*
	 * FIXME: We should not ack the DELIVER unless it has been
	 * reliably stored, i.e., written to disk. Currently, there is
	 * no such indication from core, so we just blindly trust that
	 * it did The Right Thing here.
	 */
	send_deliver_report(sd->client, TRUE);
}

static void routing_resp_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;
	struct sms_data *sd = ofono_sms_get_data(sms);

	if (!check_sms_status(msg, SMS_PP_ROUTING_RESP))
		return;

	g_isi_client_ntf_subscribe(sd->client, SMS_PP_ROUTING_NTF,
					routing_ntf_cb, sms);

	ofono_sms_register(sms);
}

static void sim_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;
	struct sms_data *sd = ofono_sms_get_data(sms);

	const uint8_t req[] = {
		SMS_PP_ROUTING_REQ,
		SMS_ROUTING_SET,
		0x01,  /* Sub-block count */
		SMS_GSM_ROUTING,
		0x08,  /* Sub-block length */
		SMS_GSM_TPDU_ROUTING,
		SMS_GSM_MT_ALL_TYPE,
		0x00, 0x00, 0x00,  /* Filler */
		0x00  /* Sub-sub-block count */
	};
	size_t len = sizeof(req);

	if (g_isi_msg_error(msg) < 0) {
		DBG("unable to find SIM resource");
		g_isi_client_destroy(sd->sim);
		sd->sim = NULL;
	}

	g_isi_client_ind_subscribe(sd->client, SMS_MESSAGE_SEND_STATUS_IND,
					send_status_ind_cb, sms);
	g_isi_client_send(sd->client, req, len, routing_resp_cb, sms, NULL);
}

static void sms_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_sms *sms = data;
	struct sms_data *sd = ofono_sms_get_data(sms);

	if (g_isi_msg_error(msg) < 0) {
		DBG("unable to find SMS resource");
		return;
	}

	ISI_VERSION_DBG(msg);

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

	g_isi_client_set_timeout(sd->client, SMS_TIMEOUT);
	g_isi_client_set_timeout(sd->sim, SIM_TIMEOUT);

	ofono_sms_set_data(sms, sd);

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

	const uint8_t msg[] = {
		SMS_PP_ROUTING_REQ,
		SMS_ROUTING_RELEASE,
		0x01,  /* Sub-block count */
		SMS_GSM_ROUTING,
		0x08,  /* Sub-block length */
		SMS_GSM_TPDU_ROUTING,
		SMS_GSM_MT_ALL_TYPE,
		0x00, 0x00, 0x00,  /* Filler */
		0x00  /* Sub-sub-block count */
	};

	ofono_sms_set_data(sms, NULL);

	if (sd == NULL)
		return;

	/*
	 * Send a promiscuous routing release, so as not to
	 * hog resources unnecessarily after being removed
	 */
	g_isi_client_send(sd->client, msg, sizeof(msg), NULL, NULL, NULL);
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
