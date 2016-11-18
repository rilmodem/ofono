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
#include "rilmodem-test-engine.h"

static const struct ofono_call_volume_driver *cvdriver;

/* Declarations && Re-implementations of core functions. */
void ril_call_volume_exit(void);
void ril_call_volume_init(void);

struct ofono_call_volume {
	void *driver_data;
	GRil *ril;
	struct engine_data *engined;
};

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

void ofono_call_volume_set_data(struct ofono_call_volume *cv, void *data)
{
	cv->driver_data = data;
}

void *ofono_call_volume_get_data(struct ofono_call_volume *cv)
{
	return cv->driver_data;
}

OFONO_EVENT_CALL_ARG_1(ofono_call_volume_register, struct ofono_call_volume *)
OFONO_EVENT_CALL_ARG_2(ofono_call_volume_set_muted,
						struct ofono_call_volume *, int)
OFONO_EVENT_CALL_CB_ARG_2(ofono_call_volume_cb, const struct ofono_error *,
						struct ofono_call_volume *)

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

/* REQUEST_GET_MUTE, seq 1 */
static const char parcel_req_get_mute_1_2[] = {
	0x00, 0x00, 0x00, 0x08, 0x36, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

/* REQUEST_GET_MUTE rsp, seq 1, not muted */
static const char parcel_rsp_get_mute_1_3[] = {
	0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void check_call_volume_set_muted_1_4(struct ofono_call_volume *cv,
								int muted)
{
	g_assert(muted == 0);
}

static void call_set_mute_1_5(gpointer data)
{
	struct ofono_call_volume *cv = data;

	cvdriver->mute(cv, 0, ofono_call_volume_cb, cv);

	rilmodem_test_engine_next_step(cv->engined);
}

/* REQUEST_SET_MUTE, seq 2, false */
static const char parcel_req_set_mute_1_6[] = {
	0x00, 0x00, 0x00, 0x10, 0x35, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* REQUEST_SET_MUTE rsp, seq 2, success */
static const char parcel_rsp_set_mute_1_7[] = {
	0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static void check_mute_1_8(const struct ofono_error *error,
						struct ofono_call_volume *cv)
{
	g_assert(error->type == OFONO_ERROR_TYPE_NO_ERROR);
}

/*
 * --- TEST 1 ---
 * Step 1: Driver calls ofono_cv_register
 * Step 2: Driver sends REQUEST_GET_MUTE
 * Step 3: Harness answers, state is not muted
 * Step 4: Driver calls ofono_call_volume_set_muted(false)
 * Step 5: Harness call drv->mute(false)
 * Step 6: Driver sends REQUEST_SET_MUTE
 * Step 7: Harness answers, success
 * Step 8: Driver calls the callback specified in step 2
 */
static const struct rilmodem_test_step steps_test_1[] = {
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_register,
		.check_func = NULL
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_get_mute_1_2,
		.parcel_size = sizeof(parcel_req_get_mute_1_2)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_get_mute_1_3,
		.parcel_size = sizeof(parcel_rsp_get_mute_1_3)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_set_muted,
		.check_func = (void (*)(void)) check_call_volume_set_muted_1_4
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_set_mute_1_5,
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_set_mute_1_6,
		.parcel_size = sizeof(parcel_req_set_mute_1_6)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_set_mute_1_7,
		.parcel_size = sizeof(parcel_rsp_set_mute_1_7)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_cb,
		.check_func = (void (*)(void)) check_mute_1_8
	},
};

static const struct rilmodem_test_data test_1 = {
	.steps = steps_test_1,
	.num_steps = G_N_ELEMENTS(steps_test_1)
};

static void call_set_mute_2_5(gpointer data)
{
	struct ofono_call_volume *cv = data;

	cvdriver->mute(cv, 1, ofono_call_volume_cb, cv);

	rilmodem_test_engine_next_step(cv->engined);
}

/* REQUEST_SET_MUTE, seq 2, true */
static const char parcel_req_set_mute_2_6[] = {
	0x00, 0x00, 0x00, 0x10, 0x35, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

/* REQUEST_SET_MUTE rsp, seq 2, success */
static const char parcel_rsp_set_mute_2_7[] = {
	0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static void check_mute_2_8(const struct ofono_error *error,
						struct ofono_call_volume *cv)
{
	g_assert(error->type == OFONO_ERROR_TYPE_NO_ERROR);
}

/*
 * --- TEST 2 ---
 * Steps 1-4: Same as test 1
 * Step 5: Harness call drv->mute(true)
 * Step 6: Driver sends REQUEST_SET_MUTE
 * Step 7: Harness answers, success
 * Step 8: Driver calls the callback specified in step 2
 */
static const struct rilmodem_test_step steps_test_2[] = {
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_register,
		.check_func = NULL
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_get_mute_1_2,
		.parcel_size = sizeof(parcel_req_get_mute_1_2)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_get_mute_1_3,
		.parcel_size = sizeof(parcel_rsp_get_mute_1_3)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_set_muted,
		.check_func = (void (*)(void)) check_call_volume_set_muted_1_4
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_set_mute_2_5,
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_set_mute_2_6,
		.parcel_size = sizeof(parcel_req_set_mute_2_6)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_set_mute_2_7,
		.parcel_size = sizeof(parcel_rsp_set_mute_2_7)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_cb,
		.check_func = (void (*)(void)) check_mute_2_8
	},
};

static const struct rilmodem_test_data test_2 = {
	.steps = steps_test_2,
	.num_steps = G_N_ELEMENTS(steps_test_2)
};

static void call_set_mute_3_5(gpointer data)
{
	struct ofono_call_volume *cv = data;

	cvdriver->mute(cv, 1, ofono_call_volume_cb, cv);

	rilmodem_test_engine_next_step(cv->engined);
}

static void check_mute_cb_3_8(const struct ofono_error *error,
						struct ofono_call_volume *cv)
{
	g_assert(error->type == OFONO_ERROR_TYPE_FAILURE);
}

/* REQUEST_SET_MUTE, seq 2, true */
static const char parcel_req_set_mute_3_6[] = {
	0x00, 0x00, 0x00, 0x10, 0x35, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

/* REQUEST_SET_MUTE rsp, seq 2, RIL_E_GENERIC_FAILURE */
static const char parcel_rsp_set_mute_3_7[] = {
	0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00
};

/*
 * --- TEST 3 ---
 * Steps 1-4: Same as test 1
 * Step 5: Harness call drv->mute(true)
 * Step 6: Driver sends REQUEST_SET_MUTE
 * Step 7: Harness answers, failure
 * Step 8: Driver calls the callback specified in step 2
 */
static const struct rilmodem_test_step steps_test_3[] = {
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_register,
		.check_func = NULL
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_get_mute_1_2,
		.parcel_size = sizeof(parcel_req_get_mute_1_2)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_get_mute_1_3,
		.parcel_size = sizeof(parcel_rsp_get_mute_1_3)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_set_muted,
		.check_func = (void (*)(void)) check_call_volume_set_muted_1_4
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_set_mute_3_5,
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_set_mute_3_6,
		.parcel_size = sizeof(parcel_req_set_mute_3_6)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_set_mute_3_7,
		.parcel_size = sizeof(parcel_rsp_set_mute_3_7)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_call_volume_cb,
		.check_func = (void (*)(void)) check_mute_cb_3_8
	},
};

static const struct rilmodem_test_data test_3 = {
	.steps = steps_test_3,
	.num_steps = G_N_ELEMENTS(steps_test_3)
};

static void server_connect_cb(gpointer data)
{
	struct ofono_call_volume *cv = data;
	int retval;

	/* This starts the test. First for this atom is a call to _register. */
	retval = cvdriver->probe(cv, OFONO_RIL_VENDOR_AOSP, cv->ril);
	g_assert(retval == 0);
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
static void test_function(gconstpointer data)
{
	const struct rilmodem_test_data *test_data = data;
	struct ofono_call_volume *cv;

	ril_call_volume_init();

	cv = g_malloc0(sizeof(*cv));

	cv->engined = rilmodem_test_engine_create(&server_connect_cb,
							test_data, cv);

	cv->ril = g_ril_new(rilmodem_test_engine_get_socket_name(cv->engined),
							OFONO_RIL_VENDOR_AOSP);
	g_assert(cv->ril != NULL);

	/* So the driver is allowed to send ALLOW_DATA */
	g_ril_set_version(cv->ril, 10);

	/* Perform test */
	rilmodem_test_engine_start(cv->engined);

	cvdriver->remove(cv);
	g_ril_unref(cv->ril);
	g_free(cv);

	rilmodem_test_engine_remove(cv->engined);

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
	g_test_add_data_func("/test-rilmodem-cv/1", &test_1, test_function);
	g_test_add_data_func("/test-rilmodem-cv/2", &test_2, test_function);
	g_test_add_data_func("/test-rilmodem-cv/3", &test_3, test_function);
#endif
	return g_test_run();
}
