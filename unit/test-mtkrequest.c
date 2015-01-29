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

#include "drivers/rilmodem/vendor.h"
#include "drivers/mtkmodem/mtkrequest.h"

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

/* MTK specific sim_read_info tests */

static const guchar req_mtk_sim_read_info_parcel_valid_1[] = {
	0xc0, 0x00, 0x00, 0x00, 0xb7, 0x6f, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x37, 0x00, 0x46, 0x00, 0x46, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char sim_mtk_read_info_path_valid_1[] = {0x7F, 0xFF};

static const struct req_sim_read_info req_mtk_sim_read_info_valid_1 = {
	.app_type = RIL_APPTYPE_UNKNOWN,
	.aid_str = "",
	.fileid = 0x6FB7,
	.path = sim_mtk_read_info_path_valid_1,
	.path_len = sizeof(sim_mtk_read_info_path_valid_1),
};

static const struct request_test_data mtk_sim_read_info_valid_test_1 = {
	.request = &req_mtk_sim_read_info_valid_1,
	.parcel_data = (guchar *) &req_mtk_sim_read_info_parcel_valid_1,
	.parcel_size = sizeof(req_mtk_sim_read_info_parcel_valid_1),
};

/* MTK specific sim_read_binary tests */

static const guchar req_mtk_sim_read_binary_parcel_valid_1[] = {
	0xb0, 0x00, 0x00, 0x00, 0xe2, 0x2f, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char sim_mtk_read_binary_path_valid_1[] = {0x3F, 0x00};

static const struct req_sim_read_binary req_mtk_sim_read_binary_valid_1 = {
	.app_type = RIL_APPTYPE_UNKNOWN,
	.aid_str = "",
	.fileid = 0x2FE2,
	.path = sim_mtk_read_binary_path_valid_1,
	.path_len = sizeof(sim_mtk_read_binary_path_valid_1),
	.start = 0,
	.length = 0x0A,
};

static const struct request_test_data mtk_sim_read_binary_valid_test_1 = {
	.request = &req_mtk_sim_read_binary_valid_1,
	.parcel_data = (guchar *) &req_mtk_sim_read_binary_parcel_valid_1,
	.parcel_size = sizeof(req_mtk_sim_read_binary_parcel_valid_1),
};

/* MTK specific sim_read_record tests */

static const guchar mtk_req_sim_read_record_parcel_valid_1[] = {
	0xb2, 0x00, 0x00, 0x00, 0xe2, 0x2f, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char mtk_sim_read_record_path_valid_1[] = {0x3F, 0x00};

static const struct req_sim_read_record mtk_req_sim_read_record_valid_1 = {
	.app_type = RIL_APPTYPE_UNKNOWN,
	.aid_str = "",
	.fileid = 0x2FE2,
	.path = mtk_sim_read_record_path_valid_1,
	.path_len = sizeof(mtk_sim_read_record_path_valid_1),
	.record = 5,
	.length = 0x0A,
};

static const struct request_test_data mtk_sim_read_record_valid_test_1 = {
	.request = &mtk_req_sim_read_record_valid_1,
	.parcel_data = (guchar *) &mtk_req_sim_read_record_parcel_valid_1,
	.parcel_size = sizeof(mtk_req_sim_read_record_parcel_valid_1),
};

/* MTK: dual_sim_mode_switch tests */

const int dual_sim_mode_1 = MTK_SWITCH_MODE_SIM_1_ACTIVE;

static const guchar req_dual_sim_mode_switch_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_data dual_sim_mode_switch_valid_test_1 = {
	.request = &dual_sim_mode_1,
	.parcel_data = req_dual_sim_mode_switch_parcel_valid_1,
	.parcel_size = sizeof(req_dual_sim_mode_switch_parcel_valid_1),
};

/* MTK: set_gprs_connect_type tests */

const int set_gprs_connect_always = MTK_GPRS_CONNECT_ALWAYS;

static const guchar req_set_gprs_connect_type_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_data set_gprs_connect_type_valid_test_1 = {
	.request = &set_gprs_connect_always,
	.parcel_data = req_set_gprs_connect_type_parcel_valid_1,
	.parcel_size = sizeof(req_set_gprs_connect_type_parcel_valid_1),
};

/* MTK: set_gprs_transfer_type tests */

const int set_gprs_transfer_call_prefer = MTK_GPRS_TRANSFER_CALL_PREFER;

static const guchar req_set_gprs_transfer_type_parcel_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct request_test_data set_gprs_transfer_type_valid_test_1 = {
	.request = &set_gprs_transfer_call_prefer,
	.parcel_data = req_set_gprs_transfer_type_parcel_valid_1,
	.parcel_size = sizeof(req_set_gprs_transfer_type_parcel_valid_1),
};

/* MTK: set_call_indication tests */

struct request_test_set_call_indication_data {
	int mode;
	int call_id;
	int seq_number;
	const unsigned char *parcel_data;
	size_t parcel_size;
};

static const guchar req_set_call_indication_parcel_valid_1[] = {
	0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00
};

static const struct request_test_set_call_indication_data
			set_call_indication_valid_test_1 = {
	.mode = MTK_CALL_INDIC_MODE_AVAIL,
	.call_id = 1,
	.seq_number = 1,
	.parcel_data = req_set_call_indication_parcel_valid_1,
	.parcel_size = sizeof(req_set_call_indication_parcel_valid_1),
};

/* MTK: set_fd_mode tests */

struct request_test_set_fd_mode_data {
	int mode;
	int param1;
	int param2;
	const unsigned char *parcel_data;
	size_t parcel_size;
};

static const guchar req_set_fd_mode_parcel_valid_1[] = {
	0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const struct request_test_set_fd_mode_data set_fd_mode_valid_test_1 = {
	.mode = MTK_FD_MODE_SCREEN_STATUS,
	.param1 = MTK_FD_PAR1_SCREEN_OFF,
	.param2 = -1,
	.parcel_data = req_set_fd_mode_parcel_valid_1,
	.parcel_size = sizeof(req_set_fd_mode_parcel_valid_1),
};

static const guchar req_set_fd_mode_parcel_valid_2[] = {
	0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static const struct request_test_set_fd_mode_data set_fd_mode_valid_test_2 = {
	.mode = MTK_FD_MODE_SCREEN_STATUS,
	.param1 = MTK_FD_PAR1_SCREEN_ON,
	.param2 = -1,
	.parcel_data = req_set_fd_mode_parcel_valid_2,
	.parcel_size = sizeof(req_set_fd_mode_parcel_valid_2),
};

/* MTK: set_3g_capability tests */

static const guchar req_set_3g_capability_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static const struct request_test_data set_3g_capability_valid_test_1 = {
	.request = NULL,
	.parcel_data = req_set_3g_capability_valid_1,
	.parcel_size = sizeof(req_set_3g_capability_valid_1),
};

/* MTK: store_modem_type tests */

static const guchar req_store_modem_type_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
};

static int store_modem_type_md_type_1 = MTK_MD_TYPE_LWG;

static const struct request_test_data store_modem_type_valid_test_1 = {
	.request = &store_modem_type_md_type_1,
	.parcel_data = req_store_modem_type_valid_1,
	.parcel_size = sizeof(req_store_modem_type_valid_1),
};

/* MTK: set_trm tests */

static const guchar req_set_trm_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00,
};

static int set_trm_argument_1 = 0x0B;

static const struct request_test_data set_trm_valid_test_1 = {
	.request = &set_trm_argument_1,
	.parcel_data = req_set_trm_valid_1,
	.parcel_size = sizeof(req_set_trm_valid_1),
};

/* MTK: resume_registration tests */

static const guchar req_resume_registration_valid_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static int resume_registration_argument_1 = 1;

static const struct request_test_data resume_registration_valid_test_1 = {
	.request = &resume_registration_argument_1,
	.parcel_data = req_resume_registration_valid_1,
	.parcel_size = sizeof(req_resume_registration_valid_1),
};

/* Test functions */

static void test_mtk_req_sim_read_info_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sim_read_info *req = test_data->request;
	gboolean result;
	struct parcel rilp;
	GRil *gril = g_ril_new(NULL, OFONO_RIL_VENDOR_MTK);

	result = g_ril_request_sim_read_info(gril, req, &rilp);

	g_assert(result == TRUE);
	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);

	g_ril_unref(gril);
}

static void test_mtk_req_sim_read_binary_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sim_read_binary *req = test_data->request;
	struct parcel rilp;
	gboolean result;
	GRil *gril = g_ril_new(NULL, OFONO_RIL_VENDOR_MTK);

	g_assert(gril != NULL);

	result = g_ril_request_sim_read_binary(gril, req, &rilp);

	g_assert(result == TRUE);
	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);

	g_ril_unref(gril);
}

static void test_mtk_req_sim_read_record_valid(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const struct req_sim_read_record *req = test_data->request;
	struct parcel rilp;
	gboolean result;
	GRil *gril = g_ril_new(NULL, OFONO_RIL_VENDOR_MTK);

	g_assert(gril != NULL);

	result = g_ril_request_sim_read_record(gril, req, &rilp);

	g_assert(result == TRUE);
	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);

	g_ril_unref(gril);
}

static void test_request_dual_sim_mode_switch(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	int mode = *(int *) test_data->request;
	struct parcel rilp;

	g_mtk_request_dual_sim_mode_switch(NULL, mode, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_set_gprs_connect_type(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	int connect_type = *(int *) test_data->request;
	struct parcel rilp;

	g_mtk_request_set_gprs_connect_type(NULL, connect_type, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_set_gprs_transfer_type(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	int transfer_type = *(int *) test_data->request;
	struct parcel rilp;

	g_mtk_request_set_gprs_transfer_type(NULL, transfer_type, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_set_call_indication(gconstpointer data)
{
	const struct request_test_set_call_indication_data *test_data = data;
	struct parcel rilp;

	g_mtk_request_set_call_indication(NULL, test_data->mode,
						test_data->call_id,
						test_data->seq_number, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_set_fd_mode(gconstpointer data)
{
	const struct request_test_set_fd_mode_data *test_data = data;
	struct parcel rilp;

	g_mtk_request_set_fd_mode(NULL, test_data->mode, test_data->param1,
					test_data->param2, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_set_3g_capability(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	struct parcel rilp;

	g_mtk_request_set_3g_capability(NULL, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_store_modem_type(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const int *md_type = test_data->request;
	struct parcel rilp;

	g_mtk_request_store_modem_type(NULL, *md_type, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_set_trm(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const int *trm = test_data->request;
	struct parcel rilp;

	g_mtk_request_set_trm(NULL, *trm, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
}

static void test_request_resume_registration(gconstpointer data)
{
	const struct request_test_data *test_data = data;
	const int *resume = test_data->request;
	struct parcel rilp;

	g_mtk_request_resume_registration(NULL, *resume, &rilp);

	g_assert(test_data->parcel_size == rilp.size);
	g_assert(!memcmp(rilp.data, test_data->parcel_data, rilp.size));

	parcel_free(&rilp);
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

	g_test_add_data_func("/testmtkrequest/mtk: "
				"valid DUAL_SIM_MODE_SWITCH Test 1",
				&dual_sim_mode_switch_valid_test_1,
				test_request_dual_sim_mode_switch);

	g_test_add_data_func("/testmtkrequest/gprs: "
				"valid SET_GPRS_CONNECT_TYPE Test 1",
				&set_gprs_connect_type_valid_test_1,
				test_request_set_gprs_connect_type);

	g_test_add_data_func("/testmtkrequest/gprs: "
				"valid SET_GPRS_TRANSFER_TYPE Test 1",
				&set_gprs_transfer_type_valid_test_1,
				test_request_set_gprs_transfer_type);

	g_test_add_data_func("/testmtkrequest/voicecall: "
				"valid SET_CALL_INDICATION Test 1",
				&set_call_indication_valid_test_1,
				test_request_set_call_indication);

	g_test_add_data_func("/testmtkrequest/sim: "
				"valid SIM_READ_INFO Test 1",
				&mtk_sim_read_info_valid_test_1,
				test_mtk_req_sim_read_info_valid);

	g_test_add_data_func("/testmtkrequest/sim: "
				"valid SIM_READ_BINARY Test 1",
				&mtk_sim_read_binary_valid_test_1,
				test_mtk_req_sim_read_binary_valid);

	g_test_add_data_func("/testmtkrequest/sim: "
				"valid SIM_READ_RECORD Test 1",
				&mtk_sim_read_record_valid_test_1,
				test_mtk_req_sim_read_record_valid);

	g_test_add_data_func("/testmtkrequest/radio-settings: "
				"valid SET_FD_MODE Test 1",
				&set_fd_mode_valid_test_1,
				test_request_set_fd_mode);

	g_test_add_data_func("/testmtkrequest/radio-settings: "
				"valid SET_FD_MODE Test 2",
				&set_fd_mode_valid_test_2,
				test_request_set_fd_mode);

	g_test_add_data_func("/testmtkrequest/mtk-settings: "
				"valid SET_3G_CAPABILITY Test 1",
				&set_3g_capability_valid_test_1,
				test_request_set_3g_capability);

	g_test_add_data_func("/testmtkrequest/modem-fw: "
				"valid STORE_MODEM_TYPE Test 1",
				&store_modem_type_valid_test_1,
				test_request_store_modem_type);

	g_test_add_data_func("/testmtkrequest/modem-fw: "
				"valid SET_TRM Test 1",
				&set_trm_valid_test_1,
				test_request_set_trm);

	g_test_add_data_func("/testmtkrequest/network: "
				"valid RESUME_REGISTRATION Test 1",
				&resume_registration_valid_test_1,
				test_request_resume_registration);

#endif	/* LITTLE_ENDIAN */

	return g_test_run();
}
