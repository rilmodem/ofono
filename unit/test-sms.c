/*
 * oFono - GSM Telephony Stack for Linux
 *
 * Copyright (C) 2008-2009 Intel Corporation.  All rights reserved.
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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

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
	g_print("%s: (YY-MM-DD) %02d-%02d-%02d\n", prefix,
		(int)scts->year, (int)scts->month, (int)scts->day);

	g_print("%s: (HH-MM-SS) %02d:%02d:%02d\n", prefix,
		(int)scts->hour, (int)scts->minute, (int)scts->second);

	g_print("%s: Timezone %d hours %d minutes\n", prefix,
		(int)scts->timezone / 4,
		(int)((abs(scts->timezone) % 4) * 15));
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

	ret = decode_sms(decoded_pdu, pdu_len, FALSE, 30, &sms);

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

	data_len = ud_len_in_octets(sms.deliver.udl, sms.deliver.dcs);

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

	ret = decode_sms(decoded_pdu, pdu_len, FALSE, 27, &sms);

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

	data_len = ud_len_in_octets(sms.deliver.udl, sms.deliver.dcs);

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

	ret = decode_sms(decoded_pdu, pdu_len, FALSE, 30, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_DELIVER);

	ret = encode_sms(&sms, &encoded_pdu_len, &encoded_tpdu_len, pdu);

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

	ret = decode_sms(decoded_pdu, pdu_len, FALSE, 27, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_DELIVER);

	ret = encode_sms(&sms, &encoded_pdu_len, &encoded_tpdu_len, pdu);

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

	ret = decode_sms(decoded_pdu, pdu_len, TRUE, 23, &sms);

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

		g_print("Validity Period Format: %d\n", (int)sms.submit.vpf);

		switch (sms.submit.vpf) {
		case SMS_VALIDITY_PERIOD_FORMAT_ABSENT:
			g_print("Validity-Period: Absent\n");
			break;
		case SMS_VALIDITY_PERIOD_FORMAT_RELATIVE:
			g_print("Validity-Period: %d\n",
				(int)sms.submit.vp.relative);
			break;
		case SMS_VALIDITY_PERIOD_FORMAT_ABSOLUTE:
			print_scts(&sms.submit.vp.absolute, "Validity-Period:");
			break;
		case SMS_VALIDITY_PERIOD_FORMAT_ENHANCED:
			g_print("Validity-Period: Enhanced");
			break;
		}
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

	data_len = ud_len_in_octets(sms.submit.udl, sms.submit.dcs);

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

	ret = decode_sms(decoded_pdu, pdu_len, TRUE, 23, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_SUBMIT);

	ret = encode_sms(&sms, &encoded_pdu_len, &encoded_tpdu_len, pdu);

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

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testsms/Test Simple Deliver", test_simple_deliver);
	g_test_add_func("/testsms/Test Alnum Deliver", test_alnum_sender);
	g_test_add_func("/testsms/Test Deliver Encode", test_deliver_encode);
	g_test_add_func("/testsms/Test Simple Submit", test_simple_submit);
	g_test_add_func("/testsms/Test Submit Encode", test_submit_encode);

	return g_test_run();
}
