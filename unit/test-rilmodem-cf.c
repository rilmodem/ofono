/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016 Canonical Ltd.
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
#include <ofono/call-forwarding.h>
#include <gril.h>

#include "common.h"
#include "ril_constants.h"
#include "rilmodem-test-server.h"

enum cf_driver_call {
	CF_CALL_ACTIVATION,
	CF_CALL_REGISTRATION,
	CF_CALL_DEACTIVATION,
	CF_CALL_ERASURE,
	CF_CALL_QUERY,
};

struct cf_data {
	enum cf_driver_call call_type;
	struct rilmodem_test_data rtd;

	/* Possible input parameters */
	int type;
	int cls;
	struct ofono_phone_number number;
	int time;

	/* Possible output parameters */
	enum ofono_error_type error_type;
	int total;
	struct ofono_call_forwarding_condition *list;
};

struct rilmodem_cf_data {
	GRil *ril;
	struct ofono_modem *modem;
	const struct cf_data *test_data;
	struct ofono_call_forwarding *cf;
	struct server_data *serverd;
};

static const struct ofono_call_forwarding_driver *cfdriver;

/* Declarations && Re-implementations of core functions. */
void ril_call_forwarding_exit(void);
void ril_call_forwarding_init(void);

struct ofono_call_forwarding {
	void *driver_data;
};

int ofono_call_forwarding_driver_register(
				const struct ofono_call_forwarding_driver *d)
{
	if (cfdriver == NULL)
		cfdriver = d;

	return 0;
}

void ofono_call_forwarding_driver_unregister(
				const struct ofono_call_forwarding_driver *d)
{
	cfdriver = NULL;
}

struct ofono_call_forwarding *ofono_call_forwarding_create(
						struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data)
{
	struct rilmodem_cf_data *rcd = data;
	struct ofono_call_forwarding *cf = g_malloc0(sizeof(*cf));
	int retval;

	retval = cfdriver->probe(cf, OFONO_RIL_VENDOR_AOSP, rcd->ril);
	g_assert(retval == 0);

	return cf;
}

void ofono_call_forwarding_register(struct ofono_call_forwarding *cf)
{
}

void ofono_call_forwarding_remove(struct ofono_call_forwarding *cf)
{
}

void ofono_call_forwarding_set_data(struct ofono_call_forwarding *cf,
					void *data)
{
	cf->driver_data = data;
}

void *ofono_call_forwarding_get_data(struct ofono_call_forwarding *cf)
{
	return cf->driver_data;
}

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

static GMainLoop *mainloop;

static void cf_set_callback(const struct ofono_error *error, void *data)
{
	struct rilmodem_cf_data *rcd = data;
	const struct cf_data *cfd = rcd->test_data;

	g_assert(error->type == cfd->error_type);

	g_main_loop_quit(mainloop);
}

static void cf_query_callback(const struct ofono_error *error, int total,
			const struct ofono_call_forwarding_condition *list,
			void *data)
{
	struct rilmodem_cf_data *rcd = data;
	const struct cf_data *cfd = rcd->test_data;

	g_assert(error->type == cfd->error_type);

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		int i;

		g_assert(total == cfd->total);

		for (i = 0; i < total; ++i) {
			const struct ofono_call_forwarding_condition *cur, *cmp;

			cur = &list[i];
			cmp = &cfd->list[i];
			g_assert(cur->status == cmp->status);
			g_assert(cur->cls == cmp->cls);
			g_assert_cmpstr(cur->phone_number.number, ==,
						cmp->phone_number.number);
			g_assert(cur->phone_number.type
						== cmp->phone_number.type);
			g_assert(cur->time == cmp->time);
		}
	}

	g_main_loop_quit(mainloop);
}

static gboolean trigger_cf_call(struct rilmodem_cf_data *rcd)
{
	switch(rcd->test_data->call_type) {
	case CF_CALL_ACTIVATION:
		cfdriver->activation(rcd->cf, rcd->test_data->type,
						rcd->test_data->cls,
						cf_set_callback, rcd);
		break;
	case CF_CALL_REGISTRATION:
		cfdriver->registration(rcd->cf, rcd->test_data->type,
						rcd->test_data->cls,
						&rcd->test_data->number,
						rcd->test_data->time,
						cf_set_callback, rcd);
		break;
	case CF_CALL_DEACTIVATION:
		cfdriver->deactivation(rcd->cf, rcd->test_data->type,
						rcd->test_data->cls,
						cf_set_callback, rcd);
		break;
	case CF_CALL_ERASURE:
		cfdriver->erasure(rcd->cf, rcd->test_data->type,
						rcd->test_data->cls,
						cf_set_callback, rcd);
		break;
	case CF_CALL_QUERY:
		cfdriver->query(rcd->cf, rcd->test_data->type,
				rcd->test_data->cls, cf_query_callback, rcd);
		break;
	};

	return FALSE;
}

/*
 * Test SET_CALL_FORWARD, activate unconditional forwarding SS. Real transaction
 * from turbo.
 */
static const guchar req_cf_set_parcel_1[] = {
	0x00, 0x00, 0x00, 0x38, 0x22, 0x00, 0x00, 0x00, 0x49, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

static const struct cf_data testdata_cf_set_valid_1 = {
	.call_type = CF_CALL_ACTIVATION,
	.rtd = {
		.req_data = req_cf_set_parcel_1,
		.req_size = sizeof(req_cf_set_parcel_1),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = 0,
	.number = { "1234567890", OFONO_NUMBER_TYPE_UNKNOWN },
	.time = 60,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.total = 0,
	.list = NULL,
};

/*
 * Test SET_CALL_FORWARD, register unconditional forwarding to 999999999.
 * Real transaction from turbo.
 */
static const guchar req_cf_set_parcel_2[] = {
	0x00, 0x00, 0x00, 0x34, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x39, 0x00, 0x39, 0x00,
	0x39, 0x00, 0x39, 0x00, 0x39, 0x00, 0x39, 0x00, 0x39, 0x00, 0x39, 0x00,
	0x39, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00
};

static const struct cf_data testdata_cf_set_valid_2 = {
	.call_type = CF_CALL_REGISTRATION,
	.rtd = {
		.req_data = req_cf_set_parcel_2,
		.req_size = sizeof(req_cf_set_parcel_2),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = 0,
	.number = { "999999999", OFONO_NUMBER_TYPE_UNKNOWN },
	.time = 20,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.total = 0,
	.list = NULL,
};

/* Test SET_CALL_FORWARD, deactivate SS.  Real transaction from turbo. */
static const guchar req_cf_set_parcel_3[] = {
	0x00, 0x00, 0x00, 0x38, 0x22, 0x00, 0x00, 0x00, 0x1d, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

static const struct cf_data testdata_cf_set_valid_3 = {
	.call_type = CF_CALL_DEACTIVATION,
	.rtd = {
		.req_data = req_cf_set_parcel_3,
		.req_size = sizeof(req_cf_set_parcel_3),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = 0,
	.number = { "1234567890", OFONO_NUMBER_TYPE_UNKNOWN },
	.time = 60,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.total = 0,
	.list = NULL,
};

/* Test SET_CALL_FORWARD, erase SS.  Real transaction from turbo. */
static const guchar req_cf_set_parcel_4[] = {
	0x00, 0x00, 0x00, 0x38, 0x22, 0x00, 0x00, 0x00, 0x64, 0x01, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

static const struct cf_data testdata_cf_set_valid_4 = {
	.call_type = CF_CALL_ERASURE,
	.rtd = {
		.req_data = req_cf_set_parcel_4,
		.req_size = sizeof(req_cf_set_parcel_4),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = 0,
	.number = { "1234567890", OFONO_NUMBER_TYPE_UNKNOWN },
	.time = 60,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.total = 0,
	.list = NULL,
};

/*
 * Test SET_CALL_FORWARD, activate unconditional forwarding SS. We check here
 * that BEARER_CLASS_DEFAULT is converted to SERVICE_CLASS_NONE and time -1 to
 * 60 seconds.
 */
static const guchar req_cf_set_parcel_5[] = {
	0x00, 0x00, 0x00, 0x38, 0x22, 0x00, 0x00, 0x00, 0x49, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

static const struct cf_data testdata_cf_set_valid_5 = {
	.call_type = CF_CALL_ACTIVATION,
	.rtd = {
		.req_data = req_cf_set_parcel_5,
		.req_size = sizeof(req_cf_set_parcel_5),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = BEARER_CLASS_DEFAULT,
	.number = { "1234567890", OFONO_NUMBER_TYPE_UNKNOWN },
	.time = -1,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.total = 0,
	.list = NULL,
};

/* Test SET_CALL_FORWARD, error on activate unconditional forwarding */
static const guchar req_cf_set_parcel_inv_1[] = {
	0x00, 0x00, 0x00, 0x38, 0x22, 0x00, 0x00, 0x00, 0x49, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

static const struct cf_data testdata_cf_set_invalid_1 = {
	.call_type = CF_CALL_ACTIVATION,
	.rtd = {
		.req_data = req_cf_set_parcel_inv_1,
		.req_size = sizeof(req_cf_set_parcel_inv_1),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_GENERIC_FAILURE,
	},
	.type = 0,
	.cls = 0,
	.number = { "1234567890", OFONO_NUMBER_TYPE_UNKNOWN },
	.time = 60,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
	.total = 0,
	.list = NULL,
};

/* Test QUERY_CALL_FORWARD_STATUS. Real transaction from turbo. */
static const guchar req_cf_query_parcel_1[] = {
	0x00, 0x00, 0x00, 0x38, 0x21, 0x00, 0x00, 0x00, 0xa5, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

/* reply data for QUERY_CALL_FORWARD (disabled) */
static const guchar rsp_cf_query_data_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

struct ofono_call_forwarding_condition list_cf_query_valid_1 = {
	.status = 0,
	.cls = BEARER_CLASS_DEFAULT,
	.phone_number = { "", 0 },
	.time = 0
};

static const struct cf_data testdata_cf_query_valid_1 = {
	.call_type = CF_CALL_QUERY,
	.rtd = {
		.req_data = req_cf_query_parcel_1,
		.req_size = sizeof(req_cf_query_parcel_1),
		.rsp_data = rsp_cf_query_data_1,
		.rsp_size = sizeof(rsp_cf_query_data_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = 0,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.total = 1,
	.list = &list_cf_query_valid_1,
};

/* Test QUERY_CALL_FORWARD_STATUS, empty list returned case */
static const guchar req_cf_query_parcel_2[] = {
	0x00, 0x00, 0x00, 0x38, 0x21, 0x00, 0x00, 0x00, 0xa5, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

static const guchar rsp_cf_query_data_2[] = {
	0x00, 0x00, 0x00, 0x00
};

struct ofono_call_forwarding_condition list_cf_query_valid_2 = {
	.status = 0,
	.cls = 0,
	.phone_number = { "", 0 },
	.time = 0
};

static const struct cf_data testdata_cf_query_valid_2 = {
	.call_type = CF_CALL_QUERY,
	.rtd = {
		.req_data = req_cf_query_parcel_2,
		.req_size = sizeof(req_cf_query_parcel_2),
		.rsp_data = rsp_cf_query_data_2,
		.rsp_size = sizeof(rsp_cf_query_data_2),
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = 0,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.total = 1,
	.list = &list_cf_query_valid_2,
};

/* Test QUERY_CALL_FORWARD_STATUS, GENERIC_FAILURE returned in RIL reply */
static const guchar req_cf_query_parcel_inv_1[] = {
	0x00, 0x00, 0x00, 0x38, 0x21, 0x00, 0x00, 0x00, 0xa5, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

static const struct cf_data testdata_cf_query_invalid_1 = {
	.call_type = CF_CALL_QUERY,
	.rtd = {
		.req_data = req_cf_query_parcel_1,
		.req_size = sizeof(req_cf_query_parcel_1),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_GENERIC_FAILURE,
	},
	.type = 0,
	.cls = 0,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
	.total = 0,
	.list = NULL,
};

/* Test QUERY_CALL_FORWARD_STATUS, malformed parcel case (no data returned). */
static const guchar req_cf_query_parcel_inv_2[] = {
	0x00, 0x00, 0x00, 0x38, 0x21, 0x00, 0x00, 0x00, 0xa5, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
	0x39, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00
};

struct ofono_call_forwarding_condition list_cf_query_invalid_2 = {
	.status = 0,
	.cls = 0,
	.phone_number = { "", 0 },
	.time = 0
};

static const struct cf_data testdata_cf_query_invalid_2 = {
	.call_type = CF_CALL_QUERY,
	.rtd = {
		.req_data = req_cf_query_parcel_inv_2,
		.req_size = sizeof(req_cf_query_parcel_inv_2),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.type = 0,
	.cls = 0,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
	.total = 1,
	.list = &list_cf_query_invalid_2,
};

static void server_connect_cb(gpointer data)
{
	struct rilmodem_cf_data *rcd = data;

	/* This causes local impl of _create() to call driver's probe func. */
	rcd->cf = ofono_call_forwarding_create(NULL, OFONO_RIL_VENDOR_AOSP,
							"rilmodem", rcd);

	/* add_idle doesn't work, read blocks main loop!!! */
	g_assert(trigger_cf_call(rcd) == FALSE);
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
static void test_cf_func(gconstpointer data)
{
	const struct cf_data *cfd = data;
	struct rilmodem_cf_data *rcd;

	ril_call_forwarding_init();

	rcd = g_malloc0(sizeof(*rcd));

	rcd->test_data = cfd;

	rcd->serverd = rilmodem_test_server_create(&server_connect_cb,
								&cfd->rtd, rcd);

	rcd->ril = g_ril_new(rilmodem_test_get_socket_name(rcd->serverd),
							OFONO_RIL_VENDOR_AOSP);
	g_assert(rcd->ril != NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);

	cfdriver->remove(rcd->cf);
	g_ril_unref(rcd->ril);
	g_free(rcd);

	rilmodem_test_server_close(rcd->serverd);

	ril_call_forwarding_exit();
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
	g_test_add_data_func("/testrilmodemcf/cf_set/valid/1",
					&testdata_cf_set_valid_1,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_set/valid/2",
					&testdata_cf_set_valid_2,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_set/valid/3",
					&testdata_cf_set_valid_3,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_set/valid/4",
					&testdata_cf_set_valid_4,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_set/valid/5",
					&testdata_cf_set_valid_5,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_set/invalid/1",
					&testdata_cf_set_invalid_1,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_query/valid/1",
					&testdata_cf_query_valid_1,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_query/valid/2",
					&testdata_cf_query_valid_2,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_query/invalid/1",
					&testdata_cf_query_invalid_1,
					test_cf_func);
	g_test_add_data_func("/testrilmodemcf/cf_query/invalid/2",
					&testdata_cf_query_invalid_2,
					test_cf_func);
#endif
	return g_test_run();
}
