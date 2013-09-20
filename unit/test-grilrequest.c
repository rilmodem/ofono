/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013 Canonical Ltd.
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

#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <glib.h>
#include <errno.h>

#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include <ofono/types.h>

#include "grilrequest.h"

struct request_test_data {
	gconstpointer request;
	guchar *parcel_data;
	gsize parcel_size;
};

/*
 * TODO: It may make sense to split this file into
 * domain-specific files ( eg. test-grilrequest-gprs-context.c )
 * once more tests are added.
 */

static const struct req_deactivate_data_call req_deact_data_call_invalid_1 = {
	.cid = 1,
	.reason = 10,
};

/*
 * The following hexadecimal data represents a serialized Binder parcel
 * instance containing a valid RIL_REQUEST_DEACTIVATE_DATA_CALL message
 * with the following parameters:
 *
 * (cid=1,reason=0)
 */
static const guchar req_deact_data_call_valid_parcel1[20] = {
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00
};

static const struct req_deactivate_data_call req_deact_data_call_valid_1 = {
	.cid = 1,
	.reason = RIL_DEACTIVATE_DATA_CALL_NO_REASON,
};

static const struct request_test_data deact_data_call_valid_test_1 = {
	.request = &req_deact_data_call_valid_1,
	.parcel_data = (guchar *) &req_deact_data_call_valid_parcel1,
	.parcel_size = 20,
};


static const struct req_setup_data_call req_setup_data_call_invalid_1 = {
	.tech = RADIO_TECH_UNKNOWN,
};

static const struct req_setup_data_call req_setup_data_call_invalid_2 = {
	.tech = 2112,
};

static const struct req_setup_data_call req_setup_data_call_invalid_3 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = 5,
};

static const struct req_setup_data_call req_setup_data_call_invalid_4 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = NULL,
};

static const struct req_setup_data_call req_setup_data_call_invalid_5 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "",
};

static const struct req_setup_data_call req_setup_data_call_invalid_6 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "",
	.apn = "12345678901234567890123456789012345678901234567890"
	"123456789012345678901234567890123456789012345678901",
};

static const struct req_setup_data_call req_setup_data_call_invalid_7 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "test.apn",
	.auth_type = 4,
};

static const struct req_setup_data_call req_setup_data_call_invalid_8 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "test.apn",
	.auth_type = RIL_AUTH_BOTH,
	.protocol = 3,
};

static const struct req_setup_data_call req_setup_data_call_valid_1 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "test.apn",
	.username = NULL,
	.password = NULL,
	.auth_type = RIL_AUTH_BOTH,
	.protocol = OFONO_GPRS_PROTO_IP,

};

static const struct req_setup_data_call req_setup_data_call_valid_2 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "test.apn",
	.username = "",
	.password = "",
	.auth_type = RIL_AUTH_NONE,
	.protocol = OFONO_GPRS_PROTO_IP,
};

static const struct req_setup_data_call req_setup_data_call_valid_3 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "test.apn",
	.username = "phablet",
	.password = "phablet",
	.auth_type = RIL_AUTH_BOTH,
	.protocol = OFONO_GPRS_PROTO_IPV4V6,
};

static const struct req_setup_data_call req_setup_data_call_valid_4 = {
	.tech = RADIO_TECH_GPRS,
	.data_profile = RIL_DATA_PROFILE_DEFAULT,
	.apn = "test.apn",
	.username = "phablet",
	.password = "phablet",
	.auth_type = RIL_AUTH_BOTH,
	.protocol = OFONO_GPRS_PROTO_IPV6,
};

/*
 * The following hexadecimal data represents a serialized Binder parcel
 * instance containing a valid RIL_REQUEST_RADIO_POWER 'OFF' message.
 */
static const guchar req_power_off_valid_parcel1[8] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * The following hexadecimal data represents a serialized Binder parcel
 * instance containing a valid RIL_REQUEST_RADIO_POWER 'ON' message.
 */
static const guchar req_power_on_valid_parcel2[8] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const gboolean power_off = FALSE;
static const gboolean power_on = TRUE;

static const struct request_test_data power_valid_test_1 = {
	.request = &power_off,
	.parcel_data = (guchar *) &req_power_off_valid_parcel1,
	.parcel_size = 8,
};

static const struct request_test_data power_valid_test_2 = {
	.request = &power_on,
	.parcel_data = (guchar *) &req_power_on_valid_parcel2,
	.parcel_size = 8,
};

static void test_deactivate_data_call_invalid(gconstpointer data)
{
	const struct req_deactivate_data_call *request = data;
	gboolean result;
	struct parcel rilp;
	struct ofono_error error;

	/*
	 * No parcel_init needed, as these tests all fail during
	 * param validation
	 */
	result = g_ril_request_deactivate_data_call(NULL, request, &rilp, &error);
	g_assert(result == FALSE);
	g_assert(error.type == OFONO_ERROR_TYPE_FAILURE &&
			error.error == -EINVAL);
}

static void test_deactivate_data_call_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_deactivate_data_call *request = test_data->request;
	gboolean result;
	struct parcel rilp;
	struct ofono_error error;

	result = g_ril_request_deactivate_data_call(NULL, request, &rilp, &error);
	g_assert(result == TRUE);
	g_assert(error.type == OFONO_ERROR_TYPE_NO_ERROR &&
			error.error == 0);

	g_assert(!memcmp(rilp.data, test_data->parcel_data, test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_setup_data_call_invalid(gconstpointer data)
{
	const struct req_setup_data_call *request = data;
	gboolean result;
	struct parcel rilp;
	struct ofono_error error;

	/*
	 * No parcel_init needed, as these tests all fail during
	 * param validation
	 */
	result = g_ril_request_setup_data_call(NULL, request, &rilp, &error);
	g_assert(result == FALSE);
	g_assert(error.type == OFONO_ERROR_TYPE_FAILURE &&
			error.error == -EINVAL);
}

static void test_request_setup_data_call_valid(gconstpointer data)
{
	const struct req_setup_data_call *request = data;
	gboolean result;
	struct parcel rilp;
	struct ofono_error error;

	result = g_ril_request_setup_data_call(NULL, request, &rilp, &error);
	g_assert(result == TRUE);
	g_assert(error.type == OFONO_ERROR_TYPE_NO_ERROR &&
			error.error == 0);

	parcel_free(&rilp);

	/* TODO: add unit 3 tests to validate binary parcel result */
}

static void test_request_power_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const gboolean *online = test_data->request;
	struct parcel rilp;

	g_ril_request_power(NULL, *online, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data, test_data->parcel_size));

	parcel_free(&rilp);
}

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

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid DEACTIVATE_DATA_CALL Test 1",
				&req_deact_data_call_invalid_1,
				test_deactivate_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"valid DEACTIVATE_DATA_CALL Test 1",
				&deact_data_call_valid_test_1,
				test_deactivate_data_call_valid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 1",
				&req_setup_data_call_invalid_1,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 2",
				&req_setup_data_call_invalid_2,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 3",
				&req_setup_data_call_invalid_3,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 4",
				&req_setup_data_call_invalid_4,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 5",
				&req_setup_data_call_invalid_5,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 6",
				&req_setup_data_call_invalid_6,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 7",
				&req_setup_data_call_invalid_7,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"invalid SETUP_DATA_CALL Test 8",
				&req_setup_data_call_invalid_8,
				test_request_setup_data_call_invalid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"valid SETUP_DATA_CALL Test 1",
				&req_setup_data_call_valid_1,
				test_request_setup_data_call_valid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"valid SETUP_DATA_CALL Test 2",
				&req_setup_data_call_valid_2,
				test_request_setup_data_call_valid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"valid SETUP_DATA_CALL Test 3",
				&req_setup_data_call_valid_3,
				test_request_setup_data_call_valid);

	g_test_add_data_func("/testgrilrequest/gprs-context: "
				"valid SETUP_DATA_CALL Test 4",
				&req_setup_data_call_valid_4,
				test_request_setup_data_call_valid);

	g_test_add_data_func("/testgrilrequest/power: "
				"valid POWER Test 1",
				&power_valid_test_1,
				test_request_power_valid);

	g_test_add_data_func("/testgrilrequest/power: "
				"valid POWER Test 1",
				&power_valid_test_2,
				test_request_power_valid);

#endif
	return g_test_run();
}
