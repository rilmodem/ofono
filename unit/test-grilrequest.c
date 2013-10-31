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

#define _GNU_SOURCE
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
static const guchar req_deact_data_call_valid_parcel1[] = {
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
	.parcel_size = sizeof(req_deact_data_call_valid_parcel1),
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

static const char sim_read_info_path_valid_1[] = {0x3F, 0x00};

static const struct req_sim_read_info req_sim_read_info_valid_1 = {
	.app_type = RIL_APPTYPE_USIM,
	.aid_str = "1234567890123456",
	.fileid = 0x7F01,
	.path = (const unsigned char *) sim_read_info_path_valid_1,
	.path_len = sizeof(sim_read_info_path_valid_1),
};

static const unsigned char sim_read_info_path_invalid_1[] =
	{0x3F, 0x00, 0x11, 0x22, 0x7F, 0x00, 0x11, 0x22};

static const struct req_sim_read_info req_sim_read_info_invalid_1 = {
	.app_type = RIL_APPTYPE_ISIM + 10,
	.aid_str = "1234567890123456",
	.fileid = 0x7F01,
	.path = sim_read_info_path_invalid_1,
	.path_len = sizeof(sim_read_info_path_invalid_1),
};

/* sim_read_binary tests */

static const guchar req_sim_read_binary_parcel_valid_1[] = {
0xb0, 0x00, 0x00, 0x00, 0xe2, 0x2f, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
0x33, 0x00, 0x46, 0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00,
};

static const unsigned char sim_read_binary_path_valid_1[] = {0x3F, 0x00};

static const struct req_sim_read_binary req_sim_read_binary_valid_1 = {
	.app_type = RIL_APPTYPE_UNKNOWN,
	.aid_str = "",
	.fileid = 0x2FE2,
	.path = sim_read_binary_path_valid_1,
	.path_len = sizeof(sim_read_binary_path_valid_1),
	.start = 0,
	.length = 0x0A,
};

static const struct request_test_data sim_read_binary_valid_test_1 = {
	.request = &req_sim_read_binary_valid_1,
	.parcel_data = (guchar *) &req_sim_read_binary_parcel_valid_1,
	.parcel_size = sizeof(req_sim_read_binary_parcel_valid_1),
};

/* sim_read_record tests */

static const guchar req_sim_read_record_parcel_valid_1[] = {
0xb2, 0x00, 0x00, 0x00, 0xe2, 0x2f, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
0x33, 0x00, 0x46, 0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00,
};

static const unsigned char sim_read_record_path_valid_1[] = {0x3F, 0x00};

static const struct req_sim_read_record req_sim_read_record_valid_1 = {
	.app_type = RIL_APPTYPE_UNKNOWN,
	.aid_str = "",
	.fileid = 0x2FE2,
	.path = sim_read_record_path_valid_1,
	.path_len = sizeof(sim_read_record_path_valid_1),
	.record = 5,
	.length = 0x0A,
};

static const struct request_test_data sim_read_record_valid_test_1 = {
	.request = &req_sim_read_record_valid_1,
	.parcel_data = (guchar *) &req_sim_read_record_parcel_valid_1,
	.parcel_size = sizeof(req_sim_read_record_parcel_valid_1),
};

/* read_imsi tests */

static const guchar req_read_imsi_parcel_valid_1[] = {
0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const char read_imsi_aid_str_1[] = "";

static const struct request_test_data read_imsi_valid_test_1 = {
	.request = &read_imsi_aid_str_1,
	.parcel_data = (guchar *) &req_read_imsi_parcel_valid_1,
	.parcel_size = sizeof(req_read_imsi_parcel_valid_1),
};

/* pin_send tests */

struct request_test_pin_send_data {
	const char *passwd;
	const gchar *aid_str;
	guchar *parcel_data;
	gsize parcel_size;
};

static const guchar req_pin_send_parcel_valid_1[] = {
0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
0x33, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00,
};

static const struct request_test_pin_send_data pin_send_record_valid_test_1 = {
	.passwd = "1234",
	.aid_str = "",
	.parcel_data = (guchar *) &req_pin_send_parcel_valid_1,
	.parcel_size = sizeof(req_pin_send_parcel_valid_1),
};

/* pin_change_state tests */

static const guchar req_pin_change_state_valid_1[] = {
0x05, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x50, 0x00, 0x53, 0x00,
0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00, 0x34, 0x00,
0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
0xFF, 0xFF, 0xFF, 0xFF,
};

static const struct req_pin_change_state req_pin_change_state_valid1 = {
	.aid_str = NULL,
	.passwd_type = OFONO_SIM_PASSWORD_PHSIM_PIN,
	.enable = 0,
	.passwd = "1234",
};

static const struct request_test_data pin_change_state_valid_test_1 = {
	.request = &req_pin_change_state_valid1,
	.parcel_data = (guchar *) &req_pin_change_state_valid_1,
	.parcel_size = sizeof(req_pin_change_state_valid_1),
};

/* pin_send_puk tests */

struct request_test_pin_send_puk_data {
	const char *puk;
	const char *passwd;
	const gchar *aid_str;
	const guchar *parcel_data;
	gsize parcel_size;
};

static const guchar req_pin_send_puk_parcel_valid_1[] = {
0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
0x33, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00,
};

static const struct request_test_pin_send_puk_data pin_send_puk_valid_test_1 = {
	.puk = "12345678",
	.passwd = "1234",
	.aid_str = "",
	.parcel_data = req_pin_send_puk_parcel_valid_1,
	.parcel_size = sizeof(req_pin_send_puk_parcel_valid_1),
};

/* change_passwd tests */

struct request_test_change_passwd_data {
	const char *old_passwd;
	const char *new_passwd;
	const gchar *aid_str;
	const guchar *parcel_data;
	gsize parcel_size;
};

static const guchar req_change_passwd_parcel_valid_1[] = {
0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
0x33, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const struct request_test_change_passwd_data change_passwd_valid_test_1 = {
	.old_passwd = "1234",
	.new_passwd = "5678",
	.aid_str = "",
	.parcel_data = req_change_passwd_parcel_valid_1,
	.parcel_size = sizeof(req_change_passwd_parcel_valid_1),
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

static void test_request_sim_read_info_valid(gconstpointer data)
{
	const struct req_sim_read_info *req = data;
	gboolean result;
	struct parcel rilp;

	result = g_ril_request_sim_read_info(NULL, req, &rilp);
	g_assert(result == TRUE);

	parcel_free(&rilp);
}

static void test_request_sim_read_info_invalid(gconstpointer data)
{
	const struct req_sim_read_info *req = data;
	gboolean result;
	struct parcel rilp;

	result = g_ril_request_sim_read_info(NULL, req, &rilp);
	g_assert(result == FALSE);

	parcel_free(&rilp);
}

static void test_request_sim_read_binary_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sim_read_binary *req = test_data->request;
	struct parcel rilp;
	gboolean result;

	result = g_ril_request_sim_read_binary(NULL, req, &rilp);

	g_assert(result == TRUE);
	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_sim_read_record_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sim_read_record *req = test_data->request;
	struct parcel rilp;
	gboolean result;

	result = g_ril_request_sim_read_record(NULL, req, &rilp);

	g_assert(result == TRUE);
	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_read_imsi(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const char *aid_str = test_data->request;
	struct parcel rilp;

	g_ril_request_read_imsi(NULL, aid_str, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_pin_send(gconstpointer data)
{
	const struct request_test_pin_send_data *test_data = data;
	const char *passwd = test_data->passwd;
	const char *aid_str = test_data->aid_str;
	struct parcel rilp;

	g_ril_request_pin_send(NULL, passwd, aid_str, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_pin_change_state(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_pin_change_state *req = test_data->request;
	struct parcel rilp;
	gboolean result;

	result = g_ril_request_pin_change_state(NULL, req, &rilp);

	g_assert(result == TRUE);
	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_pin_send_puk(gconstpointer data)
{
	const struct request_test_pin_send_puk_data *test_data = data;
	const char *puk = test_data->puk;
	const char *passwd = test_data->passwd;
	const char *aid_str = test_data->aid_str;
	struct parcel rilp;

	g_ril_request_pin_send_puk(NULL, puk, passwd, aid_str, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_change_passwd(gconstpointer data)
{
	const struct request_test_change_passwd_data *test_data = data;
	const char *old_passwd = test_data->old_passwd;
	const char *new_passwd = test_data->new_passwd;
	const char *aid_str = test_data->aid_str;
	struct parcel rilp;

	g_ril_request_change_passwd(NULL, old_passwd, new_passwd,
					aid_str, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

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

	g_test_add_data_func("/testgrilrequest/sim_read_info: "
				"valid SIM_READ_INFO Test 1",
				&req_sim_read_info_valid_1,
				test_request_sim_read_info_valid);

	g_test_add_data_func("/testgrilrequest/sim_read_info: "
				"invalid SIM_READ_INFO Test 1",
				&req_sim_read_info_invalid_1,
				test_request_sim_read_info_invalid);

	g_test_add_data_func("/testgrilrequest/sim_read_binary: "
				"valid SIM_READ_BINARY Test 1",
				&sim_read_binary_valid_test_1,
				test_request_sim_read_binary_valid);

	g_test_add_data_func("/testgrilrequest/sim_read_record: "
				"valid SIM_READ_RECORD Test 1",
				&sim_read_record_valid_test_1,
				test_request_sim_read_record_valid);

	g_test_add_data_func("/testgrilrequest/read_imsi: "
				"valid READ_IMSI Test 1",
				&read_imsi_valid_test_1,
				test_request_read_imsi);

	g_test_add_data_func("/testgrilrequest/pin_send: "
				"valid PIN_SEND Test 1",
				&pin_send_record_valid_test_1,
				test_request_pin_send);

	g_test_add_data_func("/testgrilrequest/pin_change_state: "
				"valid PIN_CHANGE_STATE Test 1",
				&pin_change_state_valid_test_1,
				test_request_pin_change_state);

	g_test_add_data_func("/testgrilrequest/pin_send_puk: "
				"valid PIN_SEND_PUK Test 1",
				&pin_send_puk_valid_test_1,
				test_request_pin_send_puk);

	g_test_add_data_func("/testgrilrequest/change_passwd: "
				"valid CHANGE_PASSWD Test 1",
				&change_passwd_valid_test_1,
				test_request_change_passwd);

#endif
	return g_test_run();
}
