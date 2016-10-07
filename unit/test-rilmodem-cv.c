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
#include <ofono/call-volume.h>
#include <gril.h>

#include "common.h"
#include "ril_constants.h"
#include "rilmodem-test-server.h"

/* Only mute action supported by rilmodem for the moment */
enum cv_driver_call {
	CV_CALL_MUTE,
};

struct cv_data {
	struct rilmodem_test_data rtd;
	enum cv_driver_call call_type;

	/* Input parameters */
	int muted;

	/* Output parameters */
	enum ofono_error_type error_type;
};

struct rilmodem_cv_data {
	GRil *ril;
	struct ofono_modem *modem;
	const struct cv_data *test_data;
	struct ofono_call_volume *cv;
	struct server_data *serverd;
};

static const struct ofono_call_volume_driver *cvdriver;

/* Declarations && Re-implementations of core functions. */
void ril_call_volume_exit(void);
void ril_call_volume_init(void);

struct ofono_call_volume {
	void *driver_data;
	const struct rilmodem_cv_data *rcd;
};

static GMainLoop *mainloop;

int ofono_call_volume_driver_register(
				const struct ofono_call_volume_driver *d)
{
	if (cvdriver == NULL)
		cvdriver = d;

	return 0;
}

void ofono_call_volume_driver_unregister(
				const struct ofono_call_volume_driver *d)
{
	cvdriver = NULL;
}

struct ofono_call_volume *ofono_call_volume_create(
						struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data)
{
	struct rilmodem_cv_data *rcd = data;
	struct ofono_call_volume *cv = g_malloc0(sizeof(*cv));
	int retval;

	cv->rcd = rcd;

	retval = cvdriver->probe(cv, OFONO_RIL_VENDOR_AOSP, rcd->ril);
	g_assert(retval == 0);

	return cv;
}

void ofono_call_volume_register(struct ofono_call_volume *cv)
{
}

void ofono_call_volume_set_data(struct ofono_call_volume *cv,
					void *data)
{
	cv->driver_data = data;
}

void *ofono_call_volume_get_data(struct ofono_call_volume *cv)
{
	return cv->driver_data;
}

void ofono_call_volume_set_muted(struct ofono_call_volume *cv, int muted)
{
	g_assert(cv->rcd->test_data->muted == muted);

	g_main_loop_quit(mainloop);
}

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

/* Test SET_MUTE to false */
static const guchar req_cv_mute_parcel_valid_1[] = {
	0x00, 0x00, 0x00, 0x10, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct cv_data testdata_cv_mute_valid_1 = {
	.rtd = {
		.req_data = req_cv_mute_parcel_valid_1,
		.req_size = sizeof(req_cv_mute_parcel_valid_1),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.call_type = CV_CALL_MUTE,
	.muted = 0,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* Test SET_MUTE to true */
static const guchar req_cv_mute_parcel_valid_2[] = {
	0x00, 0x00, 0x00, 0x10, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct cv_data testdata_cv_mute_valid_2 = {
	.rtd = {
		.req_data = req_cv_mute_parcel_valid_2,
		.req_size = sizeof(req_cv_mute_parcel_valid_2),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_SUCCESS,
	},
	.call_type = CV_CALL_MUTE,
	.muted = 1,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* Test SET_MUTE to false, fail */
static const guchar req_cv_mute_parcel_invalid_1[] = {
	0x00, 0x00, 0x00, 0x10, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct cv_data testdata_cv_mute_invalid_1 = {
	.rtd = {
		.req_data = req_cv_mute_parcel_invalid_1,
		.req_size = sizeof(req_cv_mute_parcel_invalid_1),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_GENERIC_FAILURE,
	},
	.call_type = CV_CALL_MUTE,
	.muted = 0,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

static void cv_mute_callback(const struct ofono_error *error, void *data)
{
	struct rilmodem_cv_data *rcd = data;
	const struct cv_data *cvd = rcd->test_data;

	g_assert(error->type == cvd->error_type);

	g_main_loop_quit(mainloop);
}

static void server_connect_cb(gpointer data)
{
	struct rilmodem_cv_data *rcd = data;

	/* This causes local impl of _create() to call driver's probe func. */
	/*
	 * TODO The call to probe() calls g_idle_add to send a GET_MUTE request.
	 * The way the calls are structured implies that we send the SET_MUTE
	 * made by the posterior call to mute() before this GET_MUTE. This in
	 * fact lets us test mute() and other driver calls, but impedes us to
	 * properly test the probe() call. This kind of issue is common to all
	 * test-rilmodem tests. To solve this we need to generalize the tests so
	 * general dialogues can be established with the atom driver under test.
	 * The test should define steps that could be either actions to perform
	 * or events to wait for.
	 */
	rcd->cv = ofono_call_volume_create(NULL, OFONO_RIL_VENDOR_AOSP,
							"rilmodem", rcd);

	switch (rcd->test_data->call_type) {
	case CV_CALL_MUTE:
		cvdriver->mute(rcd->cv, rcd->test_data->muted,
							cv_mute_callback, rcd);
		break;
	};
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
static void test_cv_func(gconstpointer data)
{
	const struct cv_data *cvd = data;
	struct rilmodem_cv_data *rcd;

	ril_call_volume_init();

	rcd = g_malloc0(sizeof(*rcd));

	rcd->test_data = cvd;

	rcd->serverd = rilmodem_test_server_create(&server_connect_cb,
								&cvd->rtd, rcd);

	rcd->ril = g_ril_new(rilmodem_test_get_socket_name(rcd->serverd),
							OFONO_RIL_VENDOR_AOSP);
	g_assert(rcd->ril != NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);

	cvdriver->remove(rcd->cv);
	g_free(rcd->cv);
	g_ril_unref(rcd->ril);
	g_free(rcd);

	rilmodem_test_server_close(rcd->serverd);

	ril_call_volume_exit();
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
	g_test_add_data_func("/testrilmodemcv/cv_mute/valid/1",
					&testdata_cv_mute_valid_1,
					test_cv_func);
	g_test_add_data_func("/testrilmodemcv/cv_mute/valid/2",
					&testdata_cv_mute_valid_2,
					test_cv_func);
	g_test_add_data_func("/testrilmodemcv/cv_mute/invalid/1",
					&testdata_cv_mute_invalid_1,
					test_cv_func);
#endif
	return g_test_run();
}
