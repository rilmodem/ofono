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

/*
 * TODO: It may make sense to split this file into
 * domain-specific files ( eg. test-grilrequest-gprs-context.c )
 * once more tests are added.
 */

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

struct request_test_data {
	gconstpointer request;
	const guchar *parcel_data;
	gsize parcel_size;
};

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

/* sim_write_binary tests */

static const guchar req_sim_write_binary_parcel_valid_1[] = {
	0xd6, 0x00, 0x00, 0x00, 0xcb, 0x6f, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
	0x33, 0x00, 0x46, 0x00, 0x30, 0x00, 0x30, 0x00, 0x37, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x31, 0x00,
	0x38, 0x00, 0x30, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x20, 0x00, 0x00, 0x00, 0x61, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x38, 0x00, 0x37, 0x00,
	0x31, 0x00, 0x30, 0x00, 0x30, 0x00, 0x32, 0x00, 0x66, 0x00, 0x66, 0x00,
	0x34, 0x00, 0x34, 0x00, 0x66, 0x00, 0x66, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x38, 0x00, 0x39, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char sim_write_binary_path_valid_1[] =
	{ 0x3F, 0x00, 0x7F, 0xFF };

static const struct req_sim_write_binary req_sim_write_binary_valid_1 = {
	.app_type = RIL_APPTYPE_UNKNOWN,
	.aid_str = "a0000000871002ff44ff128900000100",
	.fileid = 0x6FCB,
	.path = sim_write_binary_path_valid_1,
	.path_len = sizeof(sim_write_binary_path_valid_1),
	.start = 0,
	.length = 16,
	.data = (unsigned char[]) {
		0x01, 0x00, 0x01, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
};

static const struct request_test_data sim_write_binary_valid_test_1 = {
	.request = &req_sim_write_binary_valid_1,
	.parcel_data = (guchar *) &req_sim_write_binary_parcel_valid_1,
	.parcel_size = sizeof(req_sim_write_binary_parcel_valid_1),
};

/* sim_write_record tests */

static const guchar req_sim_write_record_parcel_valid_1[] = {
	0xdc, 0x00, 0x00, 0x00, 0xcb, 0x6f, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
	0x33, 0x00, 0x46, 0x00, 0x30, 0x00, 0x30, 0x00, 0x37, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x31, 0x00,
	0x38, 0x00, 0x30, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00,
	0x46, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x20, 0x00, 0x00, 0x00, 0x61, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x38, 0x00, 0x37, 0x00,
	0x31, 0x00, 0x30, 0x00, 0x30, 0x00, 0x32, 0x00, 0x66, 0x00, 0x66, 0x00,
	0x34, 0x00, 0x34, 0x00, 0x66, 0x00, 0x66, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x38, 0x00, 0x39, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char sim_write_record_path_valid_1[] =
	{0x3F, 0x00, 0x7F, 0xFF};

static const struct req_sim_write_record req_sim_write_record_valid_1 = {
	.app_type = RIL_APPTYPE_UNKNOWN,
	.aid_str = "a0000000871002ff44ff128900000100",
	.fileid = 0x6FCB,
	.path = sim_write_record_path_valid_1,
	.path_len = sizeof(sim_write_record_path_valid_1),
	.mode = GRIL_REC_ACCESS_MODE_ABSOLUTE,
	.record = 1,
	.length = 16,
	.data = (unsigned char[]) {
		0x01, 0x00, 0x01, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
};

static const struct request_test_data sim_write_record_valid_test_1 = {
	.request = &req_sim_write_record_valid_1,
	.parcel_data = (guchar *) &req_sim_write_record_parcel_valid_1,
	.parcel_size = sizeof(req_sim_write_record_parcel_valid_1),
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

/* sms_cmgs tests */

static const unsigned char req_sms_cmgs_pdu_valid_1[] = {
	0x00, 0x11, 0x00, 0x09, 0x81, 0x36, 0x54, 0x39, 0x80, 0xf5, 0x00, 0x00,
	0xa7, 0x0a, 0xc8, 0x37, 0x3b, 0x0c, 0x6a, 0xd7, 0xdd, 0xe4, 0x37
};

static const guchar req_sms_cmgs_parcel_valid_1[] = {
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

static const struct req_sms_cmgs req_sms_cmgs_valid1 = {
	.pdu = req_sms_cmgs_pdu_valid_1,
	.pdu_len = sizeof(req_sms_cmgs_pdu_valid_1),
	.tpdu_len = sizeof(req_sms_cmgs_pdu_valid_1) - 1,
};

static const struct request_test_data sms_cmgs_valid_test_1 = {
	.request = &req_sms_cmgs_valid1,
	.parcel_data = (guchar *) &req_sms_cmgs_parcel_valid_1,
	.parcel_size = sizeof(req_sms_cmgs_parcel_valid_1),
};

/* sms_acknowledge tests */

static const guchar req_sms_acknowledge_parcel_valid_1[] = {
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct request_test_data sms_acknowledge_valid_test_1 = {
	.request = NULL,
	.parcel_data = (guchar *) &req_sms_acknowledge_parcel_valid_1,
	.parcel_size = sizeof(req_sms_acknowledge_parcel_valid_1),
};

/* set_smsc_address tests */

static const guchar req_set_smsc_address_valid_parcel1[] = {
	0x0e, 0x00, 0x00, 0x00, 0x22, 0x00, 0x2b, 0x00, 0x33, 0x00, 0x34, 0x00,
	0x36, 0x00, 0x30, 0x00, 0x37, 0x00, 0x30, 0x00, 0x30, 0x00, 0x33, 0x00,
	0x31, 0x00, 0x31, 0x00, 0x30, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct ofono_phone_number smsc_address_valid1 = {
	.type = 145,
	.number = "34607003110"
};

static const struct request_test_data smsc_address_valid_test_1 = {
	.request = &smsc_address_valid1,
	.parcel_data = (guchar *) &req_set_smsc_address_valid_parcel1,
	.parcel_size = sizeof(req_set_smsc_address_valid_parcel1),
};

/* dial tests */

struct request_test_dial_data {
	const struct ofono_phone_number ph;
	enum ofono_clir_option clir;
	const guchar *parcel_data;
	size_t parcel_size;
};

static const guchar req_dial_parcel_valid_1[] = {
	0x09, 0x00, 0x00, 0x00, 0x39, 0x00, 0x31, 0x00, 0x37, 0x00, 0x35, 0x00,
	0x32, 0x00, 0x35, 0x00, 0x35, 0x00, 0x35, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const struct request_test_dial_data dial_valid_test_1 = {
	.ph = { .number = "917525555", .type = 129 },
	.clir = OFONO_CLIR_OPTION_DEFAULT,
	.parcel_data = req_dial_parcel_valid_1,
	.parcel_size = sizeof(req_dial_parcel_valid_1),
};

/* hangup tests */

static const guchar req_hangup_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static unsigned hangup_call_id_valid_1 = 1;

static const struct request_test_data set_hangup_valid_test_1 = {
	.request = &hangup_call_id_valid_1,
	.parcel_data = req_hangup_parcel_valid_1,
	.parcel_size = sizeof(req_hangup_parcel_valid_1),
};

/* dtmf tests */

static const guchar req_dtmf_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00
};

static char dtmf_char_valid_1 = '4';

static const struct request_test_data dtmf_valid_test_1 = {
	.request = &dtmf_char_valid_1,
	.parcel_data = req_dtmf_parcel_valid_1,
	.parcel_size = sizeof(req_dtmf_parcel_valid_1),
};

/* separate_conn tests */

static const guchar req_separate_conn_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static unsigned separate_conn_call_id_valid_1 = 1;

static const struct request_test_data separate_conn_valid_test_1 = {
	.request = &separate_conn_call_id_valid_1,
	.parcel_data = req_separate_conn_parcel_valid_1,
	.parcel_size = sizeof(req_separate_conn_parcel_valid_1),
};

/* set_supp_svc_notif tests */

static const guchar req_set_supp_svc_notif_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_data set_supp_svc_notif_valid_test_1 = {
	.request = NULL,
	.parcel_data = req_set_supp_svc_notif_parcel_valid_1,
	.parcel_size = sizeof(req_set_supp_svc_notif_parcel_valid_1),
};

/* set_mute tests */

static const int mute_off = 0;
static const int mute_on = 1;

static const guchar req_set_mute_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct request_test_data set_mute_valid_test_1 = {
	.request = &mute_off,
	.parcel_data = (guchar *) &req_set_mute_valid_parcel1,
	.parcel_size = sizeof(req_set_mute_valid_parcel1),
};

static const guchar req_set_mute_valid_parcel2[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_data set_mute_valid_test_2 = {
	.request = &mute_on,
	.parcel_data = (guchar *) &req_set_mute_valid_parcel2,
	.parcel_size = sizeof(req_set_mute_valid_parcel2),
};

/* send_ussd tests */

static const guchar req_send_ussd_parcel_valid_1[] = {
	0x05, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x31, 0x00, 0x31, 0x00, 0x38, 0x00,
	0x23, 0x00, 0x00, 0x00
};

static const struct request_test_data send_ussd_valid_test_1 = {
	.request = "*118#",
	.parcel_data = req_send_ussd_parcel_valid_1,
	.parcel_size = sizeof(req_send_ussd_parcel_valid_1),
};

/* set_call_waiting tests */

struct request_test_set_call_waiting {
	int enabled;
	int serviceclass;
	const char *parcel_data;
	size_t parcel_size;
};

static const char req_set_call_waiting_parcel_valid_1[] = {
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_set_call_waiting
		set_call_waiting_valid_test_1 = {
	.enabled = 0,
	.serviceclass = 0x01,
	.parcel_data = req_set_call_waiting_parcel_valid_1,
	.parcel_size = sizeof(req_set_call_waiting_parcel_valid_1),
};

/* query_call_waiting tests */

const int query_call_waiting_mode_0 = 0;

static const guchar req_query_call_waiting_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct request_test_data query_call_waiting_valid_test_1 = {
	.request = &query_call_waiting_mode_0,
	.parcel_data = req_query_call_waiting_parcel_valid_1,
	.parcel_size = sizeof(req_query_call_waiting_parcel_valid_1),
};

/* set_clir tests */

const int set_clir_mode_0 = 0;

static const guchar req_set_clir_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct request_test_data set_clir_valid_test_1 = {
	.request = &set_clir_mode_0,
	.parcel_data = req_set_clir_parcel_valid_1,
	.parcel_size = sizeof(req_set_clir_parcel_valid_1),
};

/* screen_state tests */

const int screen_state_0 = 0;
const int screen_state_1 = 1;

static const guchar req_screen_state_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct request_test_data screen_state_valid_test_1 = {
	.request = &screen_state_0,
	.parcel_data = req_screen_state_parcel_valid_1,
	.parcel_size = sizeof(req_screen_state_parcel_valid_1),
};

static const guchar req_screen_state_parcel_valid_2[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_data screen_state_valid_test_2 = {
	.request = &screen_state_1,
	.parcel_data = req_screen_state_parcel_valid_2,
	.parcel_size = sizeof(req_screen_state_parcel_valid_2),
};

/* set_preferred_network_type tests */

const int preferred_network_type_gsm_only = PREF_NET_TYPE_GSM_ONLY;

static const guchar req_set_preferred_network_type_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_data
		set_preferred_network_type_valid_test_1 = {
	.request = &preferred_network_type_gsm_only,
	.parcel_data = req_set_preferred_network_type_valid_1,
	.parcel_size = sizeof(req_set_preferred_network_type_valid_1),
};

/* query_facility_lock tests */

struct request_test_query_facility_lock_data {
	const char *facility;
	int services;
	const guchar *parcel_data;
	gsize parcel_size;
};

static const guchar req_query_facility_lock_valid_1[] = {
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x41, 0x00, 0x4f, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};

static const struct request_test_query_facility_lock_data
		query_facility_lock_valid_test_1 = {
	.facility = "AO",
	.services = SERVICE_CLASS_NONE,
	.parcel_data = req_query_facility_lock_valid_1,
	.parcel_size = sizeof(req_query_facility_lock_valid_1),
};

/* set_facility_lock tests */

struct request_test_set_facility_lock_data {
	const char *facility;
	int enable;
	const char *passwd;
	int services;
	const guchar *parcel_data;
	gsize parcel_size;
};

static const guchar req_set_facility_lock_valid_1[] = {
	0x05, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4f, 0x00, 0x49, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff
};

static const struct request_test_set_facility_lock_data
		set_facility_lock_valid_test_1 = {
	.facility = "OI",
	.enable = 0,
	.passwd = "0000",
	.services = SERVICE_CLASS_NONE,
	.parcel_data = req_set_facility_lock_valid_1,
	.parcel_size = sizeof(req_set_facility_lock_valid_1),
};

/* change_barring_password tests */

struct request_test_change_barring_password_data {
	const char *facility;
	const char *old_passwd;
	const char *new_passwd;
	const guchar *parcel_data;
	gsize parcel_size;
};

static const guchar req_change_barring_password_valid_1[] = {
	0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x41, 0x00, 0x42, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x31, 0x00,
	0x31, 0x00, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct request_test_change_barring_password_data
		change_barring_password_valid_test_1 = {
	.facility = "AB",
	.old_passwd = "1111",
	.new_passwd = "0000",
	.parcel_data = req_change_barring_password_valid_1,
	.parcel_size = sizeof(req_change_barring_password_valid_1),
};

/* oem_hook_raw tests */

struct request_test_oem_hook_raw_data {
	const guchar *data;
	gsize size;
	const guchar *parcel_data;
	gsize parcel_size;
};

static const guchar req_oem_hook_raw_valid_1[] = {
	0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct request_test_oem_hook_raw_data oem_hook_raw_valid_test_1 = {
	.data = req_oem_hook_raw_valid_1 + sizeof(int32_t),
	.size = sizeof(req_oem_hook_raw_valid_1) - sizeof(int32_t),
	.parcel_data = (guchar *) &req_oem_hook_raw_valid_1,
	.parcel_size = sizeof(req_oem_hook_raw_valid_1),
};

static const guchar req_oem_hook_raw_valid_2[] = {
	0xFF, 0xFF, 0xFF, 0xFF
};

static const struct request_test_oem_hook_raw_data oem_hook_raw_valid_test_2 = {
	.data = NULL,
	.size = 0,
	.parcel_data = (guchar *) &req_oem_hook_raw_valid_2,
	.parcel_size = sizeof(req_oem_hook_raw_valid_2),
};

/*
 * The following hexadecimal data represents a serialized Binder parcel
 * instance containing a valid RIL_REQUEST_RADIO_POWER 'OFF' message.
 */
static const guchar req_power_off_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * The following hexadecimal data represents a serialized Binder parcel
 * instance containing a valid RIL_REQUEST_RADIO_POWER 'ON' message.
 */
static const guchar req_power_on_valid_parcel2[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const gboolean power_off = FALSE;
static const gboolean power_on = TRUE;

static const struct request_test_data power_valid_test_1 = {
	.request = &power_off,
	.parcel_data = (guchar *) &req_power_off_valid_parcel1,
	.parcel_size = sizeof(req_power_off_valid_parcel1),
};

static const struct request_test_data power_valid_test_2 = {
	.request = &power_on,
	.parcel_data = (guchar *) &req_power_on_valid_parcel2,
	.parcel_size = sizeof(req_power_on_valid_parcel2),
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

static void test_request_sim_write_binary_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sim_write_binary *req = test_data->request;
	struct parcel rilp;
	gboolean result;

	result = g_ril_request_sim_write_binary(NULL, req, &rilp);

	g_assert(result == TRUE);
	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_sim_write_record_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sim_write_record *req = test_data->request;
	struct parcel rilp;
	gboolean result;

	result = g_ril_request_sim_write_record(NULL, req, &rilp);

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

static void test_request_sms_cmgs(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sms_cmgs *req = test_data->request;
	struct parcel rilp;

	g_ril_request_sms_cmgs(NULL, req, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_sms_acknowledge(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	struct parcel rilp;

	g_ril_request_sms_acknowledge(NULL, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_set_smsc_address(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct ofono_phone_number *number = test_data->request;
	struct parcel rilp;

	g_ril_request_set_smsc_address(NULL, number, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_dial(gconstpointer data)
{
	const struct request_test_dial_data *test_data = data;
	const struct ofono_phone_number *ph = &test_data->ph;
	enum ofono_clir_option clir = test_data->clir;
	struct parcel rilp;

	g_ril_request_dial(NULL, ph, clir, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_hangup(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const unsigned *call_id = test_data->request;
	struct parcel rilp;

	g_ril_request_hangup(NULL, *call_id, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_dtmf(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const char *dtmf_char = test_data->request;
	struct parcel rilp;

	g_ril_request_dtmf(NULL, *dtmf_char, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_separate_conn(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const unsigned *call_id = test_data->request;
	struct parcel rilp;

	g_ril_request_separate_conn(NULL, *call_id, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_set_supp_svc_notif(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	struct parcel rilp;

	g_ril_request_set_supp_svc_notif(NULL, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
			test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_set_mute_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const int *muted = test_data->request;
	struct parcel rilp;

	g_ril_request_set_mute(NULL, *muted, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_send_ussd(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	struct parcel rilp;

	g_ril_request_send_ussd(NULL, test_data->request, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_set_call_waiting(gconstpointer data)
{
	const struct request_test_set_call_waiting *test_data = data;
	struct parcel rilp;

	g_ril_request_set_call_waiting(NULL, test_data->enabled,
					test_data->serviceclass, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_query_call_waiting(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	int mode = *(int *) test_data->request;
	struct parcel rilp;

	g_ril_request_query_call_waiting(NULL, mode, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_set_clir(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	int mode = *(int *) test_data->request;
	struct parcel rilp;

	g_ril_request_set_clir(NULL, mode, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_screen_state(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	int state = *(int *) test_data->request;
	struct parcel rilp;

	g_ril_request_screen_state(NULL, state, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_set_preferred_network_type(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	int preferred_network_type = *(int *) test_data->request;
	struct parcel rilp;

	g_ril_request_set_preferred_network_type(NULL, preferred_network_type,
							&rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_query_facility_lock(gconstpointer data)
{
	const struct request_test_query_facility_lock_data *test_data = data;
	struct parcel rilp;

	g_ril_request_query_facility_lock(NULL, test_data->facility, "",
						test_data->services, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_set_facility_lock(gconstpointer data)
{
	const struct request_test_set_facility_lock_data *test_data = data;
	struct parcel rilp;

	g_ril_request_set_facility_lock(NULL, test_data->facility,
					test_data->enable, test_data->passwd,
					test_data->services, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_change_barring_password(gconstpointer data)
{
	const struct request_test_change_barring_password_data *test_data =
									data;
	struct parcel rilp;

	g_ril_request_change_barring_password(NULL, test_data->facility,
						test_data->old_passwd,
						test_data->new_passwd, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
}

static void test_request_oem_hook_raw(gconstpointer data)
{
	const struct request_test_oem_hook_raw_data *test_data = data;
	struct parcel rilp;

	g_ril_request_oem_hook_raw(NULL, test_data->data,
					test_data->size, &rilp);

	g_assert(!memcmp(rilp.data, test_data->parcel_data,
				test_data->parcel_size));

	parcel_free(&rilp);
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

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid SIM_READ_INFO Test 1",
				&req_sim_read_info_valid_1,
				test_request_sim_read_info_valid);

	g_test_add_data_func("/testgrilrequest/sim: "
				"invalid SIM_READ_INFO Test 1",
				&req_sim_read_info_invalid_1,
				test_request_sim_read_info_invalid);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid SIM_READ_BINARY Test 1",
				&sim_read_binary_valid_test_1,
				test_request_sim_read_binary_valid);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid SIM_READ_RECORD Test 1",
				&sim_read_record_valid_test_1,
				test_request_sim_read_record_valid);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid SIM_WRITE_BINARY Test 1",
				&sim_write_binary_valid_test_1,
				test_request_sim_write_binary_valid);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid SIM_WRITE_RECORD Test 1",
				&sim_write_record_valid_test_1,
				test_request_sim_write_record_valid);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid READ_IMSI Test 1",
				&read_imsi_valid_test_1,
				test_request_read_imsi);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid PIN_SEND Test 1",
				&pin_send_record_valid_test_1,
				test_request_pin_send);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid PIN_CHANGE_STATE Test 1",
				&pin_change_state_valid_test_1,
				test_request_pin_change_state);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid PIN_SEND_PUK Test 1",
				&pin_send_puk_valid_test_1,
				test_request_pin_send_puk);

	g_test_add_data_func("/testgrilrequest/sim: "
				"valid CHANGE_PASSWD Test 1",
				&change_passwd_valid_test_1,
				test_request_change_passwd);

	g_test_add_data_func("/testgrilrequest/sms: "
				"valid SMS_CMGS Test 1",
				&sms_cmgs_valid_test_1,
				test_request_sms_cmgs);

	g_test_add_data_func("/testgrilrequest/sms: "
				"valid SMS_ACKNOWLEDGE Test 1",
				&sms_acknowledge_valid_test_1,
				test_request_sms_acknowledge);

	g_test_add_data_func("/testgrilrequest/sms: "
				"valid SET_SMSC_ADDRESS Test 1",
				&smsc_address_valid_test_1,
				test_request_set_smsc_address);

	g_test_add_data_func("/testgrilrequest/voicecall: "
				"valid DIAL Test 1",
				&dial_valid_test_1,
				test_request_dial);

	g_test_add_data_func("/testgrilrequest/voicecall: "
				"valid HANGUP Test 1",
				&set_hangup_valid_test_1,
				test_request_hangup);

	g_test_add_data_func("/testgrilrequest/voicecall: "
				"valid DTMF Test 1",
				&dtmf_valid_test_1,
				test_request_dtmf);

	g_test_add_data_func("/testgrilrequest/voicecall: "
				"valid SEPARATE_CONN Test 1",
				&separate_conn_valid_test_1,
				test_request_separate_conn);

	g_test_add_data_func("/testgrilrequest/voicecall: "
				"valid SET_SUPP_SVC_NOTIF Test 1",
				&set_supp_svc_notif_valid_test_1,
				test_request_set_supp_svc_notif);

	g_test_add_data_func("/testgrilrequest/call-volume: "
				"valid SET_MUTE Test 1",
				&set_mute_valid_test_1,
				test_request_set_mute_valid);

	g_test_add_data_func("/testgrilrequest/call-volume: "
				"valid SET_MUTE Test 2",
				&set_mute_valid_test_2,
				test_request_set_mute_valid);

	g_test_add_data_func("/testgrilrequest/ussd: "
				"valid SEND_USSD Test 1",
				&send_ussd_valid_test_1,
				test_request_send_ussd);

	g_test_add_data_func("/testgrilrequest/call-settings: "
				"valid SET_CALL_WAITING Test 1",
				&set_call_waiting_valid_test_1,
				test_request_set_call_waiting);

	g_test_add_data_func("/testgrilrequest/call-settings: "
				"valid QUERY_CALL_WAITING Test 1",
				&query_call_waiting_valid_test_1,
				test_request_query_call_waiting);

	g_test_add_data_func("/testgrilrequest/call-settings: "
				"valid SET_CLIR Test 1",
				&set_clir_valid_test_1,
				test_request_set_clir);

	g_test_add_data_func("/testgrilrequest/radio-settings: "
				"valid SCREEN_STATE Test 1",
				&screen_state_valid_test_1,
				test_request_screen_state);

	g_test_add_data_func("/testgrilrequest/radio-settings: "
				"valid SCREEN_STATE Test 2",
				&screen_state_valid_test_2,
				test_request_screen_state);

	g_test_add_data_func("/testgrilrequest/radio-settings: "
				"valid SET_PREFERRED_NETWORK_TYPE Test 1",
				&set_preferred_network_type_valid_test_1,
				test_request_set_preferred_network_type);

	g_test_add_data_func("/testgrilrequest/call-barring: "
				"valid QUERY_FACILITY_LOCK Test 1",
				&query_facility_lock_valid_test_1,
				test_request_query_facility_lock);

	g_test_add_data_func("/testgrilrequest/call-barring: "
				"valid SET_FACILITY_LOCK Test 1",
				&set_facility_lock_valid_test_1,
				test_request_set_facility_lock);

	g_test_add_data_func("/testgrilrequest/call-barring: "
				"valid CHANGE_BARRING_PASSWORD Test 1",
				&change_barring_password_valid_test_1,
				test_request_change_barring_password);

	g_test_add_data_func("/testgrilrequest/oem-hook-raw: "
				"valid OEM_HOOK_RAW Test 1",
				&oem_hook_raw_valid_test_1,
				test_request_oem_hook_raw);

	g_test_add_data_func("/testgrilrequest/oem-hook-raw: "
				"valid OEM_HOOK_RAW Test 2",
				&oem_hook_raw_valid_test_2,
				test_request_oem_hook_raw);

#endif
	return g_test_run();
}
