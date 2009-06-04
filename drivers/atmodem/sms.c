/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include "driver.h"
#include "smsutil.h"
#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "at.h"

static const char *csca_prefix[] = { "+CSCA:", NULL };
static const char *csms_prefix[] = { "+CSMS:", NULL };
static const char *cmgf_prefix[] = { "+CMGF:", NULL };
static const char *cpms_prefix[] = { "+CPMS:", NULL };
static const char *cnmi_prefix[] = { "+CNMI:", NULL };
static const char *none_prefix[] = { NULL };

static gboolean set_cmgf(gpointer user_data);
static gboolean set_cpms(gpointer user_data);

#define MAX_CMGF_RETRIES 10
#define MAX_CPMS_RETRIES 10

static const char *storages[] = {
	"SM",
	"ME",
};

#define SM_STORE 0
#define ME_STORE 1

struct sms_data {
	int store;
	int retries;
	gboolean cnma_enabled;
	char *cnma_ack_pdu;
	int cnma_ack_pdu_len;
};

static struct sms_data *sms_create()
{
	return g_try_new0(struct sms_data, 1);
}

static void sms_destroy(struct sms_data *data)
{
	if (data->cnma_ack_pdu)
		g_free(data->cnma_ack_pdu);

	g_free(data);
}

static void at_csca_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("csca_set_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_csca_set(struct ofono_modem *modem,
			const struct ofono_phone_number *sca,
			ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CSCA=\"%s\",%d", sca->number, sca->type);

	if (g_at_chat_send(at->parser, buf, csca_prefix,
				at_csca_set_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_csca_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sca_query_cb_t cb = cbd->cb;
	struct ofono_error error;
	struct ofono_phone_number sca;
	const char *number;

	dump_response("at_csca_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCA:"))
		goto err;

	if (!g_at_result_iter_next_string(&iter, &number))
		goto err;

	if (number[0] == '+') {
		number = number + 1;
		sca.type = 145;
	} else
		sca.type = 129;

	strncpy(sca.number, number, OFONO_MAX_PHONE_NUMBER_LENGTH);
	sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';

	g_at_result_iter_next_number(&iter, &sca.type);

	ofono_debug("csca_query_cb: %s, %d", sca.number, sca.type);

	cb(&error, &sca, cbd->data);

	return;

err:
	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, cbd->data);
	}
}

static void at_csca_query(struct ofono_modem *modem, ofono_sca_query_cb_t cb,
					void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CSCA?", csca_prefix,
				at_csca_query_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static struct ofono_sms_ops ops = {
	.sca_query	= at_csca_query,
	.sca_set	= at_csca_set,
};

static void at_cnma_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	if (!ok)
		ofono_error("CNMA acknowledgement failed: "
				"Further SMS reception is not guaranteed");
}

static gboolean at_parse_pdu_common(GAtResult *result, const char *prefix,
					const char **pdu, int *pdulen)
{
	GAtResultIter iter;

	if (!g_at_result_iter_next(&iter, prefix))
		return FALSE;

	if (!g_at_result_iter_skip_next(&iter))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, pdulen))
		return FALSE;

	*pdu = g_at_result_pdu(result);

	return TRUE;
}

static void at_cbm_notify(GAtResult *result, gpointer user_data)
{
	int pdulen;
	const char *pdu;

	dump_response("at_cbm_notify", TRUE, result);

	if (!at_parse_pdu_common(result, "+CBM:", &pdu, &pdulen)) {
		ofono_error("Unable to parse CBM notification");
		return;
	}

	ofono_debug("Got new Cell Broadcast via CBM: %s, %d", pdu, pdulen);
}

static void at_cds_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	int pdulen;
	const char *pdu;
	char buf[256];

	dump_response("at_cds_notify", TRUE, result);

	if (!at_parse_pdu_common(result, "+CDS:", &pdu, &pdulen)) {
		ofono_error("Unable to parse CDS notification");
		return;
	}

	ofono_debug("Got new Status-Report PDU via CDS: %s, %d", pdu, pdulen);

	/* We must acknowledge the PDU using CNMA */
	if (at->sms->cnma_ack_pdu)
		sprintf(buf, "AT+CNMA=1,%d\r%s", at->sms->cnma_ack_pdu_len,
				at->sms->cnma_ack_pdu);
	else
		sprintf(buf, "AT+CNMA=0"); /* Should be a safe fallback */

	g_at_chat_send(at->parser, buf, none_prefix, at_cnma_cb, NULL, NULL);
}

static void at_cmt_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	int pdulen;
	const char *pdu;
	char buf[256];

	dump_response("at_cmt_notify", TRUE, result);

	if (!at_parse_pdu_common(result, "+CMT:", &pdu, &pdulen)) {
		ofono_error("Unable to parse CMT notification");
		return;
	}

	ofono_debug("Got new SMS Deliver PDU via CMT: %s, %d", pdu, pdulen);

	/* We must acknowledge the PDU using CNMA */
	if (at->sms->cnma_ack_pdu)
		sprintf(buf, "AT+CNMA=1,%d\r%s", at->sms->cnma_ack_pdu_len,
				at->sms->cnma_ack_pdu);
	else
		sprintf(buf, "AT+CNMA=0"); /* Should be a safe fallback */

	g_at_chat_send(at->parser, buf, none_prefix, at_cnma_cb, NULL, NULL);
}

static void at_cmgr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	//struct ofono_modem *modem = user_data;
	GAtResultIter iter;
	int pdulen;
	const char *pdu;

	dump_response("at_cmgr_cb", ok, result);

	if (!ok) {
		ofono_error("Received a CMTI indication but CMGR failed!");
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CMGR:"))
		goto err;

	if (!g_at_result_iter_skip_next(&iter))
		goto err;

	if (!g_at_result_iter_skip_next(&iter))
		goto err;

	if (!g_at_result_iter_next_number(&iter, &pdulen))
		goto err;

	if (!g_at_result_iter_next(&iter, NULL))
		goto err;

	pdu = g_at_result_iter_raw_line(&iter);

	ofono_debug("Got PDU: %s, with len: %d", pdu, pdulen);
	return;

err:
	ofono_error("Unable to parse CMGR response");
}

static void at_cmgd_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	if (!ok)
		ofono_error("Unable to delete received SMS");
}

static void at_cmti_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	GAtResultIter iter;
	int index;
	char buf[128];

	dump_response("at_cmti_notify", TRUE, result);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CMTI:"))
		goto err;

	/* We skip the memory store here as we assume the modem will honor
	 * the CPMS we sent out earlier
	 */
	if (!g_at_result_iter_skip_next(&iter))
		goto err;

	if (!g_at_result_iter_next_number(&iter, &index))
		goto err;

	ofono_debug("Got a CMTI indication at index: %d", index);

	sprintf(buf, "AT+CMGR=%d", index);

	/* Can't use a prefix here since a PDU is expected on the next line */
	g_at_chat_send(at->parser, buf, NULL, at_cmgr_cb, modem, NULL);

	sprintf(buf, "AT+CMGD=%d", index);

	/* We don't buffer SMS on the SIM/ME, send along a CMGD as well */
	g_at_chat_send(at->parser, buf, none_prefix, at_cmgd_cb, NULL, NULL);

	return;

err:
	ofono_error("Unable to parse CMTI notification");
}

static void at_sms_initialized(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	g_at_chat_register(at->parser, "+CMTI:", at_cmti_notify, FALSE,
				modem, NULL);
	g_at_chat_register(at->parser, "+CMT:", at_cmt_notify, TRUE,
				modem, NULL);
	g_at_chat_register(at->parser, "+CDS:", at_cds_notify, TRUE,
				modem, NULL);
	g_at_chat_register(at->parser, "+CBM:", at_cbm_notify, TRUE,
				modem, NULL);

	ofono_sms_manager_register(modem, &ops);
}

static void at_sms_not_supported(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	ofono_error("SMS not supported by this modem.  If this is in error"
			" please submit patches to support this hardware");
	if (at->sms) {
		sms_destroy(at->sms);
		at->sms = NULL;
	}
}

static void at_cnmi_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	if (!ok)
		return at_sms_not_supported(modem);

	at_sms_initialized(modem);
}

static inline char wanted_cnmi(int supported, const char *pref)
{
	while (*pref) {
		if (supported & (1 << (*pref - '0')))
			return *pref;

		pref++;
	}

	return '\0';
}

static inline gboolean append_cnmi_element(char *buf, int *len, int cap,
						const char *wanted,
						gboolean last)
{
	char setting = wanted_cnmi(cap, wanted);

	if (!setting)
		return FALSE;

	buf[*len] = setting;

	if (last)
		buf[*len + 1] = '\0';
	else
		buf[*len + 1] = ',';

	*len += 2;

	return TRUE;
}

static gboolean build_cnmi_string(char *buf, int *cnmi_opts,
					gboolean cnma_enabled)
{
	int len = sprintf(buf, "AT+CNMI=");

	/* Mode doesn't matter, but sounds like 2 is the sanest option */
	if (!append_cnmi_element(buf, &len, cnmi_opts[0], "2310", FALSE))
		return FALSE;

	/* Prefer to deliver SMS via +CMT if CNMA is supported */
	if (!append_cnmi_element(buf, &len, cnmi_opts[1],
					cnma_enabled ? "21" : "1", FALSE))
		return FALSE;

	/* Always deliver CB via +CBM, otherwise don't deliver at all */
	if (!append_cnmi_element(buf, &len, cnmi_opts[2], "20", FALSE))
		return FALSE;

	/* Always deliver Status-Reports via +CDS or don't deliver at all */
	if (!append_cnmi_element(buf, &len, cnmi_opts[3], "10", FALSE))
		return FALSE;

	/* Don't care about buffering, 0 seems safer */
	if (!append_cnmi_element(buf, &len, cnmi_opts[4], "01", TRUE))
		return FALSE;

	return TRUE;
}

static void construct_ack_pdu(struct sms_data *d)
{
	struct sms ackpdu;
	unsigned char pdu[164];
	int len;
	int tpdu_len;

	memset(&ackpdu, 0, sizeof(ackpdu));

	ackpdu.type = SMS_TYPE_DELIVER_REPORT_ACK;

	if (!encode_sms(&ackpdu, &len, &tpdu_len, pdu))
		goto err;

	/* Constructing an <ackpdu> according to 27.005 Section 4.6 */
	if (len != tpdu_len)
		goto err;

	d->cnma_ack_pdu = encode_hex(pdu, tpdu_len, 0);

	if (!d->cnma_ack_pdu)
		goto err;

	d->cnma_ack_pdu_len = tpdu_len;
	return;

err:
	ofono_error("Unable to construct Deliver ACK PDU");
}

static void at_cnmi_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	GAtResultIter iter;
	int cnmi_opts[5]; /* See 27.005 Section 3.4.1 */
	int opt;
	int mode;
	gboolean supported = FALSE;
	char buf[128];

	if (!ok)
		goto out;

	memset(cnmi_opts, 0, sizeof(cnmi_opts));

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CNMI:"))
		goto out;

	for (opt = 0; opt < 5; opt++) {
		if (!g_at_result_iter_open_list(&iter))
			goto out;

		while (g_at_result_iter_next_number(&iter, &mode))
			cnmi_opts[opt] |= 1 << mode;

		if (!g_at_result_iter_close_list(&iter))
			goto out;
	}

	if (build_cnmi_string(buf, cnmi_opts, at->sms->cnma_enabled))
		supported = TRUE;

	if (at->sms->cnma_enabled)
		construct_ack_pdu(at->sms);

out:
	if (!supported)
		return at_sms_not_supported(modem);

	g_at_chat_send(at->parser, buf, cnmi_prefix,
			at_cnmi_set_cb, modem, NULL);
}

static void at_query_cnmi(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	g_at_chat_send(at->parser, "AT+CNMI=?", cnmi_prefix,
			at_cnmi_query_cb, modem, NULL);
}

static void at_cpms_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);

	if (ok)
		return at_query_cnmi(modem);

	at->sms->retries += 1;

	if (at->sms->retries == MAX_CPMS_RETRIES) {
		ofono_error("Unable to set preferred storage");
		return at_sms_not_supported(modem);
	}

	g_timeout_add_seconds(1, set_cpms, modem);
}

static gboolean set_cpms(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	const char *store = storages[at->sms->store];
	char buf[128];

	sprintf(buf, "AT+CPMS=\"%s\",\"%s\",\"%s\"", store, store, store);

	g_at_chat_send(at->parser, buf, cpms_prefix,
			at_cpms_set_cb, modem, NULL);
	return FALSE;
}

static void at_cmgf_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);

	if (ok) {
		at->sms->retries = 0;
		set_cpms(modem);
		return;
	}

	at->sms->retries += 1;

	if (at->sms->retries == MAX_CMGF_RETRIES) {
		ofono_debug("Unable to enter PDU mode");
		return at_sms_not_supported(modem);
	}

	g_timeout_add_seconds(1, set_cmgf, modem);
}

static gboolean set_cmgf(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);

	g_at_chat_send(at->parser, "AT+CMGF=0", cmgf_prefix,
			at_cmgf_set_cb, modem, NULL);
	return FALSE;
}

static void at_cpms_query_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	gboolean supported = FALSE;

	if (ok) {
		int mem = 0;
		GAtResultIter iter;
		const char *store;
		gboolean me_supported[3];
		gboolean sm_supported[3];

		memset(me_supported, 0, sizeof(me_supported));
		memset(sm_supported, 0, sizeof(sm_supported));

		g_at_result_iter_init(&iter, result);

		if (!g_at_result_iter_next(&iter, "+CPMS:"))
			goto out;

		for (mem = 0; mem < 3; mem++) {
			if (!g_at_result_iter_open_list(&iter))
				goto out;

			while (g_at_result_iter_next_string(&iter, &store)) {
				if (!strcmp(store, "ME"))
					me_supported[mem] = TRUE;
				else if (!strcmp(store, "SM"))
					sm_supported[mem] = TRUE;
			}

			if (!g_at_result_iter_close_list(&iter))
				goto out;
		}

		if (me_supported[0] && me_supported[1] && me_supported[2]) {
			supported = TRUE;
			at->sms->store = ME_STORE;
		}

		if (sm_supported[0] && sm_supported[1] && sm_supported[2]) {
			supported = TRUE;
			at->sms->store = SM_STORE;
		}
	}
out:
	if (!supported)
		return at_sms_not_supported(modem);

	set_cmgf(modem);
}

static void at_cmgf_query_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	gboolean supported = FALSE;

	if (ok) {
		GAtResultIter iter;
		int mode;

		g_at_result_iter_init(&iter, result);

		if (!g_at_result_iter_next(&iter, "+CMGF:"))
			goto out;

		if (!g_at_result_iter_open_list(&iter))
			goto out;

		while (g_at_result_iter_next_number(&iter, &mode))
			if (mode == 1)
				supported = TRUE;
	}

out:
	if (!supported)
		return at_sms_not_supported(modem);

	g_at_chat_send(at->parser, "AT+CPMS=?", cpms_prefix,
			at_cpms_query_cb, modem, NULL);
}

static void at_csms_status_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	gboolean supported = FALSE;

	if (ok) {
		GAtResultIter iter;
		int service;
		int mt;
		int mo;

		g_at_result_iter_init(&iter, result);

		if (!g_at_result_iter_next(&iter, "+CSMS:"))
			goto out;

		if (!g_at_result_iter_next_number(&iter, &service))
			goto out;

		if (!g_at_result_iter_next_number(&iter, &mt))
			goto out;

		if (!g_at_result_iter_next_number(&iter, &mo))
			goto out;

		if (service == 1)
			at->sms->cnma_enabled = TRUE;

		if (mt == 1 && mo == 1)
			supported = TRUE;
	}

out:
	if (!supported)
		return at_sms_not_supported(modem);

	/* Now query supported text format */
	g_at_chat_send(at->parser, "AT+CMGF=?", cmgf_prefix,
			at_cmgf_query_cb, modem, NULL);
}

static void at_csms_set_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);

	g_at_chat_send(at->parser, "AT+CSMS?", csms_prefix,
			at_csms_status_cb, modem, NULL);
}

static void at_csms_query_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	gboolean cnma_supported = FALSE;
	GAtResultIter iter;
	int status;
	char buf[128];

	if (!ok)
		return at_sms_not_supported(modem);

	at->sms = sms_create();

	if (!at->sms)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSMS:"))
		goto out;

	if (!g_at_result_iter_open_list(&iter))
		goto out;

	while (g_at_result_iter_next_number(&iter, &status))
		if (status == 1)
			cnma_supported = TRUE;

	ofono_debug("CSMS query parsed successfully");

out:
	sprintf(buf, "AT+CSMS=%d", cnma_supported ? 1 : 0);
	g_at_chat_send(at->parser, buf, csms_prefix,
			at_csms_set_cb, modem, NULL);
}

void at_sms_init(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	g_at_chat_send(at->parser, "AT+CSMS=?", csms_prefix,
			at_csms_query_cb, modem, NULL);
}

void at_sms_exit(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	if (!at->sms)
		return;

	ofono_sms_manager_unregister(modem);
}
