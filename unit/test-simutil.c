/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

static void test_buffer(const unsigned char *buf, size_t size)
{
	struct ber_tlv_iter iter;
	struct ber_tlv_iter cont;

	ber_tlv_iter_init(&iter, buf, size);

	g_assert(ber_tlv_iter_next(&iter) == TRUE);
	g_assert(ber_tlv_iter_get_short_tag(&iter) == 0xAB);

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

static void test_ber_tlv_iter(void)
{
	test_buffer(valid_mms_params, sizeof(valid_mms_params));
}

static void test_ber_tlv_builder_mms(void)
{
	struct ber_tlv_iter top_iter, nested_iter;
	struct ber_tlv_builder top_builder, nested_builder;
	unsigned char buf[512], *pdu;
	unsigned int pdulen;

	ber_tlv_iter_init(&top_iter, valid_mms_params,
				sizeof(valid_mms_params));
	g_assert(ber_tlv_builder_init(&top_builder, buf, sizeof(buf)));

	/* Copy the structure */
	while (ber_tlv_iter_next(&top_iter) == TRUE) {
		g_assert(ber_tlv_builder_next(&top_builder,
					ber_tlv_iter_get_class(&top_iter),
					ber_tlv_iter_get_encoding(&top_iter),
					ber_tlv_iter_get_tag(&top_iter)));

		ber_tlv_iter_recurse(&top_iter, &nested_iter);
		g_assert(ber_tlv_builder_recurse(&top_builder,
							&nested_builder));

		while (ber_tlv_iter_next(&nested_iter) == TRUE) {
			g_assert(ber_tlv_builder_next(&nested_builder,
					ber_tlv_iter_get_class(&nested_iter),
					ber_tlv_iter_get_encoding(&nested_iter),
					ber_tlv_iter_get_tag(&nested_iter)));

			g_assert(ber_tlv_builder_set_length(&nested_builder,
					ber_tlv_iter_get_length(&nested_iter)));
			memcpy(ber_tlv_builder_get_data(&nested_builder),
					ber_tlv_iter_get_data(&nested_iter),
					ber_tlv_iter_get_length(&nested_iter));
		}

		ber_tlv_builder_optimize(&nested_builder, NULL, NULL);
	}

	ber_tlv_builder_optimize(&top_builder, &pdu, &pdulen);

	test_buffer(pdu, pdulen);
}

static void test_ber_tlv_builder_efpnn(void)
{
	struct sim_eons *eons_info;
	unsigned char efpnn0[64], efpnn1[64];
	struct ber_tlv_builder builder;

	g_assert(ber_tlv_builder_init(&builder, efpnn0, sizeof(efpnn0)));
	g_assert(ber_tlv_builder_next(&builder,
					BER_TLV_DATA_TYPE_APPLICATION,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x03));
	g_assert(ber_tlv_builder_set_length(&builder, 10));
	ber_tlv_builder_get_data(&builder)[0] = 0x00;
	ber_tlv_builder_get_data(&builder)[1] = 0x54;
	ber_tlv_builder_get_data(&builder)[2] = 0x75;
	ber_tlv_builder_get_data(&builder)[3] = 0x78;
	ber_tlv_builder_get_data(&builder)[4] = 0x20;
	ber_tlv_builder_get_data(&builder)[5] = 0x43;
	ber_tlv_builder_get_data(&builder)[6] = 0x6f;
	ber_tlv_builder_get_data(&builder)[7] = 0x6d;
	ber_tlv_builder_get_data(&builder)[8] = 0x6d;
	ber_tlv_builder_get_data(&builder)[9] = 0xff;
	ber_tlv_builder_get_data(&builder)[10] = 0xff;
	ber_tlv_builder_optimize(&builder, NULL, NULL);

	g_assert(ber_tlv_builder_init(&builder, efpnn1, sizeof(efpnn1)));
	g_assert(ber_tlv_builder_next(&builder,
					BER_TLV_DATA_TYPE_APPLICATION,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x03));
	g_assert(ber_tlv_builder_set_length(&builder, 3));
	ber_tlv_builder_get_data(&builder)[0] = 0x00;
	ber_tlv_builder_get_data(&builder)[1] = 0x4c;
	ber_tlv_builder_get_data(&builder)[2] = 0x6f;
	ber_tlv_builder_get_data(&builder)[3] = 0x6e;
	ber_tlv_builder_get_data(&builder)[4] = 0x67;
	g_assert(ber_tlv_builder_next(&builder,
					BER_TLV_DATA_TYPE_APPLICATION,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x05));
	g_assert(ber_tlv_builder_set_length(&builder, 6));
	ber_tlv_builder_get_data(&builder)[0] = 0x00;
	ber_tlv_builder_get_data(&builder)[1] = 0x53;
	ber_tlv_builder_get_data(&builder)[2] = 0x68;
	ber_tlv_builder_get_data(&builder)[3] = 0x6f;
	ber_tlv_builder_get_data(&builder)[4] = 0x72;
	ber_tlv_builder_get_data(&builder)[5] = 0x74;
	ber_tlv_builder_optimize(&builder, NULL, NULL);

	eons_info = sim_eons_new(1);
	sim_eons_add_pnn_record(eons_info, 1, efpnn0, sizeof(efpnn0));
	g_assert(!sim_eons_pnn_is_empty(eons_info));
	sim_eons_free(eons_info);

	eons_info = sim_eons_new(1);
	sim_eons_add_pnn_record(eons_info, 1, efpnn1, sizeof(efpnn1));
	g_assert(!sim_eons_pnn_is_empty(eons_info));
	sim_eons_free(eons_info);
}

static void test_ber_tlv_builder_3g_status(void)
{
	unsigned char buf[512];
	struct ber_tlv_builder top_builder, nested_builder;
	unsigned char *response;
	unsigned int len;
	int flen, rlen, str;
	unsigned char access[3];
	unsigned short efid;

	/* Build a binary EF status response */
	g_assert(ber_tlv_builder_init(&top_builder, buf, sizeof(buf)));

	g_assert(ber_tlv_builder_next(&top_builder,
					BER_TLV_DATA_TYPE_APPLICATION,
					BER_TLV_DATA_ENCODING_TYPE_CONSTRUCTED,
					0x02));
	g_assert(ber_tlv_builder_recurse(&top_builder, &nested_builder));

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x02));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 2));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x41;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x21;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x03));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 2));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x2f;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x05;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x0a));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 1));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x05;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x0b));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 3));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x2f;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x06;
	ber_tlv_builder_get_data(&nested_builder)[2] = 0x0f;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x00));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 2));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x00;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x0a;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x08));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 1));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x28;

	ber_tlv_builder_optimize(&nested_builder, NULL, NULL);
	ber_tlv_builder_optimize(&top_builder, &response, &len);

	sim_parse_3g_get_response(response, len, &flen, &rlen, &str,
					access, &efid);

	g_assert(flen == 10);
	g_assert(rlen == 0);
	g_assert(str == 0);
	g_assert(access[0] == 0x01);
	g_assert(access[1] == 0xff);
	g_assert(access[2] == 0x44);
	g_assert(efid == 0x2F05);

	/* Build a record-based EF status response */
	g_assert(ber_tlv_builder_init(&top_builder, buf, sizeof(buf)));

	g_assert(ber_tlv_builder_next(&top_builder,
					BER_TLV_DATA_TYPE_APPLICATION,
					BER_TLV_DATA_ENCODING_TYPE_CONSTRUCTED,
					0x02));
	g_assert(ber_tlv_builder_recurse(&top_builder, &nested_builder));

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x02));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 5));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x42;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x21;
	ber_tlv_builder_get_data(&nested_builder)[2] = 0x00;
	ber_tlv_builder_get_data(&nested_builder)[3] = 0x20;
	ber_tlv_builder_get_data(&nested_builder)[4] = 0x04;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x03));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 2));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x6f;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x40;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x0a));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 1));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x05;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x0b));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 3));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x2f;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x06;
	ber_tlv_builder_get_data(&nested_builder)[2] = 0x07;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x00));
	g_assert(ber_tlv_builder_set_length(&nested_builder, 2));
	ber_tlv_builder_get_data(&nested_builder)[0] = 0x00;
	ber_tlv_builder_get_data(&nested_builder)[1] = 0x80;

	g_assert(ber_tlv_builder_next(&nested_builder,
					BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC,
					BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE,
					0x08));

	ber_tlv_builder_optimize(&nested_builder, NULL, NULL);
	ber_tlv_builder_optimize(&top_builder, &response, &len);

	sim_parse_3g_get_response(response, len, &flen, &rlen, &str,
					access, &efid);

	g_assert(flen == 0x80);
	g_assert(rlen == 0x20);
	g_assert(str == 1);
	g_assert(access[0] == 0x11);
	g_assert(access[1] == 0xff);
	g_assert(access[2] == 0x44);
	g_assert(efid == 0x6F40);
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

static void test_eons(void)
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
	g_assert(op_info == NULL);
	op_info = sim_eons_lookup(eons_info, "246", "81");
	g_assert(op_info);

	g_assert(!strcmp(op_info->longname, "Tux Comm"));
	g_assert(!op_info->shortname);
	g_assert(!op_info->info);

	sim_eons_free(eons_info);
}

static void test_ef_db(void)
{
	struct sim_ef_info *info;

	info = sim_ef_db_lookup(0x6FAD);
	g_assert(info);

	info = sim_ef_db_lookup(0x6FB1);
	g_assert(info == NULL);

	info = sim_ef_db_lookup(0x2F05);
	g_assert(info);
}

static const char *binary_ef = "62178202412183022F058A01058B032F060F8002000A"
				"880128";
static const char *record_ef = "62198205422100200483026F408A01058B036F0607"
				"800200808800";

static void test_3g_status_data(void)
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

static char *at_cuad_response = "611B4F10A0000000871002FFFFFFFF8905080000"
	"FFFFFFFFFFFFFFFFFFFFFFFFFF611F4F0CA000000063504B43532D"
	"313550094D49445066696C657351043F007F80";

static void test_application_entry_decode(void)
{
	unsigned char *ef_dir;
	long len;
	GSList *entries;
	struct sim_app_record *app[2];

	ef_dir = decode_hex(at_cuad_response, -1, &len, 0);
	entries = sim_parse_app_template_entries(ef_dir, len);

	g_assert(g_slist_length(entries) == 2);

	app[0] = entries->next->data;
	app[1] = entries->data;

	g_assert(app[0]->aid_len == 0x10);
	g_assert(!memcmp(app[0]->aid, &ef_dir[4], 0x10));
	g_assert(app[0]->label == NULL);

	g_assert(app[1]->aid_len == 0x0c);
	g_assert(!memcmp(app[1]->aid, &ef_dir[37], 0x0c));
	g_assert(app[1]->label != NULL);
	g_assert(!strcmp(app[1]->label, "MIDPfiles"));

	g_free(ef_dir);
}

static void test_get_3g_path(void)
{
	unsigned char path[6];
	unsigned int len;
	unsigned char path1[] = { 0x3F, 0x00, 0x7F, 0xFF };

	len = sim_ef_db_get_path_3g(SIM_EFPNN_FILEID, path);
	g_assert(len == 4);
	g_assert(!memcmp(path, path1, len));
}

static void test_get_2g_path(void)
{
	unsigned char path[6];
	unsigned int len;
	unsigned char path1[] = { 0x3F, 0x00, 0x7F, 0x20 };

	len = sim_ef_db_get_path_2g(SIM_EFPNN_FILEID, path);
	g_assert(len == 4);
	g_assert(!memcmp(path, path1, len));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testsimutil/ber tlv iter", test_ber_tlv_iter);
	g_test_add_func("/testsimutil/ber tlv encode MMS",
			test_ber_tlv_builder_mms);
	g_test_add_func("/testsimutil/ber tlv encode EFpnn",
			test_ber_tlv_builder_efpnn);
	g_test_add_func("/testsimutil/ber tlv encode 3G Status response",
			test_ber_tlv_builder_3g_status);
	g_test_add_func("/testsimutil/EONS Handling", test_eons);
	g_test_add_func("/testsimutil/Elementary File DB", test_ef_db);
	g_test_add_func("/testsimutil/3G Status response", test_3g_status_data);
	g_test_add_func("/testsimutil/Application entries decoding",
			test_application_entry_decode);
	g_test_add_func("/testsimutil/3G path", test_get_3g_path);
	g_test_add_func("/testsimutil/2G path", test_get_2g_path);

	return g_test_run();
}
