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

static GMainLoop *mainloop;

static const struct ofono_sms_driver *smsdriver;

struct rilmodem_sms_data {
	GRil *ril;
	struct ofono_modem *modem;
	gconstpointer test_data;
	struct ofono_sms *sms;
};

typedef gboolean (*StartFunc)(gpointer data);

struct sms_data {
	StartFunc start_func;
	const struct ofono_phone_number ph;
	gint param_int1;
	gint param_int2;

	struct rilmodem_test_data rtd;
	enum ofono_error_type error_type;
	gint cb_int1;
	gint cb_int2;
};

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

static gboolean trigger_sca_query(gpointer data)
{
	struct rilmodem_sms_data *rsd = data;

	g_assert(smsdriver->sca_query != NULL);
	smsdriver->sca_query(rsd->sms, sca_query_callback, rsd);

	return FALSE;
}

/* RIL_REQUEST_GET_SMSC_ADDRESS */
static const guchar req_get_smsc_address_parcel_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};


/*
 * RIL_REQUEST_GET_SMSC_ADDRESS reply with the following data:
 *
 * {type=145,number=34607003110}
 */
static const guchar rsp_get_smsc_address_data_1[] = {
	0x12, 0x00, 0x00, 0x00, 0x22, 0x00, 0x2b, 0x00, 0x33, 0x00, 0x34, 0x00,
	0x36, 0x00, 0x30, 0x00, 0x37, 0x00, 0x30, 0x00, 0x30, 0x00, 0x33, 0x00,
	0x31, 0x00, 0x31, 0x00, 0x30, 0x00, 0x22, 0x00, 0x2c, 0x00, 0x31, 0x00,
	0x34, 0x00, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct sms_data testdata_sca_query_valid_1 = {
	.start_func = trigger_sca_query,
	.ph = { .number = "34607003110", .type = 145 },
	.rtd = {
		.req_data = req_get_smsc_address_parcel_1,
		.req_size = sizeof(req_get_smsc_address_parcel_1),
		.rsp_data = rsp_get_smsc_address_data_1,
		.rsp_size = sizeof(rsp_get_smsc_address_data_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.cb_int1 = 1,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* Declarations && Re-implementations of core functions. */
void ril_sms_exit(void);
void ril_sms_init(void);

struct ofono_sms {
	void *driver_data;
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
}

void ofono_sms_status_notify(struct ofono_sms *sms, const unsigned char *pdu,
							int len, int tpdu_len)
{
}

static void server_connect_cb(gpointer data)
{
	struct rilmodem_sms_data *rsd = data;
	const struct sms_data *sd = rsd->test_data;

	/* This causes local impl of _create() to call driver's probe func. */
	rsd->sms = ofono_sms_create(NULL, OFONO_RIL_VENDOR_AOSP,
							"rilmodem", rsd);

	/* add_idle doesn't work, read blocks main loop!!! */
	g_assert(sd->start_func(rsd) == FALSE);
}

#if BYTE_ORDER == LITTLE_ENDIAN

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

	rilmodem_test_server_create(&server_connect_cb, &sd->rtd, rsd);

	rsd->ril = g_ril_new(RIL_SERVER_SOCK_PATH, OFONO_RIL_VENDOR_AOSP);
	g_assert(rsd->ril != NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);

	smsdriver->remove(rsd->sms);
	g_ril_unref(rsd->ril);
	g_free(rsd);

	rilmodem_test_server_close();

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

#endif
	return g_test_run();
}
