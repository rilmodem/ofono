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

#include "common.h"
#include "grilreply.h"

/*
 * TODO: It may make sense to split this file into
 * domain-specific files ( eg. test-grilreply-gprs-context.c )
 * once more tests are added.
 */

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

typedef struct reg_state_test reg_state_test;
struct reg_state_test {
	int status;
	int tech;
	const struct ril_msg msg;
};

typedef struct get_preferred_network_test get_preferred_network_test;
struct get_preferred_network_test {
	int preferred;
	const struct ril_msg msg;
};

struct query_facility_lock_test {
	int status;
	const struct ril_msg msg;
};

struct set_facility_lock_test {
	int retries;
	const struct ril_msg msg;
};

struct sim_password_test {
	int retries;
	enum ofono_sim_password_type passwd_type;
	const struct ril_msg msg;
};

/* Invalid RIL_REQUEST_DATA_REGISTRATION_STATE: buffer too small */
static const struct ril_msg reply_data_reg_state_invalid_1 = {
	.buf = "XYZ",
	.buf_len = 3,
	.unsolicited = FALSE,
	.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hex data represents a RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with an invalid number of parameters ( 0 ).
 */
static const guchar reply_data_reg_state_invalid_parcel2[] = {
	0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_data_reg_state_invalid_2 = {
	.buf = (gchar *) &reply_data_reg_state_invalid_parcel2,
	.buf_len = sizeof(reply_data_reg_state_invalid_parcel2),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hex data represents a RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with a null status parameter:
 *
 * {(null),1b3f,07eaf3dc,HSPA,(null),20}
 */
static const guchar reply_data_reg_state_invalid_parcel3[] = {
	0x06, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x04, 0x00, 0x00, 0x00,
	0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00, 0x65, 0x00, 0x61, 0x00,
	0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_data_reg_state_invalid_3 = {
	.buf = (gchar *) &reply_data_reg_state_invalid_parcel3,
	.buf_len = sizeof(reply_data_reg_state_invalid_parcel3),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_UNKNOWN (0)
 *
 * {registered,1b3f,07eaf3dc,UNKNOWN,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel1[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const reg_state_test data_reg_valid_1 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_UNKNOWN,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel1,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel1),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_GSM (16)
 *
 * {registered,1b3f,07eaf3dc,GSM,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel2[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x36, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00,
	0x32, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const reg_state_test data_reg_valid_2 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_GSM,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel2,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel2),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_GPRS (1)
 *
 * {registered,1b3f,07eaf3dc,GPRS,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel3[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const reg_state_test data_reg_valid_3 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_GPRS,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel3,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel3),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_EDGE (2)
 *
 * {registered,1b3f,07eaf3dc,EDGE,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel4[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const reg_state_test data_reg_valid_4 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_EDGE,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel4,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel4),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_UMTS (3)
 *
 * {registered,1b3f,07eaf3dc,UMTS,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel5[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const reg_state_test data_reg_valid_5 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_UMTS,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel5,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel5),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_HSDPA (9)
 *
 * {registered,1b3f,07eaf3dc,HSDPA,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel6[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x39, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const reg_state_test data_reg_valid_6 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
 	.tech = RADIO_TECH_HSDPA,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel6,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel6),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_HSUPA (10)
 *
 * {registered,1b3f,07eaf3dc,HSUPA,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel7[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00,
	0x32, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const reg_state_test data_reg_valid_7 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_HSUPA,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel7,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel7),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_HSPA (11)
 *
 * {registered,1b3f,07eaf3dc,HSPA,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel8[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x31, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00,
	0x32, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};


static const reg_state_test data_reg_valid_8 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_HSPA,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel8,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel8),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_LTE (14)
 *
 * {registered,1b3f,07eaf3dc,LTE,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel9[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x34, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00,
	0x32, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};


static const reg_state_test data_reg_valid_9 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_LTE,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel9,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel9),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid RIL_REQUEST_DATA_REGISTRATION_STATE
 * reply with the following parameters:
 *
 * RADIO_TECH_HSPAP (15)
 *
 * Note, as ofono currently doesn't define a bearer enum that represents HSPA+,
 * it's currently mapped to HSPA.
 *
 * {registered,1b3f,07eaf3dc,HSPAP,(null),20}
 */
static const guchar reply_data_reg_state_valid_parcel10[] = {
	0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x66, 0x00, 0x33, 0x00, 0x64, 0x00, 0x63, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x35, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x00, 0x00,
	0x32, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};


static const reg_state_test data_reg_valid_10 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_HSPAP,
	.msg = {
		.buf = (gchar *) &reply_data_reg_state_valid_parcel10,
		.buf_len = sizeof(reply_data_reg_state_valid_parcel10),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_DATA_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * TODO: investigate creation of a base reply, which could
 * then be modified for each test, as opposed to duplicating
 * the bulk of the data for each test.
 */

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_UMTS (3)
 *
 * {registered,1b3f,07eaf3dc,UMTS,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel1[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x37, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_1 = {
	.status = NETWORK_REGISTRATION_STATUS_REGISTERED,
	.tech = RADIO_TECH_UMTS,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel1,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel1),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 *
 * RADIO_TECH_GPRS (1)
 *
 * {unregistered,1b3f,07eaf3dc,GPRS,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel2[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x37, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_2 = {
	.status = (int) NETWORK_REGISTRATION_STATUS_NOT_REGISTERED,
	.tech = RADIO_TECH_GPRS,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel2,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel2),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_GSM (16)
 *
 * {searching,1b3f,07eaf3dc,GSM,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel3[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x36, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_3 = {
	.status = NETWORK_REGISTRATION_STATUS_SEARCHING,
	.tech = RADIO_TECH_GSM,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel3,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel3),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_EDGE (2)
 *
 * {denied,1b3f,07eaf3dc,EDGE,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel4[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_4 = {
	.status = NETWORK_REGISTRATION_STATUS_DENIED,
	.tech = RADIO_TECH_EDGE,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel4,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel4),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_UNKNOWN (0)
 *
 * {unknown,1b3f,07eaf3dc,UNKNOWN,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel5[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_5 = {
	.status = NETWORK_REGISTRATION_STATUS_UNKNOWN,
	.tech = RADIO_TECH_UNKNOWN,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel5,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel5),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_LTE (14)
 *
 * {roaming,1b3f,07eaf3dc,LTE,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel6[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x34, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_6 = {
	.status = NETWORK_REGISTRATION_STATUS_ROAMING,
	.tech = RADIO_TECH_LTE,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel6,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel6),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_HSDPA (9)
 *
 * {roaming,1b3f,07eaf3dc,HSDPA,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel7[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x39, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_7 = {
	.status = NETWORK_REGISTRATION_STATUS_ROAMING,
	.tech = RADIO_TECH_HSDPA,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel7,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel7),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_HSUPA (10)
 *
 * {roaming,1b3f,07eaf3dc,HSUPA,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel8[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_8 = {
	.status = NETWORK_REGISTRATION_STATUS_ROAMING,
	.tech = RADIO_TECH_HSUPA,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel8,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel8),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_HSPA (11)
 *
 * {roaming,1b3f,07eaf3dc,HSPA,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel9[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x31, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_9 = {
	.status = NETWORK_REGISTRATION_STATUS_ROAMING,
	.tech = RADIO_TECH_HSPA,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel9,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel9),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * RADIO_TECH_HSPAP (15)
 *
 * {roaming,1b3f,07eaf3dc,HSPAP,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel10[] = {
	0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x31, 0x00, 0x62, 0x00, 0x33, 0x00, 0x66, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x65, 0x00, 0x61, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x61, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x35, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00,
	0x35, 0x00, 0x00, 0x00
};

static const reg_state_test voice_reg_valid_10 = {
	.status = NETWORK_REGISTRATION_STATUS_ROAMING,
	.tech = RADIO_TECH_HSPAP,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel10,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel10),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hex data represents a valid
 * RIL_REQUEST_VOICE_REGISTRATION_STATE reply with the following parameters:
 *
 * {unregistered,(null),(null),UNKNOWN,(null),(null)}
 */
static const guchar reply_voice_reg_state_valid_parcel11[] = {
	0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static const reg_state_test voice_reg_valid_11 = {
	.status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED,
	.tech = RADIO_TECH_UNKNOWN,
	.msg = {
		.buf = (gchar *) &reply_voice_reg_state_valid_parcel11,
		.buf_len = sizeof(reply_voice_reg_state_valid_parcel11),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_VOICE_REGISTRATION_STATE,
		.serial_no = 0,
		.error = 0,
	}
};

static const struct ril_msg reply_operator_invalid_1 = {
	.buf = "",
	.buf_len = 0,
	.unsolicited = FALSE,
	.req = RIL_REQUEST_OPERATOR,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of an
 * invalid RIL_REQUEST_OPERATOR with an invald number of parameters
 *
 * {lalpha=AT&T, salpha=}
 */
static const guchar reply_operator_invalid_parcel2[] = {
	0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x41, 0x00, 0x54, 0x00,
	0x26, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_operator_invalid_2 = {
	.buf = (char *) &reply_operator_invalid_parcel2,
	.buf_len = sizeof(reply_operator_invalid_parcel2),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_OPERATOR,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a
 * valid RIL_REQUEST_OPERATOR with the following parameters:
 *
 * {lalpha=AT&T, salpha=, numeric=310410}
 */
static const guchar reply_operator_valid_parcel1[] = {
	0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x41, 0x00, 0x54, 0x00,
	0x26, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x33, 0x00, 0x31, 0x00,
	0x30, 0x00, 0x34, 0x00, 0x31, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_operator_valid_1 = {
	.buf = (char *) &reply_operator_valid_parcel1,
	.buf_len = sizeof(reply_operator_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_OPERATOR,
	.serial_no = 0,
	.error = 0,
};

static const struct ril_msg reply_avail_ops_invalid_1 = {
	.buf = "",
	.buf_len = 0,
	.unsolicited = FALSE,
	.req = RIL_REQUEST_QUERY_AVAILABLE_NETWORKS,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of an
 * invalid RIL_REQUEST_QUERY_AVAILABLE_NETWORKS with an invald
 * number of strings ( not evenly divisible by 4, the count of
 * strings per operator ).
 */
static const guchar reply_avail_ops_invalid_parcel2[] = {
	0x0b, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_avail_ops_invalid_2 = {
	.buf = (char *) &reply_avail_ops_invalid_parcel2,
	.buf_len = sizeof(reply_avail_ops_invalid_parcel2),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_QUERY_AVAILABLE_NETWORKS,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a
 * valid RIL_REQUEST_AVAILABLE_NETWORKS with the following parameters:
 *
 * {lalpha=AT&T, salpha=, numeric=310410, status=available}
 */
static const guchar reply_avail_ops_valid_parcel1[] = {
	0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x41, 0x00, 0x54, 0x00,
	0x26, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x33, 0x00, 0x31, 0x00,
	0x30, 0x00, 0x34, 0x00, 0x31, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x61, 0x00, 0x76, 0x00, 0x61, 0x00, 0x69, 0x00,
	0x6c, 0x00, 0x61, 0x00, 0x62, 0x00, 0x6c, 0x00, 0x65, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_avail_ops_valid_1 = {
	.buf = (char *) &reply_avail_ops_valid_parcel1,
	.buf_len = sizeof(reply_avail_ops_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_QUERY_AVAILABLE_NETWORKS,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_SIM_IO reply with the following parameters:
 *
 * {sw1=0x90,sw2=0x00,0000000a2fe2040000000005020000}
 * This is a reply to a select file for EF_ICCID.
 */
static const guchar reply_sim_io_valid_parcel1[] = {
	0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x61, 0x00, 0x32, 0x00, 0x66, 0x00, 0x65, 0x00, 0x32, 0x00,
	0x30, 0x00, 0x34, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x35, 0x00,
	0x30, 0x00, 0x32, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_sim_io_valid_1 = {
	.buf = (gchar *) reply_sim_io_valid_parcel1,
	.buf_len = sizeof(reply_sim_io_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_SIM_IO,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of an valid
 * RIL_REQUEST_SIM_IO reply with the following parameters:
 *
 * {sw1=0x90,sw2=0x00,(null)}
 * This is a reply to a select file for EF_ICCID.
 */
static const guchar reply_sim_io_valid_parcel2[] = {
	0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};

static const struct ril_msg reply_sim_io_valid_2 = {
	.buf = (gchar *) reply_sim_io_valid_parcel2,
	.buf_len = sizeof(reply_sim_io_valid_parcel2),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_SIM_IO,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of an invalid
 * RIL_REQUEST_SIM_IO reply with the following parameters:
 *
 * Note - this is invalid because the response includes a non-hex char ('Z').
 *
 * {sw1=0x90,sw2=0x00,Z000000a2fe2040000000005020000}
 * This is a reply to a select file for EF_ICCID.
 */
static const guchar reply_sim_io_invalid_parcel1[] = {
	0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00,
	0x5A, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x61, 0x00, 0x32, 0x00, 0x66, 0x00, 0x65, 0x00, 0x32, 0x00,
	0x30, 0x00, 0x34, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x35, 0x00,
	0x30, 0x00, 0x32, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_sim_io_invalid_1 = {
	.buf = (gchar *) reply_sim_io_invalid_parcel1,
	.buf_len = sizeof(reply_sim_io_invalid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_SIM_IO,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of an invalid
 * RIL_REQUEST_SIM_IO reply with the following parameters:
 *
 * {sw1=0x90,sw2=0x00,<malformed length>}
 * This is a reply to a select file for EF_ICCID.
 */
static const guchar reply_sim_io_invalid_parcel2[] = {
	0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff
};

static const struct ril_msg reply_sim_io_invalid_2 = {
	.buf = (gchar *) reply_sim_io_invalid_parcel2,
	.buf_len = sizeof(reply_sim_io_invalid_parcel2),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_SIM_IO,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_IMSI reply with the following parameters:
 *
 * {214060200695834}
 */
static const guchar reply_imsi_valid_parcel1[] = {
	0x0f, 0x00, 0x00, 0x00, 0x32, 0x00, 0x31, 0x00, 0x34, 0x00, 0x30, 0x00,
	0x36, 0x00, 0x30, 0x00, 0x32, 0x00, 0x30, 0x00, 0x30, 0x00, 0x36, 0x00,
	0x39, 0x00, 0x35, 0x00, 0x38, 0x00, 0x33, 0x00, 0x34, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_imsi_valid_1 = {
	.buf = (gchar *) reply_imsi_valid_parcel1,
	.buf_len = sizeof(reply_imsi_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_IMSI,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_SIM_STATUS reply with the following parameters:
 *
 * {card_state=1,universal_pin_state=0,gsm_umts_index=0,cdma_index=-1,
 *  ims_index=-1, [app_type=1,app_state=5,perso_substate=2,aid_ptr=,
 *  app_label_ptr=(null),pin1_replaced=0,pin1=3,pin2=1],}
 */
static const guchar reply_sim_status_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_sim_status_valid_1 = {
	.buf = (gchar *) reply_sim_status_valid_parcel1,
	.buf_len = sizeof(reply_sim_status_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_SIM_STATUS,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_SMSC_ADDRESS reply with the following parameters:
 *
 * {type=145,number=34607003110}
 */
static const guchar reply_get_smsc_address_valid_parcel1[] = {
	0x12, 0x00, 0x00, 0x00, 0x22, 0x00, 0x2b, 0x00, 0x33, 0x00, 0x34, 0x00,
	0x36, 0x00, 0x30, 0x00, 0x37, 0x00, 0x30, 0x00, 0x30, 0x00, 0x33, 0x00,
	0x31, 0x00, 0x31, 0x00, 0x30, 0x00, 0x22, 0x00, 0x2c, 0x00, 0x31, 0x00,
	0x34, 0x00, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_get_smsc_address_valid_1 = {
	.buf = (gchar *) reply_get_smsc_address_valid_parcel1,
	.buf_len = sizeof(reply_get_smsc_address_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_SMSC_ADDRESS,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_CURRENT_CALLS reply with the following parameters:
 *
 * {[id=2,status=0,type=1,number=686732222,name=]
 *  [id=1,status=1,type=1,number=917525555,name=]}
 */
static const guchar reply_get_current_calls_valid_parcel1[] = {
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x36, 0x00, 0x38, 0x00, 0x36, 0x00, 0x37, 0x00,
	0x33, 0x00, 0x32, 0x00, 0x32, 0x00, 0x32, 0x00, 0x32, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x39, 0x00, 0x31, 0x00, 0x37, 0x00, 0x35, 0x00,
	0x32, 0x00, 0x35, 0x00, 0x35, 0x00, 0x35, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_get_current_calls_valid_1 = {
	.buf = (gchar *) reply_get_current_calls_valid_parcel1,
	.buf_len = sizeof(reply_get_current_calls_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_CURRENT_CALLS,
	.serial_no = 0,
	.error = 0,
};

/* RIL_REQUEST_GET_CURRENT_CALLS NULL reply */
static const struct ril_msg reply_get_current_calls_invalid_1 = {
	.buf = NULL,
	.buf_len = 0,
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_CURRENT_CALLS,
	.serial_no = 0,
	.error = 0,
};

/* RIL_REQUEST_GET_CURRENT_CALLS no calls */
static const guchar reply_get_current_calls_invalid_parcel2[] = {
	0x00, 0x00, 0x00, 0x00,
};

static const struct ril_msg reply_get_current_calls_invalid_2 = {
	.buf = (gchar *) reply_get_current_calls_invalid_parcel2,
	.buf_len = sizeof(reply_get_current_calls_invalid_parcel2),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_CURRENT_CALLS,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_LAST_CALL_FAIL_CAUSE reply with the following parameters:
 *
 * {16}
 */
static const guchar reply_call_fail_cause_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_call_fail_cause_valid_1 = {
	.buf = (gchar *) reply_call_fail_cause_valid_parcel1,
	.buf_len = sizeof(reply_call_fail_cause_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_LAST_CALL_FAIL_CAUSE,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_MUTE reply with the following parameters:
 *
 * {muted=0}
 */
static const guchar reply_get_mute_off_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_get_mute_off_1 = {
	.buf = (gchar *) reply_get_mute_off_parcel1,
	.buf_len = sizeof(reply_get_mute_off_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_MUTE,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_MUTE reply with the following parameters:
 *
 * {muted=1}
 */
static const guchar reply_get_mute_on_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_get_mute_on_1 = {
	.buf = (gchar *) reply_get_mute_on_parcel1,
	.buf_len = sizeof(reply_get_mute_on_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_MUTE,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_BASEBAND_VERSION reply with the following parameters:
 *
 * {M9615A-CEFWMAZM-2.0.1700.48}
 */
static const guchar reply_baseband_version_valid_parcel1[] = {
	0x1b, 0x00, 0x00, 0x00, 0x4d, 0x00, 0x39, 0x00, 0x36, 0x00, 0x31, 0x00,
	0x35, 0x00, 0x41, 0x00, 0x2d, 0x00, 0x43, 0x00, 0x45, 0x00, 0x46, 0x00,
	0x57, 0x00, 0x4d, 0x00, 0x41, 0x00, 0x5a, 0x00, 0x4d, 0x00, 0x2d, 0x00,
	0x32, 0x00, 0x2e, 0x00, 0x30, 0x00, 0x2e, 0x00, 0x31, 0x00, 0x37, 0x00,
	0x30, 0x00, 0x30, 0x00, 0x2e, 0x00, 0x34, 0x00, 0x38, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_baseband_version_valid_1 = {
	.buf = (gchar *) reply_baseband_version_valid_parcel1,
	.buf_len = sizeof(reply_baseband_version_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_BASEBAND_VERSION,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_IMEI reply with the following parameters:
 *
 * {355136050779043}
 */
static const guchar reply_get_imei_valid_parcel1[] = {
	0x0f, 0x00, 0x00, 0x00, 0x33, 0x00, 0x35, 0x00, 0x35, 0x00, 0x31, 0x00,
	0x33, 0x00, 0x36, 0x00, 0x30, 0x00, 0x35, 0x00, 0x30, 0x00, 0x37, 0x00,
	0x37, 0x00, 0x39, 0x00, 0x30, 0x00, 0x34, 0x00, 0x33, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_get_imei_valid_1 = {
	.buf = (gchar *) reply_get_imei_valid_parcel1,
	.buf_len = sizeof(reply_get_imei_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_IMEI,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_QUERY_CALL_WAITING reply with the following parameters:
 *
 * {1,0x30}
 */
static const guchar reply_query_call_waiting_valid_parcel1[] = {
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_query_call_waiting_valid_1 = {
	.buf = (gchar *) reply_query_call_waiting_valid_parcel1,
	.buf_len = sizeof(reply_query_call_waiting_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_QUERY_CALL_WAITING,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_QUERY_CLIP reply with the following parameters:
 *
 * {1}
 */
static const guchar reply_query_clip_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_query_clip_valid_1 = {
	.buf = (gchar *) reply_query_clip_valid_parcel1,
	.buf_len = sizeof(reply_query_clip_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_QUERY_CLIP,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_CLIR reply with the following parameters:
 *
 * {2,4}
 */
static const guchar reply_get_clir_valid_parcel1[] = {
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00
};

static const struct ril_msg reply_get_clir_valid_1 = {
	.buf = (gchar *) reply_get_clir_valid_parcel1,
	.buf_len = sizeof(reply_get_clir_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_GET_CLIR,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE reply with the following parameters:
 *
 * {0}
 */
static const guchar reply_get_preferred_network_type_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const get_preferred_network_test
		reply_get_preferred_network_type_valid_1 = {
	.preferred = 0,
	.msg = {
		.buf = (gchar *) reply_get_preferred_network_type_valid_parcel1,
		.buf_len =
			sizeof(reply_get_preferred_network_type_valid_parcel1),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_QUERY_FACILITY_LOCK reply with the following parameters:
 *
 * {0}
 */
static const guchar reply_query_facility_lock_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct query_facility_lock_test
				reply_query_facility_lock_valid_1 = {
	.status = 0,
	.msg = {
		.buf = (gchar *) reply_query_facility_lock_valid_parcel1,
		.buf_len = sizeof(reply_query_facility_lock_valid_parcel1),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_QUERY_FACILITY_LOCK,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following hexadecimal data contains the event data of a valid
 * RIL_REQUEST_QUERY_FACILITY_LOCK reply with the following parameters:
 *
 * {0,0} (infineon: two integers are returned)
 */
static const guchar reply_query_facility_lock_valid_parcel2[] = {
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct query_facility_lock_test
				reply_query_facility_lock_valid_2 = {
	.status = 0,
	.msg = {
		.buf = (gchar *) reply_query_facility_lock_valid_parcel2,
		.buf_len = sizeof(reply_query_facility_lock_valid_parcel2),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_QUERY_FACILITY_LOCK,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following structure contains test data for a valid
 * RIL_REQUEST_SET_FACILITY_LOCK reply with no parameters.
 */
static const struct set_facility_lock_test reply_set_facility_lock_valid_1 = {
	.retries = -1,
	.msg = {
		.buf = NULL,
		.buf_len = 0,
		.unsolicited = FALSE,
		.req = RIL_REQUEST_SET_FACILITY_LOCK,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following structure contains test data for a valid
 * RIL_REQUEST_SET_FACILITY_LOCK reply with parameter {2}
 */
static const guchar reply_set_facility_lock_valid_parcel2[] = {
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
};

static const struct set_facility_lock_test reply_set_facility_lock_valid_2 = {
	.retries = 2,
	.msg = {
		.buf = (gchar *) reply_set_facility_lock_valid_parcel2,
		.buf_len = sizeof(reply_set_facility_lock_valid_parcel2),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_SET_FACILITY_LOCK,
		.serial_no = 0,
		.error = 0,
	}
};

/*
 * The following structure contains test data for a valid
 * RIL_REQUEST_ENTER_SIM_PIN reply with parameter {0}
 */
static const guchar reply_enter_sim_pin_valid_parcel1[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct sim_password_test reply_enter_sim_pin_valid_1 = {
	.retries = -1,
	.passwd_type = OFONO_SIM_PASSWORD_SIM_PIN,
	.msg = {
		.buf = (gchar *) reply_enter_sim_pin_valid_parcel1,
		.buf_len = sizeof(reply_enter_sim_pin_valid_parcel1),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_ENTER_SIM_PIN,
		.serial_no = 0,
		.error = RIL_E_SUCCESS,
	}
};

/*
 * The following structure contains test data for a valid
 * RIL_REQUEST_ENTER_SIM_PIN reply with parameter {2}
 */
static const guchar reply_enter_sim_pin_valid_parcel2[] = {
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
};

static const struct sim_password_test reply_enter_sim_pin_valid_2 = {
	.retries = 2,
	.passwd_type = OFONO_SIM_PASSWORD_SIM_PIN,
	.msg = {
		.buf = (gchar *) reply_enter_sim_pin_valid_parcel2,
		.buf_len = sizeof(reply_enter_sim_pin_valid_parcel2),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_ENTER_SIM_PIN,
		.serial_no = 0,
		.error = RIL_E_PASSWORD_INCORRECT,
	}
};

static void test_reply_reg_state_invalid(gconstpointer data)
{
	struct reply_reg_state *reply =	g_ril_reply_parse_reg_state(NULL, data);
	g_assert(reply == NULL);
}

static void test_reply_reg_state_valid(gconstpointer data)
{
	const reg_state_test *test = data;
	struct reply_reg_state *reply =
		g_ril_reply_parse_reg_state(NULL, &test->msg);

	g_assert(reply != NULL);
	g_assert(reply->status == test->status);

	g_assert(reply->tech == test->tech);
	g_free(reply);
}

static void test_reply_operator_invalid(gconstpointer data)
{
	struct reply_operator *reply = g_ril_reply_parse_operator(NULL, data);
	g_assert(reply == NULL);
}

static void test_reply_operator_valid(gconstpointer data)
{
	struct reply_operator *reply = g_ril_reply_parse_operator(NULL, data);
	g_assert(reply != NULL);
}

static void test_reply_avail_ops_invalid(gconstpointer data)
{
	struct reply_avail_ops *reply = g_ril_reply_parse_avail_ops(NULL, data);
	g_assert(reply == NULL);
}

static void test_reply_avail_ops_valid(gconstpointer data)
{
	struct reply_avail_ops *reply = g_ril_reply_parse_avail_ops(NULL, data);
	g_assert(reply != NULL);
}

static void test_reply_sim_io_valid(gconstpointer data)
{
	struct reply_sim_io *reply = g_ril_reply_parse_sim_io(NULL, data);
	g_assert(reply != NULL);
	g_ril_reply_free_sim_io(reply);
}

static void test_reply_sim_io_invalid(gconstpointer data)
{
	struct reply_sim_io *reply = g_ril_reply_parse_sim_io(NULL, data);
	g_assert(reply == NULL);
}

static void test_reply_imsi_valid(gconstpointer data)
{
	gchar *reply = g_ril_reply_parse_imsi(NULL, data);
	g_assert(reply != NULL);
	g_free(reply);
}

static void test_reply_sim_status_valid(gconstpointer data)
{
	struct reply_sim_status *reply;

	reply = g_ril_reply_parse_sim_status(NULL, data);
	g_assert(reply != NULL);
	g_ril_reply_free_sim_status(reply);
}

static void test_reply_get_smsc_address_valid(gconstpointer data)
{
	struct ofono_phone_number *reply;

	reply = g_ril_reply_parse_get_smsc_address(NULL, data);

	g_assert(reply != NULL);
	g_free(reply);
}

static void test_reply_get_current_calls_valid(gconstpointer data)
{
	GSList *calls;

	calls = g_ril_reply_parse_get_calls(NULL, data);

	g_assert(calls != NULL);

	g_slist_foreach(calls, (GFunc) g_free, NULL);
	g_slist_free(calls);
}

static void test_reply_get_current_calls_invalid(gconstpointer data)
{
	GSList *calls;

	calls = g_ril_reply_parse_get_calls(NULL, data);

	g_assert(calls == NULL);
}

static void test_reply_call_fail_cause_valid(gconstpointer data)
{
	enum ofono_disconnect_reason reason;

	reason = g_ril_reply_parse_call_fail_cause(NULL, data);

	g_assert(reason >= 0);
}

static void test_reply_get_mute_off(gconstpointer data)
{
	int muted;

	muted = g_ril_reply_parse_get_mute(NULL, data);

	g_assert(muted == 0);
}

static void test_reply_get_mute_on(gconstpointer data)
{
	int muted;

	muted = g_ril_reply_parse_get_mute(NULL, data);

	g_assert(muted == 1);
}

static void test_reply_baseband_version_valid(gconstpointer data)
{
	char *version;

	version = g_ril_reply_parse_baseband_version(NULL, data);

	g_assert(version != NULL);

	g_free(version);
}

static void test_reply_get_imei_valid(gconstpointer data)
{
	char *imei;

	imei = g_ril_reply_parse_get_imei(NULL, data);

	g_assert(imei != NULL);

	g_free(imei);
}

static void test_reply_query_call_waiting_valid(gconstpointer data)
{
	int cls;

	cls = g_ril_reply_parse_query_call_waiting(NULL, data);

	g_assert(cls != -1);
}

static void test_reply_query_clip_valid(gconstpointer data)
{
	int clip_status;

	clip_status = g_ril_reply_parse_query_clip(NULL, data);

	g_assert(clip_status != -1);
}

static void test_reply_get_clir_valid(gconstpointer data)
{
	struct reply_clir *reply;

	reply = g_ril_reply_parse_get_clir(NULL, data);

	g_assert(reply != NULL);

	g_ril_reply_free_get_clir(reply);
}

static void test_reply_get_preferred_network_type_valid(gconstpointer data)
{
	const get_preferred_network_test *test = data;
	int type =
		g_ril_reply_parse_get_preferred_network_type(NULL, &test->msg);

	g_assert(type == test->preferred);
}

static void test_reply_query_facility_lock_valid(gconstpointer data)
{
	const struct query_facility_lock_test *test = data;
	int status = g_ril_reply_parse_query_facility_lock(NULL, &test->msg);

	g_assert(status == test->status);
}

static void test_reply_set_facility_lock_valid(gconstpointer data)
{
	const struct set_facility_lock_test *test = data;
	int retries = g_ril_reply_parse_set_facility_lock(NULL, &test->msg);

	g_assert(retries == test->retries);
}

static void test_reply_enter_sim_pin_valid(gconstpointer data)
{
	const struct sim_password_test *test = data;
	int *retries = g_ril_reply_parse_retries(NULL, &test->msg,
							test->passwd_type);

	g_assert(retries != NULL);
	g_assert(retries[test->passwd_type] == test->retries);

	g_free(retries);
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

	g_test_add_data_func("/testgrilreply/gprs: "
				"invalid DATA_REG_STATE Test 1",
				&reply_data_reg_state_invalid_1,
				test_reply_reg_state_invalid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"invalid DATA_REG_STATE Test 2",
				&reply_data_reg_state_invalid_2,
				test_reply_reg_state_invalid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"invalid DATA_REG_STATE Test 3",
				&reply_data_reg_state_invalid_3,
				test_reply_reg_state_invalid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 1",
				&data_reg_valid_1,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 2",
				&data_reg_valid_2,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 3",
				&data_reg_valid_3,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 4",
				&data_reg_valid_4,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 5",
				&data_reg_valid_5,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 6",
				&data_reg_valid_6,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 7",
				&data_reg_valid_7,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 8",
				&data_reg_valid_8,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 9",
				&data_reg_valid_9,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/gprs: "
				"valid DATA_REG_STATE Test 10",
				&data_reg_valid_10,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 1",
				&voice_reg_valid_1,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 2",
				&voice_reg_valid_2,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 3",
				&voice_reg_valid_3,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 4",
				&voice_reg_valid_4,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 5",
				&voice_reg_valid_5,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 6",
				&voice_reg_valid_6,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 7",
				&voice_reg_valid_7,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 8",
				&voice_reg_valid_8,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 9",
				&voice_reg_valid_9,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 10",
				&voice_reg_valid_10,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid VOICE_REG_STATE Test 11",
				&voice_reg_valid_11,
				test_reply_reg_state_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"invalid GET_OPERATOR Test 1",
				&reply_operator_invalid_1,
				test_reply_operator_invalid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"invalid GET_OPERATOR Test 2",
				&reply_operator_invalid_2,
				test_reply_operator_invalid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid GET_OPERATOR Test 1",
				&reply_operator_valid_1,
				test_reply_operator_valid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"invalid QUERY_AVAIL_OPS Test 1",
				&reply_avail_ops_invalid_1,
				test_reply_avail_ops_invalid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"invalid QUERY_AVAIL_OPS Test 2",
				&reply_avail_ops_invalid_2,
				test_reply_avail_ops_invalid);

	g_test_add_data_func("/testgrilreply/netreg: "
				"valid QUERY_AVAIL_OPS Test 1",
				&reply_avail_ops_valid_1,
				test_reply_avail_ops_valid);

	g_test_add_data_func("/testgrilreply/sim: "
				"valid SIM_IO Test 1",
				&reply_sim_io_valid_1,
				test_reply_sim_io_valid);

	g_test_add_data_func("/testgrilreply/sim: "
				"valid SIM_IO Test 2",
				&reply_sim_io_valid_2,
				test_reply_sim_io_valid);

	g_test_add_data_func("/testgrilreply/sim: "
				"invalid SIM_IO Test 1",
				&reply_sim_io_invalid_1,
				test_reply_sim_io_invalid);

	g_test_add_data_func("/testgrilreply/sim: "
				"invalid SIM_IO Test 2",
				&reply_sim_io_invalid_2,
				test_reply_sim_io_invalid);

	g_test_add_data_func("/testgrilreply/sim: "
				"valid GET_IMSI Test 1",
				&reply_imsi_valid_1,
				test_reply_imsi_valid);

	g_test_add_data_func("/testgrilreply/sim: "
				"valid GET_SIM_STATUS Test 1",
				&reply_sim_status_valid_1,
				test_reply_sim_status_valid);

	g_test_add_data_func("/testgrilreply/sim: "
				"valid GET_SMSC_ADDRESS Test 1",
				&reply_get_smsc_address_valid_1,
				test_reply_get_smsc_address_valid);

	g_test_add_data_func("/testgrilreply/voicecall: "
				"valid GET_CURRENT_CALLS Test 1",
				&reply_get_current_calls_valid_1,
				test_reply_get_current_calls_valid);

	g_test_add_data_func("/testgrilreply/voicecall: "
				"invalid GET_CURRENT_CALLS Test 1",
				&reply_get_current_calls_invalid_1,
				test_reply_get_current_calls_invalid);

	g_test_add_data_func("/testgrilreply/voicecall: "
				"invalid GET_CURRENT_CALLS Test 2",
				&reply_get_current_calls_invalid_2,
				test_reply_get_current_calls_invalid);

	g_test_add_data_func("/testgrilreply/voicecall: "
				"valid CALL_FAIL_CAUSE Test 1",
				&reply_call_fail_cause_valid_1,
				test_reply_call_fail_cause_valid);

	g_test_add_data_func("/testgrilreply/call-volume: "
				"off GET_MUTE Test 1",
				&reply_get_mute_off_1,
				test_reply_get_mute_off);

	g_test_add_data_func("/testgrilreply/call-volume: "
				"on GET_MUTE Test 1",
				&reply_get_mute_on_1,
				test_reply_get_mute_on);

	g_test_add_data_func("/testgrilreply/devinfo: "
				"valid BASEBAND_VERSION Test 1",
				&reply_baseband_version_valid_1,
				test_reply_baseband_version_valid);

	g_test_add_data_func("/testgrilreply/devinfo: "
				"valid GET_IMEI Test 1",
				&reply_get_imei_valid_1,
				test_reply_get_imei_valid);

	g_test_add_data_func("/testgrilreply/call-settings: "
				"valid QUERY_CALL_WAITING Test 1",
				&reply_query_call_waiting_valid_1,
				test_reply_query_call_waiting_valid);

	g_test_add_data_func("/testgrilreply/call-settings: "
				"valid QUERY_CLIP Test 1",
				&reply_query_clip_valid_1,
				test_reply_query_clip_valid);

	g_test_add_data_func("/testgrilreply/call-settings: "
				"valid GET_CLIR Test 1",
				&reply_get_clir_valid_1,
				test_reply_get_clir_valid);

	g_test_add_data_func("/testgrilreply/radio-settings: "
				"valid GET_PREFERRED_NETWORK_TYPE Test 1",
				&reply_get_preferred_network_type_valid_1,
				test_reply_get_preferred_network_type_valid);

	g_test_add_data_func("/testgrilreply/call-barring: "
				"valid QUERY_FACILITY_LOCK Test 1",
				&reply_query_facility_lock_valid_1,
				test_reply_query_facility_lock_valid);

	g_test_add_data_func("/testgrilreply/call-barring: "
				"valid QUERY_FACILITY_LOCK Test 2",
				&reply_query_facility_lock_valid_2,
				test_reply_query_facility_lock_valid);

	g_test_add_data_func("/testgrilreply/call-barring: "
				"valid SET_FACILITY_LOCK Test 1",
				&reply_set_facility_lock_valid_1,
				test_reply_set_facility_lock_valid);

	g_test_add_data_func("/testgrilreply/call-barring: "
				"valid SET_FACILITY_LOCK Test 2",
				&reply_set_facility_lock_valid_2,
				test_reply_set_facility_lock_valid);

	g_test_add_data_func("/testgrilreply/sim: "
				"valid ENTER_SIM_PIN Test 1",
				&reply_enter_sim_pin_valid_1,
				test_reply_enter_sim_pin_valid);

	g_test_add_data_func("/testgrilreply/sim: "
				"valid ENTER_SIM_PIN Test 2",
				&reply_enter_sim_pin_valid_2,
				test_reply_enter_sim_pin_valid);

#endif

	return g_test_run();
}
