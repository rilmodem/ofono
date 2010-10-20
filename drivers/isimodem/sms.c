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

struct sms_data {
	GIsiClient *client;
	GIsiClient *sim;
	/* This is a straightforward copy of the EF_smsp structure */
	struct sim_parameters {
		uint8_t      absent;
		uint8_t      tp_pid;
		uint8_t      tp_dcs;
		uint8_t      tp_vp;
		uint8_t      dst[12];
		uint8_t      sca[12];
		uint8_t      alphalen;
		uint8_t      filler[3];
		uint16_t     alpha[17];
	} params;
};

static gboolean sca_query_resp_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const uint8_t *msg = data;
	struct isi_cb_data *cbd = opaque;
	struct ofono_sms *sms = cbd->user;
	struct sms_data *sd = ofono_sms_get_data(sms);
	ofono_sms_sca_query_cb_t cb = cbd->cb;

	struct ofono_phone_number sca;
	uint8_t bcd_len;

	/* Nicely aligned. */
	struct {
		uint8_t      status;
		uint8_t      absent;
		uint8_t      tp_pid;
		uint8_t      tp_dcs;
		uint8_t      dst[12];
		uint8_t      sca[12];
		uint8_t      tp_vp;
		uint8_t      alphalen;
		uint8_t      filler[2];
		uint16_t     alpha[17];
	} params;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 31 || msg[0] != SIM_SMS_RESP || msg[1] != READ_PARAMETER)
		return FALSE;

	if (msg[3] != SIM_SERV_OK)
		goto error;

	memset(&params, 0, sizeof(params));
	if (len  > 3 + sizeof(params))
		len = 3 + sizeof(params);
	memcpy(&params, msg + 3, len - 3);

	if (params.alphalen > 17)
		params.alphalen = 17;
	else if (params.alphalen < 1)
		params.alphalen = 1;
	params.alpha[params.alphalen - 1] = '\0';

	sd->params.absent = params.absent;
	sd->params.tp_pid = params.tp_pid;
	sd->params.tp_dcs = params.tp_dcs;
	sd->params.tp_vp = params.tp_vp;
	memcpy(sd->params.dst, params.dst, sizeof(sd->params.dst));
	memcpy(sd->params.sca, params.sca, sizeof(sd->params.sca));
	sd->params.alphalen = params.alphalen;
	memcpy(sd->params.alpha, params.alpha, sizeof(sd->params.alpha));

	/*
	 * Bitmask indicating absense of parameters --
	 * If second bit is set it indicates that the SCA is absent
	 */
	if (params.absent & 0x2)
		goto error;

	bcd_len = params.sca[0];

	if (bcd_len <= 1 || bcd_len > 12)
		goto error;

	extract_bcd_number(params.sca + 2, bcd_len - 1, sca.number);
	sca.type = 0x80 | params.sca[1];

	CALLBACK_WITH_SUCCESS(cb, &sca, cbd->data);
	return TRUE;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
	return TRUE;
}

static void isi_sca_query(struct ofono_sms *sms,
				ofono_sms_sca_query_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct isi_cb_data *cbd = isi_cb_data_new(sms, cb, data);

	uint8_t msg[] = {
		SIM_SMS_REQ,
		READ_PARAMETER,
		1,	/* Location, default is 1 */
	};

	if (!cbd || !sd)
		goto error;

	if (g_isi_send(sd->sim, msg, sizeof(msg), SIM_TIMEOUT,
			sca_query_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, data);
	g_free(cbd);
}

static gboolean sca_set_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const uint8_t *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_sms_sca_set_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SIM_SMS_RESP || msg[1] != UPDATE_PARAMETER)
		return FALSE;

	if (msg[2] != SIM_SERV_OK)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return TRUE;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
	return TRUE;
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

	struct iovec iov[] = {
		{ msg, sizeof(msg) },
		{ &sd->params, sizeof(sd->params) },
	};

	if (!cbd || !sd)
		goto error;

	bcd = sd->params.sca;
	sd->params.absent &= ~0x02;

	encode_bcd_number(sca->number, bcd + 2);
	bcd[0] = 1 + (strlen(sca->number) + 1) / 2;
	bcd[1] = sca->type & 0xFF;

	if (g_isi_vsend(sd->sim, iov, G_N_ELEMENTS(iov), SIM_TIMEOUT,
			sca_set_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static gboolean submit_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const uint8_t *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_sms_submit_cb_t cb = cbd->cb;

	int mr = -1;
	GIsiSubBlockIter iter;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SMS_MESSAGE_SEND_RESP)
		return FALSE;

	for (g_isi_sb_iter_init(&iter, msg, len, 3);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		uint8_t type;
		uint8_t cause;
		uint8_t ref;

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SMS_GSM_REPORT:

			if (!g_isi_sb_iter_get_byte(&iter, &type, 2)
				|| !g_isi_sb_iter_get_byte(&iter, &cause, 3)
				|| !g_isi_sb_iter_get_byte(&iter, &ref, 4))
				goto error;

			if (cause != 0) {
				DBG("Submit error: 0x%"PRIx8" (type 0x%"PRIx8")",
					cause, type);
				goto error;
			}

			DBG("cause=0x%"PRIx8", type 0x%"PRIx8", mr=0x%"PRIx8,
					cause, type, ref);

			mr = (int)ref;
			break;

		default:
			DBG("skipped sub-block: %s (%zu bytes)",
				sms_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));

		}
	}

	if (mr == -1)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, mr, cbd->data);
	return TRUE;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	return TRUE;
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

	if (!cbd || !sd)
		goto error;

	if (use_sca) {
		sca_sb[3] = pdu_len - tpdu_len;
		memcpy(sca_sb + 4, pdu, sca_sb[3]);
	}

	/* Modem seems to time out SMS_MESSAGE_SEND_REQ in 5 seconds */
	/* Wait normal timeout plus the modem timeout */
	if (g_isi_vsend(sd->client, iov, G_N_ELEMENTS(iov), SMS_TIMEOUT + 5,
			submit_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static void send_status_ind_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const uint8_t *msg = data;

	if (!msg || len < 6 || msg[0] != SMS_MESSAGE_SEND_STATUS_IND)
		return;

	DBG("status=0x%"PRIx8", mr=0x%"PRIx8", route=0x%"PRIx8
		", cseg=0x%"PRIx8", tseg=0x%"PRIx8,
		msg[1], msg[2], msg[3], msg[4], msg[5]);

	DBG("TODO: Status notification");
}

static gboolean report_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const uint8_t *msg = data;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return TRUE;
	}

	if (len < 3 || msg[0] != SMS_GSM_RECEIVED_PP_REPORT_RESP)
		return FALSE;

	DBG("Report resp cause=0x%"PRIx8, msg[1]);

	return TRUE;
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

	return g_isi_send(client, msg, sizeof(msg), SMS_TIMEOUT,
				report_resp_cb, NULL, NULL) != NULL;
}

static void routing_ntf_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const uint8_t *msg = data;
	struct ofono_sms *sms = opaque;
	GIsiSubBlockIter iter;

	uint8_t *sca = NULL;
	uint8_t sca_len = 0;
	uint8_t *tpdu = NULL;
	uint8_t tpdu_len = 0;

	unsigned char pdu[176];

	if (!msg || len < 7 || msg[0] != SMS_PP_ROUTING_NTF
		|| msg[3] != SMS_GSM_TPDU)
		return;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		uint8_t type;
		void *data;
		uint8_t data_len;

		case SMS_ADDRESS:

			if (!g_isi_sb_iter_get_byte(&iter, &type, 2)
				|| !g_isi_sb_iter_get_byte(&iter, &data_len, 3)
				|| !g_isi_sb_iter_get_data(&iter, &data, 4)
				|| type != SMS_GSM_0411_ADDRESS)
				break;

			sca = data;
			sca_len = data_len;
			break;

		case SMS_COMMON_DATA:

			if (!g_isi_sb_iter_get_byte(&iter, &data_len, 2)
				|| !g_isi_sb_iter_get_data(&iter, &data, 4))
				break;

			tpdu = data;
			tpdu_len = data_len;
			break;

		default:
			DBG("skipped sub-block: %s (%zu bytes)",
				sms_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
		}
	}

	if (!tpdu || !sca || tpdu_len + sca_len > sizeof(pdu))
		return;

	memcpy(pdu, sca, sca_len);
	memcpy(pdu + sca_len, tpdu, tpdu_len);

	ofono_sms_deliver_notify(sms, pdu, tpdu_len + sca_len, tpdu_len);

	/* FIXME: We should not ack the DELIVER unless it has been
	 * reliably stored, i.e., written to disk. Currently, there is
	 * no such indication from core, so we just blindly trust that
	 * it did The Right Thing here. */
	send_deliver_report(client, TRUE);
}

static gboolean routing_resp_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct ofono_sms *sms = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SMS_PP_ROUTING_RESP)
		goto error;

	if (msg[1] != SMS_OK) {

		if (msg[1] == SMS_ERR_PP_RESERVED) {
			DBG("Request failed: 0x%02"PRIx8" (%s).\n\n  "
				"Unable to bootstrap SMS routing.\n  "
				"It appears some other component is "
				"already\n  registered as the SMS "
				"routing endpoint.\n  As a consequence, "
				"receiving SMSs is NOT going to work.\n  "
				"On the other hand, sending might work.\n\n",
				msg[1], sms_isi_cause_name(msg[1]));
			ofono_sms_register(sms);
		}
		return TRUE;
	}

	g_isi_subscribe(client, SMS_PP_ROUTING_NTF, routing_ntf_cb, sms);

	ofono_sms_register(sms);
	return TRUE;

error:
	DBG("Unable to bootstrap SMS routing.");
	return TRUE;
}

static int isi_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct sms_data *data = g_try_new0(struct sms_data, 1);
	const char *debug;

	const unsigned char msg[] = {
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

	if (!data)
		return -ENOMEM;

	data->params.absent = 0xff;
	data->params.alphalen = 1; /* Includes final UCS2-coded NUL */

	data->client = g_isi_client_create(idx, PN_SMS);
	if (!data->client)
		return -ENOMEM;

	data->sim = g_isi_client_create(idx, PN_SIM);
	if (!data->sim) {
		g_isi_client_destroy(data->client);
		return -ENOMEM;
	}

	ofono_sms_set_data(sms, data);

	debug = getenv("OFONO_ISI_DEBUG");
	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "sms") == 0)) {
		g_isi_client_set_debug(data->client, sms_debug, NULL);
		g_isi_client_set_debug(data->sim, sim_debug, NULL);
	}

	g_isi_subscribe(data->client, SMS_MESSAGE_SEND_STATUS_IND,
			send_status_ind_cb, sms);
	if (!g_isi_send(data->client, msg, sizeof(msg), SMS_TIMEOUT,
			routing_resp_cb, sms, NULL))
		DBG("Failed to set SMS routing.");

	return 0;
}

static void isi_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	const unsigned char msg[] = {
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

	if (!data)
		return;

	ofono_sms_set_data(sms, NULL);

	/*
	 * Send a promiscuous routing release, so as not to
	 * hog resources unnecessarily after being removed
	 */
	g_isi_send(data->client, msg, sizeof(msg),
			SMS_TIMEOUT, NULL, NULL, NULL);
	g_isi_client_destroy(data->client);
	g_isi_client_destroy(data->sim);
	g_free(data);
}

static struct ofono_sms_driver driver = {
	.name			= "isimodem",
	.probe			= isi_sms_probe,
	.remove			= isi_sms_remove,
	.sca_query		= isi_sca_query,
	.sca_set		= isi_sca_set,
	.submit			= isi_submit
};

void isi_sms_init()
{
	ofono_sms_driver_register(&driver);
}

void isi_sms_exit()
{
	ofono_sms_driver_unregister(&driver);
}
