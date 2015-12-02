/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>
#include <gril.h>
#include <parcel.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sms.h>
#include "smsutil.h"
#include "util.h"

#include "rilmodem.h"
#include "grilrequest.h"
#include "grilreply.h"
#include "grilunsol.h"

struct sms_data {
	GRil *ril;
	unsigned int vendor;
};

static void ril_csca_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_sca_set_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;

	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(sd->ril, message->req),
			ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_csca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;

	g_ril_request_set_smsc_address(sd->ril, sca, &rilp);

	/* Send request to RIL */
	if (g_ril_send(sd->ril, RIL_REQUEST_SET_SMSC_ADDRESS, &rilp,
			ril_csca_set_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, user_data);
	}
}

static void ril_csca_query_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_sca_query_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;
	struct ofono_phone_number *sca;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(sd->ril, message->req),
			ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	sca = g_ril_reply_parse_get_smsc_address(sd->ril, message);
	if (sca == NULL) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
	} else {
		CALLBACK_WITH_SUCCESS(cb, sca, cbd->data);
		g_free(sca);
	}
}

static void ril_csca_query(struct ofono_sms *sms, ofono_sms_sca_query_cb_t cb,
					void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);

	DBG("Sending csca_query");

	if (g_ril_send(sd->ril, RIL_REQUEST_GET_SMSC_ADDRESS, NULL,
			ril_csca_query_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, user_data);
	}
}

static void ril_submit_sms_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;
	int mr;

	if (message->error != RIL_E_SUCCESS) {
		CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
		return;
	}

	mr = g_ril_reply_parse_sms_response(sd->ril, message);
	CALLBACK_WITH_SUCCESS(cb, mr, cbd->data);
}

static void ril_cmgs(struct ofono_sms *sms, const unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;
	struct req_sms_cmgs req;

	DBG("pdu_len: %d, tpdu_len: %d mms: %d", pdu_len, tpdu_len, mms);

	/* TODO: if (mms) { ... } */

	req.pdu = pdu;
	req.pdu_len = pdu_len;
	req.tpdu_len = tpdu_len;

	g_ril_request_sms_cmgs(sd->ril, &req, &rilp);

	if (g_ril_send(sd->ril, RIL_REQUEST_SEND_SMS, &rilp,
			ril_submit_sms_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, user_data);
	}
}

static void ril_ack_delivery_cb(struct ril_msg *message, gpointer user_data)
{
	if (message->error != RIL_E_SUCCESS)
		ofono_error("SMS acknowledgement failed: "
				"Further SMS reception is not guaranteed");
}

static void ril_ack_delivery(struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct parcel rilp;

	g_ril_request_sms_acknowledge(sd->ril, &rilp);

	/* TODO: should ACK be sent for either of the error cases? */

	/* ACK the incoming NEW_SMS */
	g_ril_send(sd->ril, RIL_REQUEST_SMS_ACKNOWLEDGE, &rilp,
			ril_ack_delivery_cb, NULL, NULL);

}

static void ril_sms_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	unsigned int smsc_len;
	long ril_buf_len;
	struct unsol_sms_data *pdu_data;

	DBG("req: %d; data_len: %d", message->req, (int) message->buf_len);

	pdu_data = g_ril_unsol_parse_new_sms(sd->ril, message);
	if (pdu_data == NULL)
		goto error;

	/*
	 * The first octect in the pdu contains the SMSC address length
	 * which is the X following octects it reads. We add 1 octet to
	 * the read length to take into account this read octet in order
	 * to calculate the proper tpdu length.
	 */
	smsc_len = pdu_data->data[0] + 1;
	ril_buf_len = pdu_data->length;
	DBG("smsc_len is %d", smsc_len);

	if (message->req == RIL_UNSOL_RESPONSE_NEW_SMS)
		/* Last parameter is 'tpdu_len' ( substract SMSC length ) */
		ofono_sms_deliver_notify(sms, pdu_data->data,
						ril_buf_len,
						ril_buf_len - smsc_len);
	else if (message->req == RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT)
		ofono_sms_status_notify(sms, pdu_data->data, ril_buf_len,
						ril_buf_len - smsc_len);

	/* ACK the incoming NEW_SMS */
	ril_ack_delivery(sms);

	g_ril_unsol_free_sms_data(pdu_data);

error:
	;
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);

	DBG("");
	ofono_sms_register(sms);

	/* register to receive INCOMING_SMS and SMS status reports */
	g_ril_register(data->ril, RIL_UNSOL_RESPONSE_NEW_SMS,
			ril_sms_notify,	sms);
	g_ril_register(data->ril, RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
			ril_sms_notify, sms);

	/* This makes the delayed call a single-shot */
	return FALSE;
}

static int ril_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	GRil *ril = user;
	struct sms_data *data;

	data = g_new0(struct sms_data, 1);
	data->ril = g_ril_clone(ril);
	data->vendor = vendor;

	ofono_sms_set_data(sms, data);

	/*
	 * ofono_sms_register() needs to be called after
	 * the driver has been set in ofono_sms_create(), which
	 * calls this function.  Most other drivers make some
	 * kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle add instead.
	 */
	g_idle_add(ril_delayed_register, sms);

	return 0;
}

static void ril_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	DBG("");

	g_ril_unref(data->ril);
	g_free(data);

	ofono_sms_set_data(sms, NULL);
}

static struct ofono_sms_driver driver = {
	.name		= RILMODEM,
	.probe		= ril_sms_probe,
	.sca_query	= ril_csca_query,
	.sca_set	= ril_csca_set,
	.remove		= ril_sms_remove,
	.submit		= ril_cmgs,

	/*
	 * TODO: investigate/implement:
	 * .bearer_query  = NULL,
	 * .bearer_set	  = NULL,
	 */
};

void ril_sms_init(void)
{
	DBG("");
	if (ofono_sms_driver_register(&driver))
		DBG("ofono_sms_driver_register failed!");
}

void ril_sms_exit(void)
{
	DBG("");
	ofono_sms_driver_unregister(&driver);
}
