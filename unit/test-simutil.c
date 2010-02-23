/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#include <ofono/types.h>

#include "simutil.h"
#include "util.h"

/* Taken from 51.011 Appendix K.2 */
const unsigned char valid_mms_params[] = {
	0xAB, 0x81, 0x88, 0x80, 0x01, 0x01, 0x81, 0x17, 0x68, 0x74, 0x74, 0x70,
	0x3A, 0x2F, 0x2F, 0x6D, 0x6D, 0x73, 0x2D, 0x6F, 0x70, 0x65, 0x72, 0x61,
	0x74, 0x6F, 0x72, 0x2E, 0x63, 0x6F, 0x6D, 0x82, 0x32, 0x10, 0xAA, 0x08,
	0x2B, 0x34, 0x39, 0x35, 0x33, 0x34, 0x31, 0x39, 0x30, 0x36, 0x00, 0x09,
	0x87, 0x25, 0xC5, 0x0A, 0x90, 0x0C, 0x9A, 0x0D, 0x64, 0x75, 0x6D, 0x6D,
	0x79, 0x5F, 0x6E, 0x61, 0x6D, 0x65, 0x00, 0x0E, 0x64, 0x75, 0x6D, 0x6D,
	0x79, 0x5F, 0x70, 0x61, 0x73, 0x73, 0x77, 0x6F, 0x72, 0x64, 0x00, 0x83,
	0x36, 0x20, 0x31, 0x37, 0x30, 0x2E, 0x31, 0x38, 0x37, 0x2E, 0x35, 0x31,
	0x2E, 0x33, 0x00, 0x21, 0x85, 0x23, 0x39, 0x32, 0x30, 0x33, 0x00, 0x24,
	0xCB, 0x19, 0x9C, 0x1A, 0x64, 0x75, 0x6D, 0x6D, 0x79, 0x5F, 0x6E, 0x61,
	0x6D, 0x65, 0x00, 0x1B, 0x64, 0x75, 0x6D, 0x6D, 0x79, 0x5F, 0x70, 0x61,
	0x73, 0x73, 0x77, 0x6F, 0x72, 0x64, 0x00 };

static void test_ber_tlv_iter()
{
	struct ber_tlv_iter iter;
	struct ber_tlv_iter cont;

	ber_tlv_iter_init(&iter, valid_mms_params, sizeof(valid_mms_params));

	g_assert(ber_tlv_iter_next(&iter) == TRUE);
	g_assert(ber_tlv_iter_get_short_tag(&iter) == 0xAB);
	g_assert(ber_tlv_iter_get_length(&iter) == 136);

	ber_tlv_iter_recurse(&iter, &cont);

	g_assert(ber_tlv_iter_next(&cont) == TRUE);
	g_assert(ber_tlv_iter_get_short_tag(&cont) == 0x80);
	g_assert(ber_tlv_iter_get_length(&cont) == 1);

	g_assert(ber_tlv_iter_next(&cont) == TRUE);
	g_assert(ber_tlv_iter_get_short_tag(&cont) == 0x81);
	g_assert(ber_tlv_iter_get_length(&cont) == 23);

	g_assert(ber_tlv_iter_next(&cont) == TRUE);
	g_assert(ber_tlv_iter_get_short_tag(&cont) == 0x82);
	g_assert(ber_tlv_iter_get_length(&cont) == 50);

	g_assert(ber_tlv_iter_next(&cont) == TRUE);
	g_assert(ber_tlv_iter_get_short_tag(&cont) == 0x83);
	g_assert(ber_tlv_iter_get_length(&cont) == 54);

	g_assert(ber_tlv_iter_next(&cont) == FALSE);
	g_assert(ber_tlv_iter_next(&iter) == FALSE);
}

const unsigned char valid_efopl[] = {
	0x42, 0xf6, 0x1d, 0x00, 0x00, 0xff, 0xfe, 0x01,
};

const unsigned char valid_efpnn[][28] = {
	{ 0x43, 0x0a, 0x00, 0x54, 0x75, 0x78, 0x20, 0x43, 0x6f, 0x6d,
	  0x6d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, },
	{ 0x43, 0x05, 0x00, 0x4C, 0x6F, 0x6E, 0x67, 0x45, 0x06, 0x00,
	  0x53, 0x68, 0x6F, 0x72, 0x74, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, }
};

static void test_eons()
{
	const struct sim_eons_operator_info *op_info;
	struct sim_eons *eons_info;

	eons_info = sim_eons_new(2);

	g_assert(sim_eons_pnn_is_empty(eons_info));

	sim_eons_add_pnn_record(eons_info, 1,
			valid_efpnn[0], sizeof(valid_efpnn[0]));
	g_assert(!sim_eons_pnn_is_empty(eons_info));

	sim_eons_add_pnn_record(eons_info, 2,
			valid_efpnn[1], sizeof(valid_efpnn[1]));
	g_assert(!sim_eons_pnn_is_empty(eons_info));

	sim_eons_add_opl_record(eons_info, valid_efopl, sizeof(valid_efopl));
	sim_eons_optimize(eons_info);

	op_info = sim_eons_lookup(eons_info, "246", "82");
	g_assert(!op_info);
	op_info = sim_eons_lookup(eons_info, "246", "81");
	g_assert(op_info);

	g_assert(!strcmp(op_info->longname, "Tux Comm"));
	g_assert(!op_info->shortname);
	g_assert(!op_info->info);

	sim_eons_free(eons_info);
}

static void test_ef_db()
{
	struct sim_ef_info *info;

	info = sim_ef_db_lookup(0x6FAD);
	g_assert(info);

	info = sim_ef_db_lookup(0x6FB1);
	g_assert(!info);

	info = sim_ef_db_lookup(0x2F05);
	g_assert(info);

	info = sim_ef_db_lookup(0x6FE3);
	g_assert(info);
}

static const char *binary_ef = "62178202412183022F058A01058B032F060F8002000A"
				"880128";
static const char *record_ef = "62198205422100200483026F408A01058B036F0607"
				"800200808800";

static void test_3g_status_data()
{
	unsigned char *response;
	long len;
	int flen, rlen, str;
	unsigned char access[3];
	unsigned short efid;

	response = decode_hex(binary_ef, -1, &len, 0);

	sim_parse_3g_get_response(response, len, &flen, &rlen, &str,
					access, &efid);

	g_assert(flen == 10);
	g_assert(rlen == 0);
	g_assert(str == 0);
	g_assert(access[0] == 0x01);
	g_assert(access[1] == 0xff);
	g_assert(access[2] == 0x44);
	g_assert(efid == 0x2F05);

	g_free(response);

	response = decode_hex(record_ef, -1, &len, 0);

	sim_parse_3g_get_response(response, len, &flen, &rlen, &str,
					access, &efid);

	g_assert(flen == 0x80);
	g_assert(rlen == 0x20);
	g_assert(str == 1);
	g_assert(access[0] == 0x11);
	g_assert(access[1] == 0xff);
	g_assert(access[2] == 0x44);
	g_assert(efid == 0x6F40);

	g_free(response);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testsimutil/ber tlv iter", test_ber_tlv_iter);
	g_test_add_func("/testsimutil/EONS Handling", test_eons);
	g_test_add_func("/testsimutil/Elementary File DB", test_ef_db);
	g_test_add_func("/testsimutil/3G Status response", test_3g_status_data);

	return g_test_run();
}
