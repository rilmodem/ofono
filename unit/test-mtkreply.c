/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014 Canonical Ltd.
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
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

struct sim_password_test {
	int retries;
	enum ofono_sim_password_type passwd_type;
	const struct ril_msg msg;
};

/*
 * The following hexadecimal data contains the event data of a valid
 * MTK-specific RIL_REQUEST_AVAILABLE_NETWORKS with the following parameters:
 *
 * {lalpha=AT&T, salpha=, numeric=310410, status=available, tech=3G}
 */
static const guchar mtk_reply_avail_ops_valid_parcel1[] = {
	0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x41, 0x00, 0x54, 0x00,
	0x26, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x33, 0x00, 0x31, 0x00,
	0x30, 0x00, 0x34, 0x00, 0x31, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x61, 0x00, 0x76, 0x00, 0x61, 0x00, 0x69, 0x00,
	0x6c, 0x00, 0x61, 0x00, 0x62, 0x00, 0x6c, 0x00, 0x65, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x33, 0x00, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct ril_msg mtk_reply_avail_ops_valid_1 = {
	.buf = (char *) &mtk_reply_avail_ops_valid_parcel1,
	.buf_len = sizeof(mtk_reply_avail_ops_valid_parcel1),
	.unsolicited = FALSE,
	.req = RIL_REQUEST_QUERY_AVAILABLE_NETWORKS,
	.serial_no = 0,
	.error = 0,
};

/*
 * The following structure contains test data for a valid
 * RIL_REQUEST_ENTER_SIM_PIN reply with parameters {3,3,10,10}
 */
static const guchar mtk_reply_enter_sim_pin_valid_parcel1[] = {
	0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x0A, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00
};

static const struct sim_password_test mtk_reply_enter_sim_pin_valid_1 = {
	.retries = 3,
	.passwd_type = OFONO_SIM_PASSWORD_SIM_PIN,
	.msg = {
		.buf = (gchar *) mtk_reply_enter_sim_pin_valid_parcel1,
		.buf_len = sizeof(mtk_reply_enter_sim_pin_valid_parcel1),
		.unsolicited = FALSE,
		.req = RIL_REQUEST_ENTER_SIM_PIN,
		.serial_no = 0,
		.error = RIL_E_SUCCESS,
	}
};

static void test_mtk_reply_avail_ops_valid(gconstpointer data)
{
	struct reply_avail_ops *reply;
	GRil *gril = g_ril_new(NULL, OFONO_RIL_VENDOR_MTK);

	reply = g_ril_reply_parse_avail_ops(gril, data);
	g_assert(reply != NULL);

	g_ril_unref(gril);
}

static void test_mtk_reply_enter_sim_pin_valid(gconstpointer data)
{
	GRil *gril = g_ril_new(NULL, OFONO_RIL_VENDOR_MTK);
	const struct sim_password_test *test = data;
	int *retries = g_ril_reply_parse_retries(gril, &test->msg,
							test->passwd_type);

	g_assert(retries != NULL);
	g_assert(retries[test->passwd_type] == test->retries);

	g_free(retries);
	g_ril_unref(gril);
}

#endif	/* LITTLE_ENDIAN */

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

	g_test_add_data_func("/testmtkreply/netreg: "
				"valid QUERY_AVAIL_OPS Test 1",
				&mtk_reply_avail_ops_valid_1,
				test_mtk_reply_avail_ops_valid);

	g_test_add_data_func("/testmtkreply/sim: "
				"valid ENTER_SIM_PIN Test 1",
				&mtk_reply_enter_sim_pin_valid_1,
				test_mtk_reply_enter_sim_pin_valid);

#endif	/* LITTLE_ENDIAN */

	return g_test_run();
}
