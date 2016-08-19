/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2015 Canonical Ltd.
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
#include <assert.h>
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <ofono/modem.h>
#include <ofono/types.h>
#include <ofono/sms.h>
#include <gril.h>

#include "common.h"
#include "ril_constants.h"
#include "rilmodem-test-server.h"

struct rilmodem_sms_data {
	GRil *ril;
	struct ofono_modem *modem;
	gconstpointer test_data;
	struct ofono_sms *sms;
	struct server_data *serverd;
};

typedef gboolean (*StartFunc)(gpointer data);

struct sms_data {
	StartFunc start_func;

	const unsigned char *pdu;
	gint pdu_len;
	gint tpdu_len;
	gint mms;

	struct rilmodem_test_data rtd;
	enum ofono_error_type error_type;

	const struct ofono_phone_number ph;
	gint mr;
};

static GMainLoop *mainloop;

static const struct ofono_sms_driver *smsdriver;

/* Declarations && Re-implementations of core functions. */
void ril_sms_exit(void);
void ril_sms_init(void);

struct ofono_sms {
	void *driver_data;
	const struct sms_data *sd;
};

struct ofono_sms *ofono_sms_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct rilmodem_sms_data *rsd = data;
	struct ofono_sms *sms = g_new0(struct ofono_sms, 1);
	int retval;

	retval = smsdriver->probe(sms, OFONO_RIL_VENDOR_AOSP, rsd->ril);
	g_assert(retval == 0);

	return sms;
}

int ofono_sms_driver_register(const struct ofono_sms_driver *d)
{
	if (smsdriver == NULL)
		smsdriver = d;

	return 0;
}

void ofono_sms_set_data(struct ofono_sms *sms, void *data)
{
	sms->driver_data = data;
}

void *ofono_sms_get_data(struct ofono_sms *sms)
{
	return sms->driver_data;
}

void ofono_sms_register(struct ofono_sms *sms)
{
}

void ofono_sms_driver_unregister(const struct ofono_sms_driver *d)
{
}

void ofono_sms_deliver_notify(struct ofono_sms *sms, const unsigned char *pdu,
							int len, int tpdu_len)
{
	g_assert(sms->sd->pdu_len == len);
	g_assert(sms->sd->tpdu_len == tpdu_len);
	g_assert(!memcmp(pdu, sms->sd->pdu, len));

	g_main_loop_quit(mainloop);
}

void ofono_sms_status_notify(struct ofono_sms *sms, const unsigned char *pdu,
							int len, int tpdu_len)
{
	ofono_sms_deliver_notify(sms, pdu, len, tpdu_len);
}

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

static void sca_query_callback(const struct ofono_error *error,
					const struct ofono_phone_number *ph,
					gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	g_assert(error->type == sd->error_type);

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		g_assert(ph->type == sd->ph.type);
		g_assert(strcmp(ph->number, sd->ph.number) == 0);
	}

	g_main_loop_quit(mainloop);
}

static void sca_set_callback(const struct ofono_error *error, gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	g_assert(error->type == sd->error_type);

	g_main_loop_quit(mainloop);
}

static void submit_callback(const struct ofono_error *error, int mr,
								gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	g_assert(error->type == sd->error_type);
	g_assert(mr == sd->mr);

	g_main_loop_quit(mainloop);
}

static gboolean trigger_sca_query(gpointer data)
{
	struct rilmodem_sms_data *rsd = data;

	g_assert(smsdriver->sca_query != NULL);
	smsdriver->sca_query(rsd->sms, sca_query_callback, rsd);

	return FALSE;
}

static gboolean trigger_sca_set(gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	g_assert(smsdriver->sca_set != NULL);
	smsdriver->sca_set(rsd->sms, &sd->ph, sca_set_callback, rsd);

	return FALSE;
}

static gboolean trigger_submit(gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	g_assert(smsdriver->submit != NULL);

	smsdriver->submit(rsd->sms, sd->pdu, sd->pdu_len, sd->tpdu_len,
						sd->mms, submit_callback, rsd);

	return FALSE;
}

static gboolean trigger_new_sms(gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	rilmodem_test_server_write(rsd->serverd, sd->rtd.req_data,
							sd->rtd.req_size);

	return FALSE;
}

/* RIL_REQUEST_GET_SMSC_ADDRESS */
static const guchar req_get_smsc_address_parcel_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};


/*
 * RIL_REQUEST_GET_SMSC_ADDRESS reply with the following data:
 *
 * {number="+34607003110"}
 */
static const guchar rsp_get_smsc_address_data_1[] = {
	0x0d, 0x00, 0x00, 0x00, 0x22, 0x00, 0x2b, 0x00, 0x33, 0x00, 0x34, 0x00,
	0x36, 0x00, 0x30, 0x00, 0x37, 0x00, 0x30, 0x00, 0x30, 0x00, 0x33, 0x00,
	0x31, 0x00, 0x31, 0x00, 0x30, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct sms_data testdata_sca_query_valid_1 = {
	.start_func = trigger_sca_query,
	.rtd = {
		.req_data = req_get_smsc_address_parcel_1,
		.req_size = sizeof(req_get_smsc_address_parcel_1),
		.rsp_data = rsp_get_smsc_address_data_1,
		.rsp_size = sizeof(rsp_get_smsc_address_data_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.ph = { .number = "34607003110", .type = 145 },
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/*
 * RIL_REQUEST_GET_SMSC_ADDRESS reply with no data, which should
 * trigger a callback failure.
 */
static const struct sms_data testdata_sca_query_invalid_1 = {
	.start_func = trigger_sca_query,
	.rtd = {
		.req_data = req_get_smsc_address_parcel_1,
		.req_size = sizeof(req_get_smsc_address_parcel_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/*
 * RIL_REQUEST_GET_SMSC_ADDRESS reply with no quotes found which
 * should trigger a callback failure.
 */
static const guchar rsp_get_smsc_address_data_3[] = {
	0x02, 0x00, 0x00, 0x00, 0x22, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct sms_data testdata_sca_query_invalid_2 = {
	.start_func = trigger_sca_query,
	.ph = { .number = "34607003110", .type = 145 },
	.rtd = {
		.req_data = req_get_smsc_address_parcel_1,
		.req_size = sizeof(req_get_smsc_address_parcel_1),
		.rsp_data = rsp_get_smsc_address_data_3,
		.rsp_size = sizeof(rsp_get_smsc_address_data_3),
		.rsp_error = RIL_E_SUCCESS,
	},
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/* GENERIC_FAILURE returned in RIL reply */
static const struct sms_data testdata_sca_query_invalid_3 = {
	.start_func = trigger_sca_query,
	.rtd = {
		.req_data = req_get_smsc_address_parcel_1,
		.req_size = sizeof(req_get_smsc_address_parcel_1),
		.rsp_error = RIL_E_GENERIC_FAILURE,
	},
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/*
 * RIL_REQUEST_SET_SMSC_ADDRESS with the following data:
 *
 * {number="+34607003110"}
 */
static const guchar req_set_smsc_address_parcel_1[] = {
	+0x00, 0x00, 0x00, 0x2C, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	+0x0e, 0x00, 0x00, 0x00, 0x22, 0x00, 0x2b, 0x00, 0x33, 0x00, 0x34, 0x00,
	+0x36, 0x00, 0x30, 0x00, 0x37, 0x00, 0x30, 0x00, 0x30, 0x00, 0x33, 0x00,
	+0x31, 0x00, 0x31, 0x00, 0x30, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct sms_data testdata_sca_set_valid_1 = {
	.start_func = trigger_sca_set,
	.ph = { .number = "34607003110", .type = 145 },
	.rtd = {
		.req_data = req_set_smsc_address_parcel_1,
		.req_size = sizeof(req_set_smsc_address_parcel_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* GENERIC_FAILURE returned in RIL reply */
static const struct sms_data testdata_sca_set_invalid_1 = {
	.start_func = trigger_sca_set,
	.ph = { .number = "34607003110", .type = 145 },
	.rtd = {
		.req_data = req_set_smsc_address_parcel_1,
		.req_size = sizeof(req_set_smsc_address_parcel_1),
		.rsp_error = RIL_E_GENERIC_FAILURE,
	},
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

static const unsigned char req_send_sms_pdu_valid_1[] = {
	0x00, 0x11, 0x00, 0x09, 0x81, 0x36, 0x54, 0x39, 0x80, 0xf5, 0x00, 0x00,
	0xa7, 0x0a, 0xc8, 0x37, 0x3b, 0x0c, 0x6a, 0xd7, 0xdd, 0xe4, 0x37
};

static const guchar req_send_sms_parcel_1[] = {
	0x00, 0x00, 0x00, 0x70, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x2c, 0x00, 0x00, 0x00,
	0x31, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x39, 0x00,
	0x38, 0x00, 0x31, 0x00, 0x33, 0x00, 0x36, 0x00, 0x35, 0x00, 0x34, 0x00,
	0x33, 0x00, 0x39, 0x00, 0x38, 0x00, 0x30, 0x00, 0x46, 0x00, 0x35, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x41, 0x00, 0x37, 0x00,
	0x30, 0x00, 0x41, 0x00, 0x43, 0x00, 0x38, 0x00, 0x33, 0x00, 0x37, 0x00,
	0x33, 0x00, 0x42, 0x00, 0x30, 0x00, 0x43, 0x00, 0x36, 0x00, 0x41, 0x00,
	0x44, 0x00, 0x37, 0x00, 0x44, 0x00, 0x44, 0x00, 0x45, 0x00, 0x34, 0x00,
	0x33, 0x00, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * SEND_SMS reply with the following data:
 *
 * messageRef=1
 * ackPDU=NULL
 * errorCode=0
 */
static const guchar rsp_send_sms_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	0x00, 0x00, 0x00, 0x00
};

static const struct sms_data testdata_submit_valid_1 = {
	.start_func = trigger_submit,
	.pdu = req_send_sms_pdu_valid_1,
	.pdu_len = sizeof(req_send_sms_pdu_valid_1),
	.tpdu_len = sizeof(req_send_sms_pdu_valid_1) - 1,
	.mms = 0,
	.rtd = {
		.req_data = req_send_sms_parcel_1,
		.req_size = sizeof(req_send_sms_parcel_1),
		.rsp_data = rsp_send_sms_valid_1,
		.rsp_size = sizeof(rsp_send_sms_valid_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.mr = 1,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/*
 * SEND_SMS reply with failure indicated
 */
static const struct sms_data testdata_submit_invalid_1 = {
	.start_func = trigger_submit,
	.pdu = req_send_sms_pdu_valid_1,
	.pdu_len = sizeof(req_send_sms_pdu_valid_1),
	.tpdu_len = sizeof(req_send_sms_pdu_valid_1) - 1,
	.mms = 0,
	.rtd = {
		.req_data = req_send_sms_parcel_1,
		.req_size = sizeof(req_send_sms_parcel_1),
		.rsp_error = RIL_E_GENERIC_FAILURE,
	},
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/*
 * The following hexadecimal data represents a serialized Binder parcel
 * instance containing a valid RIL_UNSOL_RESPONSE_NEW_SMS message
 * with the following parameter (SMSC address length is 7):
 *
 * {07914306073011F0040B914336543980F50000310113212002400AC8373B0C6AD7DDE437}
 * {069143060730F0040B914336543980F50000310113212002400AC8373B0C6AD7DDE437}
 */
static const guchar unsol_response_new_sms_parcel_1[] = {
	0x00, 0x00, 0x00, 0xA0, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x03, 0x00, 0x00,
	0x48, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00, 0x39, 0x00, 0x31, 0x00,
	0x34, 0x00, 0x33, 0x00, 0x30, 0x00, 0x36, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x33, 0x00, 0x30, 0x00, 0x31, 0x00, 0x31, 0x00, 0x46, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x34, 0x00, 0x30, 0x00, 0x42, 0x00, 0x39, 0x00, 0x31, 0x00,
	0x34, 0x00, 0x33, 0x00, 0x33, 0x00, 0x36, 0x00, 0x35, 0x00, 0x34, 0x00,
	0x33, 0x00, 0x39, 0x00, 0x38, 0x00, 0x30, 0x00, 0x46, 0x00, 0x35, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x33, 0x00, 0x31, 0x00,
	0x30, 0x00, 0x31, 0x00, 0x31, 0x00, 0x33, 0x00, 0x32, 0x00, 0x31, 0x00,
	0x32, 0x00, 0x30, 0x00, 0x30, 0x00, 0x32, 0x00, 0x34, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x41, 0x00, 0x43, 0x00, 0x38, 0x00, 0x33, 0x00, 0x37, 0x00,
	0x33, 0x00, 0x42, 0x00, 0x30, 0x00, 0x43, 0x00, 0x36, 0x00, 0x41, 0x00,
	0x44, 0x00, 0x37, 0x00, 0x44, 0x00, 0x44, 0x00, 0x45, 0x00, 0x34, 0x00,
	0x33, 0x00, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00
};

const unsigned char new_sms_pdu_valid_1[] = {
	0x07, 0x91, 0x43, 0x06, 0x07, 0x30, 0x11, 0xf0, 0x04, 0x0b, 0x91, 0x43,
	0x36, 0x54, 0x39, 0x80, 0xf5, 0x00, 0x00, 0x31, 0x01, 0x13, 0x21, 0x20,
	0x02, 0x40, 0x0a, 0xc8, 0x37, 0x3b, 0x0c, 0x6a, 0xd7, 0xdd, 0xe4, 0x37
};

static const struct sms_data testdata_new_sms_valid_1 = {
	.start_func = trigger_new_sms,
	.rtd = {
		.req_data = unsol_response_new_sms_parcel_1,
		.req_size = sizeof(unsol_response_new_sms_parcel_1),
		.unsol_test = TRUE,
	},
	.pdu = new_sms_pdu_valid_1,
	.pdu_len = sizeof(new_sms_pdu_valid_1),
	.tpdu_len = 28,
};

/*
 * The following hexadecimal data represents a serialized Binder parcel
 * instance containing a valid UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT message
 * with the following parameter (SMSC address length is 6):
 *
 * {069143060730F0040B914336543980F50000310113212002400AC8373B0C6AD7DDE437}
 */
static const guchar unsol_response_new_sms_parcel_2[] = {
	0x00, 0x00, 0x00, 0x9C, 0x01, 0x00, 0x00, 0x00, 0xEC, 0x03, 0x00, 0x00,
	0x46, 0x00, 0x00, 0x00, 0x30, 0x00, 0x36, 0x00, 0x39, 0x00, 0x31, 0x00,
	0x34, 0x00, 0x33, 0x00, 0x30, 0x00, 0x36, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x33, 0x00, 0x30, 0x00, 0x46, 0x00, 0x30, 0x00,	0x30, 0x00, 0x34, 0x00,
	0x30, 0x00, 0x42, 0x00, 0x39, 0x00, 0x31, 0x00,	0x34, 0x00, 0x33, 0x00,
	0x33, 0x00, 0x36, 0x00, 0x35, 0x00, 0x34, 0x00,	0x33, 0x00, 0x39, 0x00,
	0x38, 0x00, 0x30, 0x00, 0x46, 0x00, 0x35, 0x00,	0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x33, 0x00, 0x31, 0x00,	0x30, 0x00, 0x31, 0x00,
	0x31, 0x00, 0x33, 0x00, 0x32, 0x00, 0x31, 0x00,	0x32, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x32, 0x00, 0x34, 0x00, 0x30, 0x00,	0x30, 0x00, 0x41, 0x00,
	0x43, 0x00, 0x38, 0x00, 0x33, 0x00, 0x37, 0x00,	0x33, 0x00, 0x42, 0x00,
	0x30, 0x00, 0x43, 0x00, 0x36, 0x00, 0x41, 0x00,	0x44, 0x00, 0x37, 0x00,
	0x44, 0x00, 0x44, 0x00, 0x45, 0x00, 0x34, 0x00,	0x33, 0x00, 0x37, 0x00,
	0x00, 0x00, 0x00, 0x00
};

const unsigned char new_sms_pdu_valid_2[] = {
	0x06, 0x91, 0x43, 0x06, 0x07, 0x30, 0xf0, 0x04, 0x0b, 0x91, 0x43, 0x36,
	0x54, 0x39, 0x80, 0xf5, 0x00, 0x00, 0x31, 0x01, 0x13, 0x21, 0x20, 0x02,
	0x40, 0x0a, 0xc8, 0x37, 0x3b, 0x0c, 0x6a, 0xd7, 0xdd, 0xe4, 0x37
};

static const struct sms_data testdata_new_sms_valid_2 = {
	.start_func = trigger_new_sms,
	.rtd = {
		.req_data = unsol_response_new_sms_parcel_2,
		.req_size = sizeof(unsol_response_new_sms_parcel_2),
		.unsol_test = TRUE,
	},
	.pdu = new_sms_pdu_valid_2,
	.pdu_len = sizeof(new_sms_pdu_valid_2),
	.tpdu_len = 28,
};

static void server_connect_cb(gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	/* This causes local impl of _create() to call driver's probe func. */
	rsd->sms = ofono_sms_create(NULL, OFONO_RIL_VENDOR_AOSP,
							"rilmodem", rsd);
	rsd->sms->sd = sd;

	/* add_idle doesn't work, read blocks main loop!!! */

	if (sd->rtd.unsol_test)
		g_idle_add(sd->start_func, (void *) rsd);
	else
		g_assert(sd->start_func(rsd) == FALSE);
}

/*
 * This unit test:
 *  - does some test data setup
 *  - configures a dummy server socket
 *  - creates a new gril client instance
 *    - triggers a connect to the dummy
 *      server socket
 *  - starts a mainloop
 */
static void test_sms_func(gconstpointer data)
{
	const struct sms_data *sd = data;
	struct rilmodem_sms_data *rsd;

	ril_sms_init();

	rsd = g_new0(struct rilmodem_sms_data, 1);

	rsd->test_data = sd;

	rsd->serverd = rilmodem_test_server_create(&server_connect_cb,
								&sd->rtd, rsd);

	rsd->ril = g_ril_new(rilmodem_test_get_socket_name(rsd->serverd),
							OFONO_RIL_VENDOR_AOSP);
	g_assert(rsd->ril != NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);

	smsdriver->remove(rsd->sms);
	g_ril_unref(rsd->ril);
	g_free(rsd);

	rilmodem_test_server_close(rsd->serverd);

	ril_sms_exit();
}

#endif

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN
	g_test_add_data_func("/testrilmodemsms/sca_query/valid/1",
					&testdata_sca_query_valid_1,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/sca_query/invalid/1",
					&testdata_sca_query_invalid_1,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/sca_query/invalid/2",
					&testdata_sca_query_invalid_2,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/sca_query/invalid/3",
					&testdata_sca_query_invalid_3,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/sca_set/valid/1",
					&testdata_sca_set_valid_1,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/sca_set/invalid/1",
					&testdata_sca_set_invalid_1,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/submit/valid/1",
					&testdata_submit_valid_1,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/submit/invalid/1",
					&testdata_submit_invalid_1,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/new_sms/valid/1",
					&testdata_new_sms_valid_1,
					test_sms_func);
	g_test_add_data_func("/testrilmodemsms/new_sms/valid/2",
					&testdata_new_sms_valid_2,
					test_sms_func);

#endif
	return g_test_run();
}
