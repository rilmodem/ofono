/*
 * oFono - GSM Telephony Stack for Linux
 *
 * Copyright (C) 2008-2010 Intel Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "util.h"
#include "smsutil.h"

static const char *simple_deliver = "07911326040000F0"
		"040B911346610089F60000208062917314480CC8F71D14969741F977FD07";
static const char *alnum_sender = "0791447758100650"
		"040DD0F334FC1CA6970100008080312170224008D4F29CDE0EA7D9";
static const char *simple_submit = "0011000B916407281553F80000AA"
		"0AE8329BFD4697D9EC37";

static void print_scts(struct sms_scts *scts, const char *prefix)
{
	time_t ts;
	struct tm remote;
	char buf[128];

	g_print("%s: (YY-MM-DD) %02d-%02d-%02d\n", prefix,
		(int)scts->year, (int)scts->month, (int)scts->day);

	g_print("%s: (HH-MM-SS) %02d:%02d:%02d\n", prefix,
		(int)scts->hour, (int)scts->minute, (int)scts->second);

	g_print("%s: Timezone %d hours %d minutes\n", prefix,
		(int)scts->timezone / 4,
		(int)((abs(scts->timezone) % 4) * 15));

	ts = sms_scts_to_time(scts, &remote);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime(&ts));
	buf[127] = '\0';

	g_print("local time: %s\n", buf);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", &remote);
	buf[127] = '\0';

	g_print("remote time: %s\n", buf);
}

static void print_vpf(enum sms_validity_period_format vpf,
			struct sms_validity_period *vp)
{
	g_print("Validity Period Format: %d\n", (int)vpf);

	switch (vpf) {
	case SMS_VALIDITY_PERIOD_FORMAT_ABSENT:
		g_print("Validity-Period: Absent\n");
		break;
	case SMS_VALIDITY_PERIOD_FORMAT_RELATIVE:
		g_print("Validity-Period: %d\n",
			(int)vp->relative);
		break;
	case SMS_VALIDITY_PERIOD_FORMAT_ABSOLUTE:
		print_scts(&vp->absolute, "Validity-Period:");
		break;
	case SMS_VALIDITY_PERIOD_FORMAT_ENHANCED:
		g_print("Validity-Period: Enhanced");
		break;
	}
}

static void test_simple_deliver()
{
	struct sms sms;
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	int data_len;
	unsigned char *unpacked;
	char *utf8;

	decoded_pdu = decode_hex(simple_deliver, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(simple_deliver) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, FALSE, 30, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_DELIVER);

	if (g_test_verbose()) {
		g_print("SMSC Address number_type: %d, number_plan: %d, %s\n",
			(int)sms.sc_addr.number_type,
			(int)sms.sc_addr.numbering_plan, sms.sc_addr.address);

		g_print("SMS type: %d\n", (int)sms.type);

		g_print("Originator-Address: %d, %d, %s\n",
			(int)sms.deliver.oaddr.number_type,
			(int)sms.deliver.oaddr.numbering_plan,
			sms.deliver.oaddr.address);

		g_print("PID: %d\n", (int)sms.deliver.pid);
		g_print("DCS: %d\n", (int)sms.deliver.dcs);

		print_scts(&sms.deliver.scts, "Timezone");
	}

	g_assert(sms.sc_addr.number_type == SMS_NUMBER_TYPE_INTERNATIONAL);
	g_assert(sms.sc_addr.numbering_plan == SMS_NUMBERING_PLAN_ISDN);
	g_assert(strcmp(sms.sc_addr.address, "31624000000") == 0);

	g_assert(sms.deliver.oaddr.number_type ==
			SMS_NUMBER_TYPE_INTERNATIONAL);
	g_assert(sms.deliver.oaddr.numbering_plan ==
			SMS_NUMBERING_PLAN_ISDN);
	g_assert(strcmp(sms.deliver.oaddr.address, "31641600986") == 0);

	g_assert(sms.deliver.pid == 0);
	g_assert(sms.deliver.dcs == 0);

	g_assert(sms.deliver.scts.year == 2);
	g_assert(sms.deliver.scts.month == 8);
	g_assert(sms.deliver.scts.day == 26);
	g_assert(sms.deliver.scts.hour == 19);
	g_assert(sms.deliver.scts.minute == 37);
	g_assert(sms.deliver.scts.second == 41);
	g_assert(sms.deliver.scts.timezone == -4);

	g_assert(sms.deliver.udl == 12);

	data_len = sms_udl_in_bytes(sms.deliver.udl, sms.deliver.dcs);

	g_assert(data_len == 11);

	unpacked = unpack_7bit(sms.deliver.ud, data_len, 0, FALSE,
				sms.deliver.udl, NULL, 0xff);

	g_assert(unpacked);

	utf8 = convert_gsm_to_utf8(unpacked, -1, NULL, NULL, 0xff);

	g_free(unpacked);

	g_assert(utf8);

	if (g_test_verbose())
		g_print("Decoded user data is: %s\n", utf8);

	g_assert(strcmp(utf8, "How are you?") == 0);

	g_free(utf8);
}

static void test_alnum_sender()
{
	struct sms sms;
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	int data_len;
	unsigned char *unpacked;
	char *utf8;

	decoded_pdu = decode_hex(alnum_sender, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(alnum_sender) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, FALSE, 27, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_DELIVER);

	if (g_test_verbose()) {
		g_print("SMSC Address number_type: %d, number_plan: %d, %s\n",
			(int)sms.sc_addr.number_type,
			(int)sms.sc_addr.numbering_plan, sms.sc_addr.address);

		g_print("SMS type: %d\n", (int)sms.type);

		g_print("Originator-Address: %d, %d, %s\n",
			(int)sms.deliver.oaddr.number_type,
			(int)sms.deliver.oaddr.numbering_plan,
			sms.deliver.oaddr.address);

		g_print("PID: %d\n", (int)sms.deliver.pid);
		g_print("DCS: %d\n", (int)sms.deliver.dcs);

		print_scts(&sms.deliver.scts, "Timestamp");
	}

	g_assert(sms.sc_addr.number_type == SMS_NUMBER_TYPE_INTERNATIONAL);
	g_assert(sms.sc_addr.numbering_plan == SMS_NUMBERING_PLAN_ISDN);
	g_assert(strcmp(sms.sc_addr.address, "447785016005") == 0);

	g_assert(sms.deliver.oaddr.number_type ==
			SMS_NUMBER_TYPE_ALPHANUMERIC);
	g_assert(sms.deliver.oaddr.numbering_plan ==
			SMS_NUMBERING_PLAN_UNKNOWN);
	g_assert(strcmp(sms.deliver.oaddr.address, "sipgate") == 0);

	g_assert(sms.deliver.pid == 0);
	g_assert(sms.deliver.dcs == 0);

	g_assert(sms.deliver.scts.year == 8);
	g_assert(sms.deliver.scts.month == 8);
	g_assert(sms.deliver.scts.day == 13);
	g_assert(sms.deliver.scts.hour == 12);
	g_assert(sms.deliver.scts.minute == 07);
	g_assert(sms.deliver.scts.second == 22);
	g_assert(sms.deliver.scts.timezone == 4);

	g_assert(sms.deliver.udl == 8);

	data_len = sms_udl_in_bytes(sms.deliver.udl, sms.deliver.dcs);

	g_assert(data_len == 7);

	unpacked = unpack_7bit(sms.deliver.ud, data_len, 0, FALSE,
				sms.deliver.udl, NULL, 0xff);

	g_assert(unpacked);

	utf8 = convert_gsm_to_utf8(unpacked, -1, NULL, NULL, 0xff);

	g_free(unpacked);

	g_assert(utf8);

	if (g_test_verbose())
		g_print("Decoded user data is: %s\n", utf8);

	g_assert(strcmp(utf8, "Testmail") == 0);

	g_free(utf8);
}
static void test_deliver_encode()
{
	struct sms sms;
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	unsigned char pdu[176];
	int encoded_pdu_len;
	int encoded_tpdu_len;
	char *encoded_pdu;

	decoded_pdu = decode_hex(simple_deliver, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(simple_deliver) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, FALSE, 30, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_DELIVER);

	ret = sms_encode(&sms, &encoded_pdu_len, &encoded_tpdu_len, pdu);

	if (g_test_verbose()) {
		int i;

		for (i = 0; i < encoded_pdu_len; i++)
			g_print("%02X", pdu[i]);
		g_print("\n");
	}

	g_assert(ret);
	g_assert(encoded_tpdu_len == 30);
	g_assert(encoded_pdu_len == pdu_len);

	encoded_pdu = encode_hex(pdu, encoded_pdu_len, 0);

	g_assert(strcmp(simple_deliver, encoded_pdu) == 0);

	g_free(encoded_pdu);

	decoded_pdu = decode_hex(alnum_sender, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(alnum_sender) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, FALSE, 27, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_DELIVER);

	ret = sms_encode(&sms, &encoded_pdu_len, &encoded_tpdu_len, pdu);

	if (g_test_verbose()) {
		int i;

		for (i = 0; i < encoded_pdu_len; i++)
			g_print("%02X", pdu[i]);
		g_print("\n");
	}

	g_assert(ret);
	g_assert(encoded_tpdu_len == 27);
	g_assert(encoded_pdu_len == pdu_len);

	encoded_pdu = encode_hex(pdu, encoded_pdu_len, 0);

	g_assert(strcmp(alnum_sender, encoded_pdu) == 0);

	g_free(encoded_pdu);
}

static void test_simple_submit()
{
	struct sms sms;
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	int data_len;
	unsigned char *unpacked;
	char *utf8;

	decoded_pdu = decode_hex(simple_submit, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(simple_submit) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, TRUE, 23, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_SUBMIT);

	if (g_test_verbose()) {
		if (sms.sc_addr.address[0] == '\0')
			g_print("SMSC Address absent, default will be used\n");
		else
			g_print("SMSC Address number_type: %d,"
				" number_plan: %d, %s\n",
				(int)sms.sc_addr.number_type,
				(int)sms.sc_addr.numbering_plan,
				sms.sc_addr.address);

		g_print("SMS type: %d\n", (int)sms.type);

		g_print("Message Reference: %u\n", (int)sms.submit.mr);

		g_print("Destination-Address: %d, %d, %s\n",
			(int)sms.submit.daddr.number_type,
			(int)sms.submit.daddr.numbering_plan,
			sms.submit.daddr.address);

		g_print("PID: %d\n", (int)sms.submit.pid);
		g_print("DCS: %d\n", (int)sms.submit.dcs);

		print_vpf(sms.submit.vpf, &sms.submit.vp);
	}

	g_assert(strlen(sms.sc_addr.address) == 0);

	g_assert(sms.submit.mr == 0);

	g_assert(sms.submit.daddr.number_type ==
			SMS_NUMBER_TYPE_INTERNATIONAL);
	g_assert(sms.submit.daddr.numbering_plan ==
			SMS_NUMBERING_PLAN_ISDN);
	g_assert(strcmp(sms.submit.daddr.address, "46708251358") == 0);

	g_assert(sms.submit.pid == 0);
	g_assert(sms.submit.dcs == 0);

	g_assert(sms.submit.vpf == SMS_VALIDITY_PERIOD_FORMAT_RELATIVE);
	g_assert(sms.submit.vp.relative == 0xAA);

	g_assert(sms.submit.udl == 10);

	data_len = sms_udl_in_bytes(sms.submit.udl, sms.submit.dcs);

	g_assert(data_len == 9);

	unpacked = unpack_7bit(sms.submit.ud, data_len, 0, FALSE,
				sms.submit.udl, NULL, 0xff);

	g_assert(unpacked);

	utf8 = convert_gsm_to_utf8(unpacked, -1, NULL, NULL, 0xff);

	g_free(unpacked);

	g_assert(utf8);

	if (g_test_verbose())
		g_print("Decoded user data is: %s\n", utf8);

	g_assert(strcmp(utf8, "hellohello") == 0);

	g_free(utf8);
}

static void test_submit_encode()
{
	struct sms sms;
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	unsigned char pdu[176];
	int encoded_pdu_len;
	int encoded_tpdu_len;
	char *encoded_pdu;

	decoded_pdu = decode_hex(simple_submit, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(simple_submit) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, TRUE, 23, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_SUBMIT);

	ret = sms_encode(&sms, &encoded_pdu_len, &encoded_tpdu_len, pdu);

	if (g_test_verbose()) {
		int i;

		for (i = 0; i < encoded_pdu_len; i++)
			g_print("%02X", pdu[i]);
		g_print("\n");
	}

	g_assert(ret);
	g_assert(encoded_tpdu_len == 23);
	g_assert(encoded_pdu_len == pdu_len);

	encoded_pdu = encode_hex(pdu, encoded_pdu_len, 0);

	g_assert(strcmp(simple_submit, encoded_pdu) == 0);

	g_free(encoded_pdu);
}

static const char *header_test = "0041000B915121551532F40000631A0A031906200A03"
	"2104100A032705040A032E05080A043807002B8ACD29A85D9ECFC3E7F21C340EBB41E"
	"3B79B1E4EBB41697A989D1EB340E2379BCC02B1C3F27399059AB7C36C3628EC2683C6"
	"6FF65B5E2683E8653C1D";
static int header_test_len = 100;
static const char *header_test_expected = "EMS messages can contain italic, bold"
	", large, small and colored text";

static void test_udh_iter()
{
	struct sms sms;
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	int data_len;
	int udhl;
	struct sms_udh_iter iter;
	int max_chars;
	unsigned char *unpacked;
	char *utf8;

	decoded_pdu = decode_hex(header_test, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(header_test) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, TRUE,
				header_test_len, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_SUBMIT);

	if (g_test_verbose()) {
		if (sms.sc_addr.address[0] == '\0')
			g_print("SMSC Address absent, default will be used\n");
		else
			g_print("SMSC Address number_type: %d,"
				" number_plan: %d, %s\n",
				(int)sms.sc_addr.number_type,
				(int)sms.sc_addr.numbering_plan,
				sms.sc_addr.address);

		g_print("SMS type: %d\n", (int)sms.type);

		g_print("Message Reference: %u\n", (int)sms.submit.mr);

		g_print("Destination-Address: %d, %d, %s\n",
			(int)sms.submit.daddr.number_type,
			(int)sms.submit.daddr.numbering_plan,
			sms.submit.daddr.address);

		g_print("PID: %d\n", (int)sms.submit.pid);
		g_print("DCS: %d\n", (int)sms.submit.dcs);

		print_vpf(sms.submit.vpf, &sms.submit.vp);
	}

	udhl = sms.submit.ud[0];
	g_assert(sms.submit.udl == 99);
	g_assert(udhl == 26);

	ret = sms_udh_iter_init(&sms, &iter);

	g_assert(ret);

	g_assert(sms_udh_iter_get_ie_type(&iter) == SMS_IEI_TEXT_FORMAT);
	g_assert(sms_udh_iter_get_ie_length(&iter) == 3);
	g_assert(sms_udh_iter_has_next(&iter) == TRUE);
	g_assert(sms_udh_iter_next(&iter) == TRUE);

	g_assert(sms_udh_iter_get_ie_type(&iter) == SMS_IEI_TEXT_FORMAT);
	g_assert(sms_udh_iter_get_ie_length(&iter) == 3);
	g_assert(sms_udh_iter_has_next(&iter) == TRUE);
	g_assert(sms_udh_iter_next(&iter) == TRUE);

	g_assert(sms_udh_iter_get_ie_type(&iter) == SMS_IEI_TEXT_FORMAT);
	g_assert(sms_udh_iter_get_ie_length(&iter) == 3);
	g_assert(sms_udh_iter_has_next(&iter) == TRUE);
	g_assert(sms_udh_iter_next(&iter) == TRUE);

	g_assert(sms_udh_iter_get_ie_type(&iter) == SMS_IEI_TEXT_FORMAT);
	g_assert(sms_udh_iter_get_ie_length(&iter) == 3);
	g_assert(sms_udh_iter_has_next(&iter) == TRUE);
	g_assert(sms_udh_iter_next(&iter) == TRUE);

	g_assert(sms_udh_iter_get_ie_type(&iter) == SMS_IEI_TEXT_FORMAT);
	g_assert(sms_udh_iter_get_ie_length(&iter) == 4);
	g_assert(sms_udh_iter_has_next(&iter) == FALSE);
	g_assert(sms_udh_iter_next(&iter) == FALSE);
	g_assert(sms_udh_iter_get_ie_type(&iter) == SMS_IEI_INVALID);

	data_len = sms_udl_in_bytes(sms.submit.udl, sms.submit.dcs);

	g_assert(data_len == 87);

	max_chars = (data_len - (udhl + 1)) * 8 / 7;

	unpacked = unpack_7bit(sms.submit.ud + udhl + 1, data_len - (udhl + 1),
				udhl + 1, FALSE, max_chars, NULL, 0xff);

	g_assert(unpacked);

	utf8 = convert_gsm_to_utf8(unpacked, -1, NULL, NULL, 0xff);

	g_free(unpacked);

	g_assert(utf8);

	if (g_test_verbose())
		g_print("Decoded user data is: %s\n", utf8);

	g_assert(strcmp(utf8, header_test_expected) == 0);

	g_free(utf8);
}

static const char *assembly_pdu1 = "038121F340048155550119906041001222048C0500"
					"031E0301041804420430043A002C002004100"
					"43B0435043A04410430043D04340440002000"
					"200441043B044304480430043B00200437043"
					"000200434043204350440044C044E00200020"
					"04380020002004320441043500200431043E0"
					"43B044C044804350020043F04400435043804"
					"41043F043E043B043D044F043B0441044F002"
					"000200433043D0435";
static int assembly_pdu_len1 = 155;

static const char *assembly_pdu2 = "038121F340048155550119906041001222048C0500"
					"031E03020432043E043C002E000A041D04300"
					"43A043E043D04350446002C0020043D043500"
					"200432002004410438043B043004450020043"
					"40430043B043504350020044204350440043F"
					"04350442044C002C0020043E043D002004410"
					"44204400435043C043804420435043B044C04"
					"3D043E002004320431043504360430043B002"
					"004320020043A043E";
static int assembly_pdu_len2 = 155;

static const char *assembly_pdu3 = "038121F340048155550119906041001222044A0500"
					"031E0303043C043D043004420443002C00200"
					"43F043E043704300431044B0432000A043404"
					"3004360435002C002004470442043E0020002"
					"00431044B043B0020043D04300433002E";
static int assembly_pdu_len3 = 89;

static void test_assembly()
{
	unsigned char pdu[176];
	long pdu_len;
	struct sms sms;
	struct sms_assembly *assembly = sms_assembly_new(NULL);
	guint16 ref;
	guint8 max;
	guint8 seq;
	GSList *l;
	char *utf8;
	char *reencoded;

	decode_hex_own_buf(assembly_pdu1, -1, &pdu_len, 0, pdu);
	sms_decode(pdu, pdu_len, FALSE, assembly_pdu_len1, &sms);

	sms_extract_concatenation(&sms, &ref, &max, &seq);
	l = sms_assembly_add_fragment(assembly, &sms, time(NULL),
					&sms.deliver.oaddr, ref, max, seq);

	if (g_test_verbose()) {
		g_print("Ref: %u\n", ref);
		g_print("Max: %u\n", max);
		g_print("From: %s\n",
				sms_address_to_string(&sms.deliver.oaddr));
	}

	g_assert(g_slist_length(assembly->assembly_list) == 1);
	g_assert(l == NULL);

	sms_assembly_expire(assembly, time(NULL) + 40);

	g_assert(g_slist_length(assembly->assembly_list) == 0);

	sms_extract_concatenation(&sms, &ref, &max, &seq);
	l = sms_assembly_add_fragment(assembly, &sms, time(NULL),
					&sms.deliver.oaddr, ref, max, seq);
	g_assert(g_slist_length(assembly->assembly_list) == 1);
	g_assert(l == NULL);

	decode_hex_own_buf(assembly_pdu2, -1, &pdu_len, 0, pdu);
	sms_decode(pdu, pdu_len, FALSE, assembly_pdu_len2, &sms);

	sms_extract_concatenation(&sms, &ref, &max, &seq);
	l = sms_assembly_add_fragment(assembly, &sms, time(NULL),
					&sms.deliver.oaddr, ref, max, seq);
	g_assert(l == NULL);

	decode_hex_own_buf(assembly_pdu3, -1, &pdu_len, 0, pdu);
	sms_decode(pdu, pdu_len, FALSE, assembly_pdu_len3, &sms);

	sms_extract_concatenation(&sms, &ref, &max, &seq);
	l = sms_assembly_add_fragment(assembly, &sms, time(NULL),
					&sms.deliver.oaddr, ref, max, seq);

	g_assert(l != NULL);

	utf8 = sms_decode_text(l);

	g_slist_foreach(l, (GFunc)g_free, NULL);
	g_slist_free(l);

	sms_assembly_free(assembly);

	if (g_test_verbose())
		g_printf("Text:\n%s\n", utf8);

	l = sms_text_prepare(utf8, ref, TRUE, NULL);
	g_assert(l);
	g_assert(g_slist_length(l) == 3);

	reencoded = sms_decode_text(l);

	if (g_test_verbose())
		g_printf("ReEncoded:\n%s\n", reencoded);

	g_assert(strcmp(utf8, reencoded) == 0);

	g_free(utf8);
	g_free(reencoded);
}

static const char *test_no_fragmentation_7bit = "This is testing !";
static const char *expected_no_fragmentation_7bit = "079153485002020911000C915"
			"348870420140000A71154747A0E4ACF41F4F29C9E769F4121";
static const char *sc_addr = "+358405202090";
static const char *da_addr = "+358478400241";
static void test_prepare_7bit()
{
	GSList *r;
	struct sms *sms;
	gboolean ret;
	unsigned char pdu[176];
	int encoded_pdu_len;
	int encoded_tpdu_len;
	char *encoded_pdu;

	r = sms_text_prepare(test_no_fragmentation_7bit, 0, FALSE, NULL);

	g_assert(r != NULL);

	sms = r->data;

	sms->sc_addr.number_type = SMS_NUMBER_TYPE_INTERNATIONAL;
	sms->sc_addr.numbering_plan = SMS_NUMBERING_PLAN_ISDN;
	strcpy(sms->sc_addr.address, sc_addr+1);

	if (g_test_verbose())
		g_print("sc_addr: %s\n", sms_address_to_string(&sms->sc_addr));

	g_assert(!strcmp(sc_addr, sms_address_to_string(&sms->sc_addr)));

	sms->submit.daddr.number_type = SMS_NUMBER_TYPE_INTERNATIONAL;
	sms->submit.daddr.numbering_plan = SMS_NUMBERING_PLAN_ISDN;
	strcpy(sms->submit.daddr.address, da_addr+1);

	if (g_test_verbose())
		g_print("da_addr: %s\n",
			sms_address_to_string(&sms->submit.daddr));

	g_assert(!strcmp(da_addr,
				sms_address_to_string(&sms->submit.daddr)));

	ret = sms_encode(sms, &encoded_pdu_len, &encoded_tpdu_len, pdu);

	g_assert(ret);

	if (g_test_verbose()) {
		int i;

		for (i = 0; i < encoded_pdu_len; i++)
			g_print("%02X", pdu[i]);
		g_print("\n");
	}

	encoded_pdu = encode_hex(pdu, encoded_pdu_len, 0);

	g_assert(strcmp(expected_no_fragmentation_7bit, encoded_pdu) == 0);

	g_free(encoded_pdu);
	g_slist_foreach(r, (GFunc)g_free, NULL);
	g_slist_free(r);
}

struct sms_concat_data {
	const char *str;
	unsigned int segments;
};


static struct sms_concat_data shakespeare_test = {
	.str = "Shakespeare divided his time between London and Str"
	"atford during his career. In 1596, the year before he bought New Plac"
	"e as his family home in Stratford, Shakespeare was living in the pari"
	"sh of St. Helen's, Bishopsgate, north of the River Thames.",
	.segments = 2,
};

/* The string in this test should be padded at the end.  This confuses some
 * decoders which do not use udl properly
 */
static void test_prepare_concat(gconstpointer data)
{
	const struct sms_concat_data *test = data;
	GSList *r;
	GSList *l;
	char *decoded_str;
	GSList *pdus = NULL;
	unsigned char pdu[176];
	struct sms *sms;
	struct sms decoded;
	int pdu_len, tpdu_len;
	struct sms_assembly *assembly = sms_assembly_new(NULL);
	guint16 ref;
	guint8 max;
	guint8 seq;

	if (g_test_verbose())
		g_print("strlen: %zd\n", strlen(test->str));

	r = sms_text_prepare(test->str, 0, TRUE, NULL);

	g_assert(r);
	g_assert(g_slist_length(r) == test->segments);

	for (l = r; l; l = l->next) {
		char *strpdu;

		sms = l->data;

		sms_address_from_string(&sms->submit.daddr, "+15554449999");
		sms_encode(sms, &pdu_len, &tpdu_len, pdu);
		g_assert(pdu_len == (tpdu_len + 1));

		strpdu = encode_hex(pdu, pdu_len, 0);

		if (g_test_verbose())
			g_printf("PDU: %s, len: %d, tlen: %d\n",
				strpdu, pdu_len, tpdu_len);
		pdus = g_slist_append(pdus, strpdu);
	}

	g_slist_foreach(r, (GFunc)g_free, NULL);
	g_slist_free(r);

	for (l = pdus; l; l = l->next) {
		long len;
		gboolean ok;

		decode_hex_own_buf((char *)l->data, -1, &len, 0, pdu);

		if (g_test_verbose())
			g_print("PDU Len: %ld\n", len);

		ok = sms_decode(pdu, len, TRUE, len - 1, &decoded);
		g_assert(ok);

		if (g_test_verbose())
			g_print("Pdu udl: %d\n", (int)decoded.submit.udl);

		sms_extract_concatenation(&decoded, &ref, &max, &seq);
		r = sms_assembly_add_fragment(assembly, &decoded, time(NULL),
						&decoded.submit.daddr,
						ref, max, seq);
	}

	g_assert(r);

	decoded_str = sms_decode_text(r);

	if (g_test_verbose())
		g_printf("Decoded String: %s\n", decoded_str);

	g_assert(decoded_str);
	g_assert(strcmp(decoded_str, test->str) == 0);
	g_free(decoded_str);
	sms_assembly_free(assembly);
}

static void test_limit(gunichar uni, int target_size, gboolean use_16bit)
{
	char *utf8;
	char *decoded;
	GSList *l;
	unsigned int i;
	char utf8_char[6];
	unsigned int stride;

	stride = g_unichar_to_utf8(uni, utf8_char);

	utf8 = g_new0(char, (target_size + 2) * stride);

	for (i = 0; i < target_size * stride; i += stride)
		memcpy(utf8 + i, utf8_char, stride);

	utf8[i] = '\0';

	l = sms_text_prepare(utf8, 0, use_16bit, NULL);

	g_assert(l);
	g_assert(g_slist_length(l) == 255);

	decoded = sms_decode_text(l);
	g_assert(g_utf8_strlen(decoded, -1) == target_size);

	g_free(decoded);

	memcpy(utf8 + i, utf8_char, stride);
	utf8[i+stride] = '\0';

	l = sms_text_prepare(utf8, 0, use_16bit, NULL);

	g_assert(l == NULL);
	g_free(utf8);
}

static void test_prepare_limits()
{
	gunichar ascii = 0x41;
	gunichar ucs2 = 0x416;
	unsigned int target_size;

	/* The limit for 16 bit headers is 255 * 152 for GSM7 */
	target_size = 255 * 152;
	test_limit(ascii, target_size, TRUE);

	/* The limit for 8 bit headers is 255 * 153 for GSM7 */
	target_size = 255 * 153;
	test_limit(ascii, target_size, FALSE);

	/* The limit for 16 bit headers is 255 * 66 for UCS2 */
	target_size = 255 * 66;
	test_limit(ucs2, target_size, TRUE);

	/* The limit for 8 bit headers is 255 * 67 for UCS2 */
	target_size = 255 * 67;
	test_limit(ucs2, target_size, FALSE);
}

static const char *cbs1 = "011000320111C2327BFC76BBCBEE46A3D168341A8D46A3D1683"
	"41A8D46A3D168341A8D46A3D168341A8D46A3D168341A8D46A3D168341A8D46A3D168"
	"341A8D46A3D168341A8D46A3D168341A8D46A3D168341A8D46A3D100";

static const char *cbs2 = "0110003201114679785E96371A8D46A3D168341A8D46A3D1683"
	"41A8D46A3D168341A8D46A3D168341A8D46A3D168341A8D46A3D168341A8D46A3D168"
	"341A8D46A3D168341A8D46A3D168341A8D46A3D168341A8D46A3D100";

static void test_cbs_encode_decode()
{
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	struct cbs cbs;
	unsigned char pdu[88];
	int len;
	char *encoded_pdu;
	GSList *l;
	char iso639_lang[3];
	char *utf8;

	decoded_pdu = decode_hex(cbs1, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(cbs1) / 2);
	g_assert(pdu_len == 88);

	ret = cbs_decode(decoded_pdu, pdu_len, &cbs);

	g_free(decoded_pdu);

	g_assert(ret);

	g_assert(cbs.gs == CBS_GEO_SCOPE_CELL_IMMEDIATE);
	g_assert(cbs.message_code == 17);
	g_assert(cbs.update_number == 0);
	g_assert(cbs.message_identifier == 50);
	g_assert(cbs.dcs == 1);
	g_assert(cbs.max_pages == 1);
	g_assert(cbs.page == 1);

	l = g_slist_append(NULL, &cbs);

	utf8 = cbs_decode_text(l, iso639_lang);

	g_assert(utf8);

	if (g_test_verbose()) {
		g_printf("%s\n", utf8);
		if (iso639_lang[0] == '\0')
			g_printf("Lang: Unspecified\n");
		else
			g_printf("Lang: %s\n", iso639_lang);
	}

	g_assert(strcmp(utf8, "Belconnen") == 0);
	g_assert(strcmp(iso639_lang, "en") == 0);

	g_free(utf8);

	g_slist_free(l);

	ret = cbs_encode(&cbs, &len, pdu);

	g_assert(ret);

	encoded_pdu = encode_hex(pdu, len, 0);

	g_assert(strcmp(cbs1, encoded_pdu) == 0);

	g_free(encoded_pdu);
}

static void test_cbs_assembly()
{
	unsigned char *decoded_pdu;
	long pdu_len;
	struct cbs dec1;
	struct cbs dec2;
	struct cbs_assembly *assembly;
	char iso639_lang[3];
	GSList *l;
	char *utf8;

	assembly = cbs_assembly_new();

	g_assert(assembly);

	decoded_pdu = decode_hex(cbs1, -1, &pdu_len, 0);
	cbs_decode(decoded_pdu, pdu_len, &dec1);
	g_free(decoded_pdu);

	decoded_pdu = decode_hex(cbs2, -1, &pdu_len, 0);
	cbs_decode(decoded_pdu, pdu_len, &dec2);
	g_free(decoded_pdu);

	/* Add an initial page to the assembly */
	l = cbs_assembly_add_page(assembly, &dec1);
	g_assert(l);
	g_assert(g_slist_length(assembly->recv_cell) == 1);
	g_slist_foreach(l, (GFunc)g_free, NULL);
	g_slist_free(l);

	/* Can we receive new updates ? */
	dec1.update_number = 8;
	l = cbs_assembly_add_page(assembly, &dec1);
	g_assert(l);
	g_assert(g_slist_length(assembly->recv_cell) == 1);
	g_slist_foreach(l, (GFunc)g_free, NULL);
	g_slist_free(l);

	/* Do we ignore old pages ? */
	l = cbs_assembly_add_page(assembly, &dec1);
	g_assert(l == NULL);

	/* Do we ignore older pages ? */
	dec1.update_number = 5;
	l = cbs_assembly_add_page(assembly, &dec1);
	g_assert(l == NULL);

	cbs_assembly_location_changed(assembly, TRUE, TRUE, TRUE);
	g_assert(assembly->recv_cell == NULL);

	dec1.update_number = 9;
	dec1.page = 3;
	dec1.max_pages = 3;

	dec2.update_number = 9;
	dec2.page = 2;
	dec2.max_pages = 3;

	l = cbs_assembly_add_page(assembly, &dec2);
	g_assert(l == NULL);
	l = cbs_assembly_add_page(assembly, &dec1);
	g_assert(l == NULL);

	dec1.page = 1;
	l = cbs_assembly_add_page(assembly, &dec1);
	g_assert(l);

	utf8 = cbs_decode_text(l, iso639_lang);

	g_assert(utf8);

	if (g_test_verbose()) {
		g_printf("%s\n", utf8);
		if (iso639_lang[0] == '\0')
			g_printf("Lang: Unspecified\n");
		else
			g_printf("Lang: %s\n", iso639_lang);
	}

	g_assert(strcmp(utf8, "BelconnenFraserBelconnen") == 0);

	g_free(utf8);
	g_slist_foreach(l, (GFunc)g_free, NULL);
	g_slist_free(l);

	cbs_assembly_free(assembly);
}

static void test_serialize_assembly()
{
	unsigned char pdu[176];
	long pdu_len;
	struct sms sms;
	struct sms_assembly *assembly = sms_assembly_new("1234");
	guint16 ref;
	guint8 max;
	guint8 seq;
	GSList *l;

	decode_hex_own_buf(assembly_pdu1, -1, &pdu_len, 0, pdu);
	sms_decode(pdu, pdu_len, FALSE, assembly_pdu_len1, &sms);

	sms_extract_concatenation(&sms, &ref, &max, &seq);
	l = sms_assembly_add_fragment(assembly, &sms, time(NULL),
					&sms.deliver.oaddr, ref, max, seq);

	if (g_test_verbose()) {
		g_print("Ref: %u\n", ref);
		g_print("Max: %u\n", max);
		g_print("From: %s\n",
				sms_address_to_string(&sms.deliver.oaddr));
	}

	g_assert(g_slist_length(assembly->assembly_list) == 1);
	g_assert(l == NULL);

	decode_hex_own_buf(assembly_pdu2, -1, &pdu_len, 0, pdu);
	sms_decode(pdu, pdu_len, FALSE, assembly_pdu_len2, &sms);

	sms_extract_concatenation(&sms, &ref, &max, &seq);
	l = sms_assembly_add_fragment(assembly, &sms, time(NULL),
					&sms.deliver.oaddr, ref, max, seq);
	g_assert(l == NULL);

	sms_assembly_free(assembly);

	assembly = sms_assembly_new("1234");

	decode_hex_own_buf(assembly_pdu3, -1, &pdu_len, 0, pdu);
	sms_decode(pdu, pdu_len, FALSE, assembly_pdu_len3, &sms);

	sms_extract_concatenation(&sms, &ref, &max, &seq);
	l = sms_assembly_add_fragment(assembly, &sms, time(NULL),
					&sms.deliver.oaddr, ref, max, seq);

	g_assert(l != NULL);

	sms_assembly_free(assembly);
}

static const char *ranges[] = { "1-5, 2, 3, 600, 569-900, 999",
				"0-20, 33, 44, 50-60, 20-50, 1-5, 5, 3, 5",
				NULL };
static const char *inv_ranges[] = { "1-5, 3333", "1-5, afbcd", "1-5, 3-5,,",
					"1-5, 3-5, c", NULL };

static void test_range_minimizer()
{
	int i = 0;

	while (inv_ranges[i]) {
		GSList *l = cbs_extract_topic_ranges(inv_ranges[i]);

		g_assert(l == NULL);
		i++;
	}

	i = 0;

	while (ranges[i]) {
		GSList *r = cbs_extract_topic_ranges(ranges[i]);
		char *rangestr;

		g_assert(r != NULL);
		i++;

		rangestr = cbs_topic_ranges_to_string(r);

		g_assert(rangestr);

		if (g_test_verbose())
			g_print("range: %s\n", rangestr);

		g_free(rangestr);
		g_slist_foreach(r, (GFunc)g_free, NULL);
		g_slist_free(r);
	}
}

int main(int argc, char **argv)
{
	char long_string[152*33 + 1];
	struct sms_concat_data long_string_test;

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testsms/Test Simple Deliver", test_simple_deliver);
	g_test_add_func("/testsms/Test Alnum Deliver", test_alnum_sender);
	g_test_add_func("/testsms/Test Deliver Encode", test_deliver_encode);
	g_test_add_func("/testsms/Test Simple Submit", test_simple_submit);
	g_test_add_func("/testsms/Test Submit Encode", test_submit_encode);
	g_test_add_func("/testsms/Test UDH Iterator", test_udh_iter);
	g_test_add_func("/testsms/Test Assembly", test_assembly);
	g_test_add_func("/testsms/Test Prepare 7Bit", test_prepare_7bit);

	g_test_add_data_func("/testsms/Test Prepare Concat",
			&shakespeare_test, test_prepare_concat);

	memset(long_string, 'a', 152*30);
	memset(long_string + 152*30, 'b', 152);
	memset(long_string + 152*31, 'c', 152);
	memset(long_string + 152*32, 'd', 152);
	long_string[152*33] = '\0';

	long_string_test.str = long_string;
	long_string_test.segments = 33;

	g_test_add_data_func("/testsms/Test Prepare Concat 30+ segments",
			&long_string_test, test_prepare_concat);

	g_test_add_func("/testsms/Test Prepare Limits", test_prepare_limits);

	g_test_add_func("/testsms/Test CBS Encode / Decode",
			test_cbs_encode_decode);
	g_test_add_func("/testsms/Test CBS Assembly", test_cbs_assembly);

	g_test_add_func("/testsms/Test SMS Assembly Serialize",
			test_serialize_assembly);

	g_test_add_func("/testsms/Range minimizer", test_range_minimizer);

	return g_test_run();
}
