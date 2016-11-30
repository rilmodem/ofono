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

#include <ofono.h>
#include <ofono/modem.h>
#include <ofono/types.h>
#include <ofono/gprs-context.h>
#include <gril.h>

#include "drivers/rilmodem/rilutil.h"
#include "drivers/mtkmodem/mtkutil.h"
#include "drivers/rilmodem/gprs.h"
#include "common.h"
#include "ril_constants.h"
#include "rilmodem-test-engine.h"

static const struct ofono_gprs_context_driver *gcdriver;

/* Declarations && Re-implementations of core functions. */
void ril_gprs_context_exit(void);
void ril_gprs_context_init(void);

struct ofono_gprs_context {
	void *driver_data;
	GRil *ril;
	struct engine_data *engined;
};

struct ofono_gprs {
	struct ril_gprs_data gprs_data;
};

struct ofono_atom {
	struct ofono_gprs gprs;
};

struct ofono_atom gprs_atom =
		{ .gprs = { .gprs_data = { .tech = RADIO_TECH_GPRS } } };

int ofono_gprs_context_driver_register(
				const struct ofono_gprs_context_driver *d)
{
	if (gcdriver == NULL)
		gcdriver = d;

	return 0;
}

void ofono_gprs_context_driver_unregister(
				const struct ofono_gprs_context_driver *d)
{
	gcdriver = NULL;
}

void ofono_gprs_context_set_data(struct ofono_gprs_context *gc, void *data)
{
	const struct rilmodem_test_step *step;

	/* Test finished (set_data(NULL) is called from driver->remove() */
	if (data == NULL)
		return;

	step = rilmodem_test_engine_get_current_step(gc->engined);

	g_assert(step->type == TST_EVENT_CALL);
	g_assert(step->call_func == (void (*)(void))
						ofono_gprs_context_set_data);

	if (step->check_func != NULL)
		((void (*)(struct ofono_gprs_context *, void *))
						step->check_func)(gc, data);

	gc->driver_data = data;

	rilmodem_test_engine_next_step(gc->engined);
}

void *ofono_gprs_context_get_data(struct ofono_gprs_context *gc)
{
	return gc->driver_data;
}

void mtk_reset_modem(struct ofono_modem *modem)
{
}

struct ofono_modem *ofono_gprs_context_get_modem(struct ofono_gprs_context *gc)
{
	return NULL;
}

struct ofono_atom *__ofono_modem_find_atom(struct ofono_modem *modem,
						enum ofono_atom_type type)
{
	return &gprs_atom;
}

void *__ofono_atom_get_data(struct ofono_atom *atom)
{
	return &atom->gprs;
}

void *ofono_gprs_get_data(struct ofono_gprs *gprs)
{
	return &gprs->gprs_data;
}

OFONO_EVENT_CALL_ARG_2(ofono_gprs_context_deactivated,
				struct ofono_gprs_context *, unsigned int)
OFONO_EVENT_CALL_ARG_2(ofono_gprs_context_set_interface,
				struct ofono_gprs_context *, const char *)
OFONO_EVENT_CALL_ARG_2(ofono_gprs_context_set_ipv4_netmask,
				struct ofono_gprs_context *, const char *)
OFONO_EVENT_CALL_ARG_3(ofono_gprs_context_set_ipv4_address,
						struct ofono_gprs_context *,
						const char *, ofono_bool_t)
OFONO_EVENT_CALL_ARG_2(ofono_gprs_context_set_ipv4_gateway,
				struct ofono_gprs_context *, const char *)
OFONO_EVENT_CALL_ARG_2(ofono_gprs_context_set_ipv4_dns_servers,
				struct ofono_gprs_context *, const char **)
OFONO_EVENT_CALL_CB_ARG_2(ofono_gprs_context_cb,
			const struct ofono_error *, struct ofono_gprs_context *)

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

static void check_gprs_context_set_data_1_1(struct ofono_gprs_context *gc,
								void *data)
{
	g_assert(data != NULL);
}

static void call_activate_primary_1_2(gpointer data)
{
	struct ofono_gprs_context *gc = data;
	struct ofono_gprs_primary_context primary = {
		.cid = 0,
		.direction = 0,
		.apn = "gprs.pepephone.com",
		.username = "",
		.password = "",
		.proto = OFONO_GPRS_PROTO_IP,
		.auth_method = OFONO_GPRS_AUTH_METHOD_CHAP
	};

	gcdriver->activate_primary(gc, &primary, ofono_gprs_context_cb, gc);

	rilmodem_test_engine_next_step(gc->engined);
}

/* REQUEST_SETUP_DATA_CALL, seq 1 */
static const char parcel_req_setup_data_call_1_3[] = {
	0x00, 0x00, 0x00, 0x6c, 0x1b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
	0x67, 0x00, 0x70, 0x00, 0x72, 0x00, 0x73, 0x00, 0x2e, 0x00, 0x70, 0x00,
	0x65, 0x00, 0x70, 0x00, 0x65, 0x00, 0x70, 0x00, 0x68, 0x00, 0x6f, 0x00,
	0x6e, 0x00, 0x65, 0x00, 0x2e, 0x00, 0x63, 0x00, 0x6f, 0x00, 0x6d, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x49, 0x00, 0x50, 0x00,
	0x00, 0x00, 0x00, 0x00
};

/*
 * Response to REQUEST_SETUP_DATA_CALL, seq 1,
 * {version=10,num=1 [status=0,retry=-1,cid=6,active=2,type=IP,ifname=rmnet5,
 * address=10.57.49.18,dns=80.58.61.250 80.58.61.254,gateways=10.57.49.1]}
 */
static const char parcel_rsp_setup_data_call_1_4[] = {
	0x00, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x06, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x49, 0x00, 0x50, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x72, 0x00, 0x6d, 0x00,
	0x6e, 0x00, 0x65, 0x00, 0x74, 0x00, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0b, 0x00, 0x00, 0x00, 0x31, 0x00, 0x30, 0x00, 0x2e, 0x00, 0x35, 0x00,
	0x37, 0x00, 0x2e, 0x00, 0x34, 0x00, 0x39, 0x00, 0x2e, 0x00, 0x31, 0x00,
	0x38, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x38, 0x00, 0x30, 0x00,
	0x2e, 0x00, 0x35, 0x00, 0x38, 0x00, 0x2e, 0x00, 0x36, 0x00, 0x31, 0x00,
	0x2e, 0x00, 0x32, 0x00, 0x35, 0x00, 0x30, 0x00, 0x20, 0x00, 0x38, 0x00,
	0x30, 0x00, 0x2e, 0x00, 0x35, 0x00, 0x38, 0x00, 0x2e, 0x00, 0x36, 0x00,
	0x31, 0x00, 0x2e, 0x00, 0x32, 0x00, 0x35, 0x00, 0x34, 0x00, 0x00, 0x00,
	0x0a, 0x00, 0x00, 0x00, 0x31, 0x00, 0x30, 0x00, 0x2e, 0x00, 0x35, 0x00,
	0x37, 0x00, 0x2e, 0x00, 0x34, 0x00, 0x39, 0x00, 0x2e, 0x00, 0x31, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};

static void check_gprs_context_set_interface_1_5(struct ofono_gprs_context *gc,
							const char *interface)
{
	g_assert_cmpstr(interface, ==, "rmnet5");
}

static void check_gprs_context_set_ipv4_netmask_1_6(
			struct ofono_gprs_context *gc, const char *netmask)
{
	g_assert_cmpstr(netmask, ==, "255.255.255.0");
}

static void check_gprs_context_set_ipv4_address_1_7(
						struct ofono_gprs_context *gc,
						const char *address,
						ofono_bool_t static_ip)
{
	g_assert_cmpstr(address, ==, "10.57.49.18");
	g_assert(static_ip == TRUE);
}

static void check_gprs_context_set_ipv4_gateway_1_8(
						struct ofono_gprs_context *gc,
						const char *gateway)
{
	g_assert_cmpstr(gateway, ==, "10.57.49.1");
}

static void check_gprs_context_set_ipv4_dns_servers_1_9(
						struct ofono_gprs_context *gc,
						const char **dns)
{
	g_assert_cmpstr(*dns, ==, "80.58.61.250");
	++dns;
	g_assert_cmpstr(*dns, ==, "80.58.61.254");
	++dns;
	g_assert(*dns == NULL);
}

static void check_activate_primary_1_10(const struct ofono_error *error,
						struct ofono_gprs_context *gc)
{
	g_assert(error->type == OFONO_ERROR_TYPE_NO_ERROR);
}

static void call_deactivate_primary_1_11(gpointer data)
{
	struct ofono_gprs_context *gc = data;

	gcdriver->deactivate_primary(gc, 0, ofono_gprs_context_cb, gc);

	rilmodem_test_engine_next_step(gc->engined);
}

/* REQUEST_DEACTIVATE_DATA_CALL, seq 2, cid=6 */
static const char parcel_req_deactivate_data_call_1_12[] = {
	0x00, 0x00, 0x00, 0x1c, 0x29, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00
};

/* Response to REQUEST_DEACTIVATE_DATA_CALL, seq 2 */
static const char parcel_rsp_deactivate_data_call_1_13[] = {
	0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static void check_deactivate_primary_1_14(const struct ofono_error *error,
						struct ofono_gprs_context *gc)
{
	g_assert(error->type == OFONO_ERROR_TYPE_NO_ERROR);
}

/*
 * --- TEST 1 ---
 * Step 1: Driver calls ofono_gprs_context_set_data(data)
 * Step 2: Call driver->activate_primary
 * Step 3: Driver sends SETUP_DATA_CALL
 * Step 4: Send response to SETUP_DATA_CALL
 * Step 5: Driver calls to ofono_gprs_context_set_interface
 * Step 6: Driver calls to ofono_gprs_context_set_ipv4_netmask
 * Step 7: Driver calls to ofono_gprs_context_set_ipv4_address
 * Step 8: Driver calls to ofono_gprs_context_set_ipv4_gateway
 * Step 9: Driver calls to ofono_gprs_context_set_ipv4_dns_servers
 * Step 10: Driver calls callback from step 2
 * Step 11: Call driver->deactivate_primary
 * Step 12: Driver sends DEACTIVATE_DATA_CALL
 * Step 13: Send response to DEACTIVATE_DATA_CALL
 * Step 14: Driver calls callback from step 11
 */
static const struct rilmodem_test_step steps_test_1[] = {
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_set_data,
		.check_func = (void (*)(void)) check_gprs_context_set_data_1_1
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_activate_primary_1_2
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_setup_data_call_1_3,
		.parcel_size = sizeof(parcel_req_setup_data_call_1_3)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_setup_data_call_1_4,
		.parcel_size = sizeof(parcel_rsp_setup_data_call_1_4)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_set_interface,
		.check_func =
			(void (*)(void)) check_gprs_context_set_interface_1_5
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_netmask,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_netmask_1_6
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_address,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_address_1_7
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_gateway,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_gateway_1_8
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void))
					ofono_gprs_context_set_ipv4_dns_servers,
		.check_func = (void (*)(void))
				check_gprs_context_set_ipv4_dns_servers_1_9
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_cb,
		.check_func = (void (*)(void)) check_activate_primary_1_10
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_deactivate_primary_1_11
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_deactivate_data_call_1_12,
		.parcel_size = sizeof(parcel_req_deactivate_data_call_1_12)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_deactivate_data_call_1_13,
		.parcel_size = sizeof(parcel_rsp_deactivate_data_call_1_13)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_cb,
		.check_func = (void (*)(void)) check_deactivate_primary_1_14
	},
};

static const struct rilmodem_test_data test_1 = {
	.steps = steps_test_1,
	.num_steps = G_N_ELEMENTS(steps_test_1)
};

static void call_detach_shutdown_2_11(gpointer data)
{
	struct ofono_gprs_context *gc = data;

	gcdriver->detach_shutdown(gc, 0);

	rilmodem_test_engine_next_step(gc->engined);
}

static void check_gprs_context_deactivated_2_14(struct ofono_gprs_context *gc,
							unsigned int cid)
{
	g_assert(cid == 0);
}

/*
 * --- TEST 2 ---
 * Step 1-10: Same as test 1
 * Step 11: Call driver->detach_shutdown
 * Step 12-13: Same as test 1
 * Step 14: Driver calls ofono_gprs_context_deactivated
 */
static const struct rilmodem_test_step steps_test_2[] = {
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_set_data,
		.check_func = (void (*)(void)) check_gprs_context_set_data_1_1
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_activate_primary_1_2
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_setup_data_call_1_3,
		.parcel_size = sizeof(parcel_req_setup_data_call_1_3)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_setup_data_call_1_4,
		.parcel_size = sizeof(parcel_rsp_setup_data_call_1_4)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_set_interface,
		.check_func =
			(void (*)(void)) check_gprs_context_set_interface_1_5
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_netmask,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_netmask_1_6
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_address,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_address_1_7
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_gateway,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_gateway_1_8
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void))
					ofono_gprs_context_set_ipv4_dns_servers,
		.check_func = (void (*)(void))
				check_gprs_context_set_ipv4_dns_servers_1_9
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_cb,
		.check_func = (void (*)(void)) check_activate_primary_1_10
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_detach_shutdown_2_11
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_deactivate_data_call_1_12,
		.parcel_size = sizeof(parcel_req_deactivate_data_call_1_12)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_deactivate_data_call_1_13,
		.parcel_size = sizeof(parcel_rsp_deactivate_data_call_1_13)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_deactivated,
		.check_func = (void (*)(void))
					check_gprs_context_deactivated_2_14
	},
};

static const struct rilmodem_test_data test_2 = {
	.steps = steps_test_2,
	.num_steps = G_N_ELEMENTS(steps_test_2)
};

/* UNSOL_DATA_CALL_LIST_CHANGED, seq 2, v6, no calls */
static const char parcel_unsol_data_call_list_changed_3_11[] = {
	0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00, 0xf2, 0x03, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void check_gprs_context_deactivated_3_12(struct ofono_gprs_context *gc,
							unsigned int cid)
{
	g_assert(cid == 0);
}

/*
 * --- TEST 3 ---
 * Step 1-10: Same as test 1
 * Step 11: Send UNSOL_DATA_CALL_LIST_CHANGED
 * Step 12: Driver calls ofono_gprs_context_deactivated
 */
static const struct rilmodem_test_step steps_test_3[] = {
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_set_data,
		.check_func = (void (*)(void)) check_gprs_context_set_data_1_1
	},
	{
		.type = TST_ACTION_CALL,
		.call_action = call_activate_primary_1_2
	},
	{
		.type = TST_EVENT_RECEIVE,
		.parcel_data = parcel_req_setup_data_call_1_3,
		.parcel_size = sizeof(parcel_req_setup_data_call_1_3)
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_rsp_setup_data_call_1_4,
		.parcel_size = sizeof(parcel_rsp_setup_data_call_1_4)
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_set_interface,
		.check_func =
			(void (*)(void)) check_gprs_context_set_interface_1_5
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_netmask,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_netmask_1_6
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_address,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_address_1_7
	},
	{
		.type = TST_EVENT_CALL,
		.call_func =
			(void (*)(void)) ofono_gprs_context_set_ipv4_gateway,
		.check_func =
			(void (*)(void)) check_gprs_context_set_ipv4_gateway_1_8
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void))
					ofono_gprs_context_set_ipv4_dns_servers,
		.check_func = (void (*)(void))
				check_gprs_context_set_ipv4_dns_servers_1_9
	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_cb,
		.check_func = (void (*)(void)) check_activate_primary_1_10
	},
	{
		.type = TST_ACTION_SEND,
		.parcel_data = parcel_unsol_data_call_list_changed_3_11,
		.parcel_size = sizeof(parcel_unsol_data_call_list_changed_3_11)

	},
	{
		.type = TST_EVENT_CALL,
		.call_func = (void (*)(void)) ofono_gprs_context_deactivated,
		.check_func = (void (*)(void))
					check_gprs_context_deactivated_3_12
	},
};

static const struct rilmodem_test_data test_3 = {
	.steps = steps_test_3,
	.num_steps = G_N_ELEMENTS(steps_test_3)
};

static void server_connect_cb(gpointer data)
{
	struct ofono_gprs_context *gc = data;
	struct ril_gprs_context_data
		ctx = { gc->ril, NULL, OFONO_GPRS_CONTEXT_TYPE_INTERNET };
	int retval;

	/* This starts the test. First for this atom is a call to _set_data. */
	retval = gcdriver->probe(gc, OFONO_RIL_VENDOR_AOSP, &ctx);
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
	struct ofono_gprs_context *gc;

	ril_gprs_context_init();

	gc = g_malloc0(sizeof(*gc));

	gc->engined = rilmodem_test_engine_create(&server_connect_cb,
							test_data, gc);

	gc->ril = g_ril_new(rilmodem_test_engine_get_socket_name(gc->engined),
							OFONO_RIL_VENDOR_AOSP);
	g_assert(gc->ril != NULL);

	/* Perform test */
	rilmodem_test_engine_start(gc->engined);

	gcdriver->remove(gc);
	g_ril_unref(gc->ril);
	g_free(gc);

	rilmodem_test_engine_remove(gc->engined);

	ril_gprs_context_exit();
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
	g_test_add_data_func("/test-rilmodem-gprs-context/1",
							&test_1, test_function);
	g_test_add_data_func("/test-rilmodem-gprs-context/2",
							&test_2, test_function);
	g_test_add_data_func("/test-rilmodem-gprs-context/3",
							&test_3, test_function);
#endif
	return g_test_run();
}
