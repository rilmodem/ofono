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

static void dump_details(struct sms *sms)
{
	if (sms->sc_addr.address[0] == '\0')
		g_print("SMSC Address absent, default will be used\n");
	else
		g_print("SMSC Address number_type: %d, number_plan: %d, %s\n",
			(int)sms->sc_addr.number_type,
			(int)sms->sc_addr.numbering_plan, sms->sc_addr.address);

	switch (sms->type) {
	case SMS_TYPE_DELIVER:
		g_print("Type: Deliver\n");

		g_print("Originator-Address: %d, %d, %s\n",
			(int)sms->deliver.oaddr.number_type,
			(int)sms->deliver.oaddr.numbering_plan,
			sms->deliver.oaddr.address);

		g_print("PID: %d\n", (int)sms->deliver.pid);
		g_print("DCS: %d\n", (int)sms->deliver.dcs);

		print_scts(&sms->deliver.scts, "Timestamp");

		break;
	case SMS_TYPE_SUBMIT:
		g_print("Type: Submit\n");

		g_print("Message Reference: %u\n", (int)sms->submit.mr);

		g_print("Destination-Address: %d, %d, %s\n",
			(int)sms->submit.daddr.number_type,
			(int)sms->submit.daddr.numbering_plan,
			sms->submit.daddr.address);

		g_print("PID: %d\n", (int)sms->submit.pid);
		g_print("DCS: %d\n", (int)sms->submit.dcs);

		print_vpf(sms->submit.vpf, &sms->submit.vp);

		break;
	case SMS_TYPE_STATUS_REPORT:
		break;
	case SMS_TYPE_COMMAND:
	case SMS_TYPE_DELIVER_REPORT_ACK:
	case SMS_TYPE_DELIVER_REPORT_ERROR:
	case SMS_TYPE_SUBMIT_REPORT_ACK:
	case SMS_TYPE_SUBMIT_REPORT_ERROR:
		break;
	}
}

static void test_simple_deliver(void)
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

	if (g_test_verbose())
		dump_details(&sms);

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

static void test_alnum_sender(void)
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

	if (g_test_verbose())
		dump_details(&sms);

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

static void test_deliver_encode(void)
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

static void test_simple_submit(void)
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

	if (g_test_verbose())
		dump_details(&sms);

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

static void test_submit_encode(void)
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

struct sms_charset_data {
	char *pdu;
	int data_len;
	enum gsm_dialect locking_lang;
	enum gsm_dialect single_lang;
	char expected_text[];
};

static struct sms_charset_data sms_charset_default = {
	.pdu =
		"0001000B91" "5310101010" "1000008080" "8060402818" "0E888462C1"
		"68381E9088" "6442A9582E" "988C06C4E9" "783EA09068" "442A994EA8"
		"946AC56AB9" "5EB0986C46" "ABD96EB89C" "6EC7EBF97E" "C0A070482C"
		"1A8FC8A472" "C96C3A9FD0" "A8744AAD5A" "AFD8AC76CB" "ED7ABFE0B0"
		"784C2E9BCF" "E8B47ACD6E" "BBDFF0B87C" "4EAFDBEFF8" "BC7ECFEFFB"
		"FF",
	.data_len = 112,
	.expected_text = {
		0x40, 0xc2, 0xa3, 0x24, 0xc2, 0xa5, 0xc3, 0xa8, 0xc3, 0xa9,
		0xc3, 0xb9, 0xc3, 0xac, 0xc3, 0xb2, 0xc3, 0x87, 0x0a, 0xc3,
		0x98, 0xc3, 0xb8, 0x0d, 0xc3, 0x85, 0xc3, 0xa5, 0xce, 0x94,
		0x5f, 0xce, 0xa6, 0xce, 0x93, 0xce, 0x9b, 0xce, 0xa9, 0xce,
		0xa0, 0xce, 0xa8, 0xce, 0xa3, 0xce, 0x98, 0xce, 0x9e, 0x20,
		0xc3, 0x86, 0xc3, 0xa6, 0xc3, 0x9f, 0xc3, 0x89, 0x20, 0x21,
		0x22, 0x23, 0xc2, 0xa4, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a,
		0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34,
		0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
		0x3f, 0xc2, 0xa1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
		0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51,
		0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xc3,
		0x84, 0xc3, 0x96, 0xc3, 0x91, 0xc3, 0x9c, 0xc2, 0xa7, 0xc2,
		0xbf, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
		0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73,
		0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0xc3, 0xa4, 0xc3,
		0xb6, 0xc3, 0xb1, 0xc3, 0xbc, 0xc3, 0xa0, 0x00
	}
};

static struct sms_charset_data sms_charset_default_ext = {
	.pdu =
		"0001000B91" "5310101010" "100000151B" "C58602DAA0" "36A9CD6BC3"
		"DBF436BE0D" "705306",
	.data_len = 19,
	.expected_text = {
		0x0c, 0x5e, 0x20, 0x7b,	0x7d, 0x5c, 0x5b, 0x7e,	0x5d, 0x7c,
		0xe2, 0x82, 0xac, 0x00
	}
};

static struct sms_charset_data sms_charset_turkey = {
	.pdu =
		"0001000B91" "5310101010" "1000008080" "8060402818" "0E888462C1"
		"68381E9088" "6442A9582E" "988C06C4E9" "783EA09068" "442A994EA8"
		"946AC56AB9" "5EB0986C46" "ABD96EB89C" "6EC7EBF97E" "C0A070482C"
		"1A8FC8A472" "C96C3A9FD0" "A8744AAD5A" "AFD8AC76CB" "ED7ABFE0B0"
		"784C2E9BCF" "E8B47ACD6E" "BBDFF0B87C" "4EAFDBEFF8" "BC7ECFEFFB"
		"FF",
	.data_len = 112,
	.locking_lang = GSM_DIALECT_TURKISH,
	.expected_text = {
		0x40, 0xc2, 0xa3, 0x24, 0xc2, 0xa5, 0xe2, 0x82, 0xac, 0xc3,
		0xa9, 0xc3, 0xb9, 0xc4, 0xb1, 0xc3, 0xb2, 0xc3, 0x87, 0x0a,
		0xc4, 0x9e, 0xc4, 0x9f, 0x0d, 0xc3, 0x85, 0xc3, 0xa5, 0xce,
		0x94, 0x5f, 0xce, 0xa6, 0xce, 0x93, 0xce, 0x9b, 0xce, 0xa9,
		0xce, 0xa0, 0xce, 0xa8, 0xce, 0xa3, 0xce, 0x98, 0xce, 0x9e,
		0x20, 0xc5, 0x9e, 0xc5, 0x9f, 0xc3, 0x9f, 0xc3, 0x89, 0x20,
		0x21, 0x22, 0x23, 0xc2, 0xa4, 0x25, 0x26, 0x27, 0x28, 0x29,
		0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
		0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d,
		0x3e, 0x3f, 0xc4, 0xb0, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
		0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50,
		0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
		0xc3, 0x84, 0xc3, 0x96, 0xc3, 0x91, 0xc3, 0x9c, 0xc2, 0xa7,
		0xc3, 0xa7, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
		0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72,
		0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0xc3, 0xa4,
		0xc3, 0xb6, 0xc3, 0xb1, 0xc3, 0xbc, 0xc3, 0xa0, 0x00
	}
};

static struct sms_charset_data sms_charset_turkey_ext = {
	.pdu =
		"0001000B91" "5310101010" "1000001A1B" "C586B2416D" "529BD786B7"
		"E96D7C1BE0" "02C8011318" "870E",
	.data_len = 23,
	.locking_lang = GSM_DIALECT_TURKISH,
	.single_lang = GSM_DIALECT_TURKISH,
	.expected_text = {
		0x0c, 0x5e, 0x7b, 0x7d, 0x5c, 0x5b, 0x7e, 0x5d, 0x7c, 0xc4,
		0x9e, 0xc4, 0xb0, 0xc5, 0x9e, 0xc3, 0xa7, 0xe2, 0x82, 0xac,
		0xc4, 0x9f, 0xc4, 0xb1, 0xc5, 0x9f, 0x00
	}
};

static struct sms_charset_data sms_charset_portugal = {
	.pdu =
		"0001000B91" "5310101010" "1000008080" "8060402818" "0E888462C1"
		"68381E9088" "6442A9582E" "988C06C4E9" "783EA09068" "442A994EA8"
		"946AC56AB9" "5EB0986C46" "ABD96EB89C" "6EC7EBF97E" "C0A070482C"
		"1A8FC8A472" "C96C3A9FD0" "A8744AAD5A" "AFD8AC76CB" "ED7ABFE0B0"
		"784C2E9BCF" "E8B47ACD6E" "BBDFF0B87C" "4EAFDBEFF8" "BC7ECFEFFB"
		"FF",
	.data_len = 112,
	.locking_lang = GSM_DIALECT_PORTUGUESE,
	.expected_text = {
		0x40, 0xc2, 0xa3, 0x24, 0xc2, 0xa5, 0xc3, 0xaa, 0xc3, 0xa9,
		0xc3, 0xba, 0xc3, 0xad, 0xc3, 0xb3, 0xc3, 0xa7, 0x0a, 0xc3,
		0x94, 0xc3, 0xb4, 0x0d, 0xc3, 0x81, 0xc3, 0xa1, 0xce, 0x94,
		0x5f, 0xc2, 0xaa, 0xc3, 0x87, 0xc3, 0x80, 0xe2, 0x88, 0x9e,
		0x5e, 0x5c, 0xe2, 0x82, 0xac, 0xc3, 0x93, 0x7c, 0x20, 0xc3,
		0x82, 0xc3, 0xa2, 0xc3, 0x8a, 0xc3, 0x89, 0x20, 0x21, 0x22,
		0x23, 0xc2, 0xba, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
		0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
		0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
		0xc3, 0x8d, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
		0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52,
		0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xc3, 0x83,
		0xc3, 0x95, 0xc3, 0x9a, 0xc3, 0x9c, 0xc2, 0xa7, 0x7e, 0x61,
		0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
		0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
		0x76, 0x77, 0x78, 0x79, 0x7a, 0xc3, 0xa3, 0xc3, 0xb5, 0x60,
		0xc3, 0xbc, 0xc3, 0xa0, 0x00
	}
};

static struct sms_charset_data sms_charset_portugal_ext = {
	.pdu =
		"0001000B91" "5310101010" "1000003184" "C446B16038" "1E1BC96662"
		"D9543696CD" "6583D9643C" "1BD42675D9" "F0C01B9F86" "02CC74B75C"
		"0EE68030EC" "F91D",
	.data_len = 43,
	.locking_lang = GSM_DIALECT_PORTUGUESE,
	.single_lang = GSM_DIALECT_PORTUGUESE,
	.expected_text = {
		0xc3, 0xaa, 0xc3, 0xa7, 0x0c, 0xc3, 0x94, 0xc3, 0xb4, 0xc3,
		0x81, 0xc3, 0xa1, 0xce, 0xa6, 0xce, 0x93, 0x5e, 0xce, 0xa9,
		0xce, 0xa0, 0xce, 0xa8, 0xce, 0xa3, 0xce, 0x98, 0xc3, 0x8a,
		0x7b, 0x7d, 0x5c, 0x5b, 0x7e, 0x5d, 0x7c, 0xc3, 0x80, 0xc3,
		0x8d, 0xc3, 0x93, 0xc3, 0x9a, 0xc3, 0x83, 0xc3, 0x95, 0xc3,
		0x82, 0xe2, 0x82, 0xac, 0xc3, 0xad, 0xc3, 0xb3, 0xc3, 0xba,
		0xc3, 0xa3, 0xc3, 0xb5, 0xc3, 0xa2, 0x00
	}
};

static struct sms_charset_data sms_charset_spain = {
	.pdu =
		"0001000B91" "5310101010" "100000269B" "C446B1A16C" "509BD4E6B5"
		"E16D7A1BDF" "06B8096E92" "9BE7A6BA09" "6FCA9BF4E6" "BDA903",
	.data_len = 34,
	.locking_lang = GSM_DIALECT_SPANISH,
	.single_lang = GSM_DIALECT_SPANISH,
	.expected_text = {
		0xc3, 0xa7, 0x0c, 0x5e, 0x7b, 0x7d, 0x5c, 0x5b, 0x7e, 0x5d,
		0x7c, 0xc3, 0x81, 0xc3, 0x8d, 0xc3, 0x93, 0xc3, 0x9a, 0xc3,
		0xa1, 0xe2, 0x82, 0xac, 0xc3, 0xad, 0xc3, 0xb3, 0xc3, 0xba,
		0x00
	}
};

static void test_sms_charset(gconstpointer param)
{
	gboolean ret;
	struct sms sms;
	unsigned char *pdu;
	unsigned char *unpacked;
	long pdu_len;
	int data_len;
	enum sms_charset sms_charset;
	gboolean sms_compressed;
	char *text;
	struct sms_charset_data *data = (struct sms_charset_data *)param;

	pdu = decode_hex(data->pdu, -1, &pdu_len, 0);

	g_assert(pdu);
	g_assert(pdu_len == (gint64)strlen(data->pdu) / 2);

	ret = sms_decode(pdu, pdu_len, FALSE, pdu_len, &sms);

	g_assert(ret);

	g_free(pdu);

	g_assert(sms.type == SMS_TYPE_DELIVER);

	ret = sms_dcs_decode(sms.deliver.dcs, NULL, &sms_charset,
				&sms_compressed, NULL);

	g_assert(ret);
	g_assert(sms_charset == SMS_CHARSET_7BIT);
	g_assert(sms_compressed == FALSE);

	data_len = sms_udl_in_bytes(sms.deliver.udl, sms.deliver.dcs);

	g_assert(data_len == data->data_len);

	unpacked = unpack_7bit(sms.deliver.ud, data_len, 0, FALSE,
				sms.deliver.udl, NULL, 0xff);

	g_assert(unpacked);

	text = convert_gsm_to_utf8_with_lang(unpacked, -1, NULL, NULL, 0xff,
		data->locking_lang, data->single_lang);

	g_assert(text);

	g_free(unpacked);

	g_assert(strcmp(data->expected_text, text) == 0);

	g_free(text);
}

struct text_format_header {
	unsigned char len;
	unsigned char start;
	unsigned char span;
	unsigned char format;
	unsigned char color;
};

struct ems_udh_test {
	const char *pdu;
	unsigned int len;
	const char *expected;
	unsigned int udl;
	unsigned int udhl;
	unsigned int data_len;
	struct text_format_header formats[];
};

static struct ems_udh_test ems_udh_test_1 = {
	.pdu = "0041000B915121551532F40000631A0A031906200A032104100A03270504"
		"0A032E05080A043807002B8ACD29A85D9ECFC3E7F21C340EBB41E3B79B1"
		"E4EBB41697A989D1EB340E2379BCC02B1C3F27399059AB7C36C3628EC26"
		"83C66FF65B5E2683E8653C1D",
	.len = 100,
	.expected = "EMS messages can contain italic, bold, large, small and"
		" colored text",
	.formats = {
		{
			.len = 3,
			.start = 0x19,
			.span = 0x06,
			.format = 0x20,
		},
		{
			.len = 3,
			.start = 0x21,
			.span = 0x04,
			.format = 0x10,
		},
		{
			.len = 3,
			.start = 0x27,
			.span = 0x05,
			.format = 0x04,
		},
		{
			.len = 3,
			.start = 0x2E,
			.span = 0x05,
			.format = 0x08,
		},
		{
			.len = 4,
			.start = 0x38,
			.span = 0x07,
			.format = 0x00,
			.color = 0x2B,
		},
		{
			.len = 0,
		}
	},
	.udl = 99,
	.udhl = 26,
	.data_len = 87,
};

static struct ems_udh_test ems_udh_test_2 = {
	.pdu = "079194712272303351030B915121340195F60000FF80230A030F07230A031"
		"806130A031E0A430A032E0D830A033D14020A035104F60A0355010600159"
		"D9E83D2735018442FCFE98A243DCC4E97C92C90F8CD26B3407537B92C67A"
		"7DD65320B1476934173BA3CBD2ED3D1F277FD8C76299CEF3B280C92A7CF6"
		"83A28CC4E9FDD6532E8FE96935D",
	.len = 126,
	.expected = "This is a test\nItalied, bold, underlined, and "
		"strikethrough.\nNow a right aligned word.",
	.formats = {
		{
			.len = 3,
			.start = 0x0f,
			.span = 0x07,
			.format = 0x23,
		},
		{
			.len = 3,
			.start = 0x18,
			.span = 0x06,
			.format = 0x13,
		},
		{
			.len = 3,
			.start = 0x1e,
			.span = 0x0a,
			.format = 0x43,
		},
		{
			.len = 3,
			.start = 0x2e,
			.span = 0x0d,
			.format = 0x83,
		},
		{
			.len = 3,
			.start = 0x3d,
			.span = 0x14,
			.format = 0x02,
		},
		{
			.len = 3,
			.start = 0x51,
			.span = 0x04,
			.format = 0xf6,
		},
		{
			.len = 3,
			.start = 0x55,
			.span = 0x01,
			.format = 0x06,
		},
	},
	.udl = 128,
	.udhl = 35,
	.data_len = 112,
};

static void test_ems_udh(gconstpointer data)
{
	const struct ems_udh_test *test = data;
	struct sms sms;
	unsigned char *decoded_pdu;
	long pdu_len;
	gboolean ret;
	unsigned int data_len;
	unsigned int udhl;
	struct sms_udh_iter iter;
	int max_chars;
	unsigned char *unpacked;
	char *utf8;
	int i;

	decoded_pdu = decode_hex(test->pdu, -1, &pdu_len, 0);

	g_assert(decoded_pdu);
	g_assert(pdu_len == (long)strlen(test->pdu) / 2);

	ret = sms_decode(decoded_pdu, pdu_len, TRUE, test->len, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_SUBMIT);

	if (g_test_verbose())
		dump_details(&sms);
	udhl = sms.submit.ud[0];

	g_assert(sms.submit.udl == test->udl);
	g_assert(udhl == test->udhl);

	ret = sms_udh_iter_init(&sms, &iter);

	g_assert(ret);

	for (i = 0; test->formats[i].len; i++) {
		if (g_test_verbose()) {
			int j;
			unsigned char data[4];

			sms_udh_iter_get_ie_data(&iter, data);

			g_print("Header:\n");
			for (j = 0; j < sms_udh_iter_get_ie_length(&iter); j++)
				g_print("0x%02x ", data[j]);

			g_print("\n");
		}

		g_assert(sms_udh_iter_get_ie_type(&iter) ==
				SMS_IEI_TEXT_FORMAT);
		g_assert(sms_udh_iter_get_ie_length(&iter) ==
				test->formats[i].len);

		if (test->formats[i+1].len) {
			g_assert(sms_udh_iter_has_next(&iter) == TRUE);
			g_assert(sms_udh_iter_next(&iter) == TRUE);
		} else {
			g_assert(sms_udh_iter_has_next(&iter) == FALSE);
			g_assert(sms_udh_iter_next(&iter) == FALSE);
			g_assert(sms_udh_iter_get_ie_type(&iter) ==
					SMS_IEI_INVALID);
		}
	}

	data_len = sms_udl_in_bytes(sms.submit.udl, sms.submit.dcs);

	g_assert(data_len == test->data_len);

	max_chars = (data_len - (udhl + 1)) * 8 / 7;

	unpacked = unpack_7bit(sms.submit.ud + udhl + 1, data_len - (udhl + 1),
				udhl + 1, FALSE, max_chars, NULL, 0xff);

	g_assert(unpacked);

	utf8 = convert_gsm_to_utf8(unpacked, -1, NULL, NULL, 0xff);

	g_free(unpacked);

	g_assert(utf8);

	if (g_test_verbose())
		g_print("Decoded user data is: %s\n", utf8);

	g_assert(strcmp(utf8, test->expected) == 0);

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

static void test_assembly(void)
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

	l = sms_text_prepare("555", utf8, ref, TRUE, FALSE);
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
static void test_prepare_7bit(void)
{
	GSList *r;
	struct sms *sms;
	gboolean ret;
	unsigned char pdu[176];
	int encoded_pdu_len;
	int encoded_tpdu_len;
	char *encoded_pdu;

	r = sms_text_prepare("555", test_no_fragmentation_7bit, 0,
				FALSE, FALSE);

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

	r = sms_text_prepare("+15554449999", test->str, 0, TRUE, FALSE);
	g_assert(r);
	g_assert(g_slist_length(r) == test->segments);

	for (l = r; l; l = l->next) {
		char *strpdu;

		sms = l->data;

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

	l = sms_text_prepare("555", utf8, 0, use_16bit, FALSE);

	g_assert(l);
	g_assert(g_slist_length(l) == 255);

	decoded = sms_decode_text(l);
	g_assert(g_utf8_strlen(decoded, -1) == target_size);

	g_free(decoded);

	memcpy(utf8 + i, utf8_char, stride);
	utf8[i+stride] = '\0';

	l = sms_text_prepare("555", utf8, 0, use_16bit, FALSE);

	g_assert(l == NULL);
	g_free(utf8);
}

static void test_prepare_limits(void)
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

static void test_cbs_encode_decode(void)
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

static void test_cbs_assembly(void)
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

static const char *ranges[] = { "1-5, 2, 3, 600, 569-900, 999",
				"0-20, 33, 44, 50-60, 20-50, 1-5, 5, 3, 5",
				NULL };
static const char *inv_ranges[] = { "1-5, 3333", "1-5, afbcd", "1-5, 3-5,,",
					"1-5, 3-5, c", NULL };

static void test_range_minimizer(void)
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

static void test_sr_assembly(void)
{
	const char *sr_pdu1 = "06040D91945152991136F00160124130340A0160124130"
				"940A00";
	const char *sr_pdu2 = "06050D91945152991136F00160124130640A0160124130"
				"450A00";
	const char *sr_pdu3 = "0606098121436587F9019012413064A0019012413045A0"
				"00";
        struct sms sr1;
	struct sms sr2;
	struct sms sr3;
	unsigned char pdu[176];
	long pdu_len;
	struct status_report_assembly *sra;
	gboolean delivered;
	struct sms_address addr;
	unsigned char sha1[SMS_MSGID_LEN] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
						10, 11, 12, 13, 14, 15,
						16, 17, 18, 19 };
	unsigned char id[SMS_MSGID_LEN];

	/* international address, mr 4 & mr 5 */

        decode_hex_own_buf(sr_pdu1, -1, &pdu_len, 0, pdu);
	g_assert(sms_decode(pdu, pdu_len, FALSE, 26, &sr1) == TRUE);

	decode_hex_own_buf(sr_pdu2, -1, &pdu_len, 0, pdu);
	g_assert(sms_decode(pdu, pdu_len, FALSE, 26, &sr2) == TRUE);

	/* national address, mr 6 */

	decode_hex_own_buf(sr_pdu3, -1, &pdu_len, 0, pdu);
	g_assert(sms_decode(pdu, pdu_len, FALSE, 24, &sr3) == TRUE);

	if (g_test_verbose()) {
		g_print("sr1 address: %s, mr: %d\n",
			sms_address_to_string(&sr1.status_report.raddr),
			sr1.status_report.mr);

		g_print("sr2 address: %s, mr: %d\n",
			sms_address_to_string(&sr2.status_report.raddr),
			sr2.status_report.mr);

		g_print("sr3 address: %s, mr: %d\n",
			sms_address_to_string(&sr3.status_report.raddr),
			sr3.status_report.mr);
	}

	sms_address_from_string(&addr, "+4915259911630");

	sra = status_report_assembly_new(NULL);

	status_report_assembly_add_fragment(sra, sha1, &addr, 4, time(NULL), 2);
	status_report_assembly_add_fragment(sra, sha1, &addr, 5, time(NULL), 2);

	status_report_assembly_expire(sra, time(NULL) + 40);
	g_assert(g_hash_table_size(sra->assembly_table) == 0);

	status_report_assembly_add_fragment(sra, sha1, &addr, 4, time(NULL), 2);
	status_report_assembly_add_fragment(sra, sha1, &addr, 5, time(NULL), 2);

	g_assert(!status_report_assembly_report(sra, &sr1, id, &delivered));
	g_assert(status_report_assembly_report(sra, &sr2, id, &delivered));

	g_assert(memcmp(id, sha1, SMS_MSGID_LEN) == 0);
	g_assert(delivered == TRUE);

	/*
	 * Send sms-message in the national address-format,
	 * but receive in the international address-format.
	 */
	sms_address_from_string(&addr, "9911630");
	status_report_assembly_add_fragment(sra, sha1, &addr, 4, time(NULL), 2);
	status_report_assembly_add_fragment(sra, sha1, &addr, 5, time(NULL), 2);

	g_assert(!status_report_assembly_report(sra, &sr1, id, &delivered));
	g_assert(status_report_assembly_report(sra, &sr2, id, &delivered));

	g_assert(memcmp(id, sha1, SMS_MSGID_LEN) == 0);
	g_assert(delivered == TRUE);
	g_assert(g_hash_table_size(sra->assembly_table) == 0);

	/*
	 * Send sms-message in the international address-format,
	 * but receive in the national address-format.
	 */
	sms_address_from_string(&addr, "+358123456789");
	status_report_assembly_add_fragment(sra, sha1, &addr, 6, time(NULL), 1);

	g_assert(status_report_assembly_report(sra, &sr3, id, &delivered));

	g_assert(memcmp(id, sha1, SMS_MSGID_LEN) == 0);
	g_assert(delivered == TRUE);
	g_assert(g_hash_table_size(sra->assembly_table) == 0);

	status_report_assembly_free(sra);
}

struct wap_push_data {
	const char *pdu;
	int len;
};

static struct wap_push_data wap_push_1 = {
	.pdu = "0791947122725014440185F039F501801140311480720605040B8423F00106"
		"246170706C69636174696F6E2F766E642E7761702E6D6D732D6D657373616"
		"76500AF84B4868C82984F67514B4B42008D9089088045726F74696B009650"
		"696E2D557073008A808E0240008805810303F48083687474703A2F2F65707"
		"3332E64652F4F2F5A39495A4F00",
	.len = 128,
};

static void test_wap_push(gconstpointer data)
{
	const struct wap_push_data *test = data;
	struct sms sms;
	unsigned char *decoded_pdu;
	gboolean ret;
	long pdu_len;
	long data_len;
	enum sms_class cls;
	enum sms_charset charset;
	GSList *list;
	unsigned char *wap_push;
	int dst_port, src_port;
	gboolean is_8bit;

	decoded_pdu = decode_hex(test->pdu, -1, &pdu_len, 0);

	g_assert(decoded_pdu);

	ret = sms_decode(decoded_pdu, pdu_len, FALSE, test->len, &sms);

	g_free(decoded_pdu);

	g_assert(ret);
	g_assert(sms.type == SMS_TYPE_DELIVER);

	if (g_test_verbose())
		dump_details(&sms);

	ret = sms_dcs_decode(sms.deliver.dcs, &cls, &charset, NULL, NULL);

	g_assert(ret == TRUE);
	g_assert(charset == SMS_CHARSET_8BIT);

	g_assert(sms_extract_app_port(&sms, &dst_port, &src_port, &is_8bit));

	if (g_test_verbose()) {
		g_print("8bit: %d\n", is_8bit);
		g_print("src: %d, dst: %d\n", src_port, dst_port);
	}

	g_assert(is_8bit == FALSE);
	g_assert(dst_port == 2948);

	list = g_slist_append(NULL, &sms);

	wap_push = sms_decode_datagram(list, &data_len);

	if (g_test_verbose()) {
		int i;

		g_print("data_len: %ld\n", data_len);

		for (i = 0; i < data_len; i++) {
			g_print("%02x", wap_push[i]);

			if ((i % 16) == 15)
				g_print("\n");
		}

		g_print("\n");
	}

	g_assert(wap_push);

	g_free(wap_push);
	g_slist_free(list);
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

	g_test_add_data_func("/testsms/Test "
		"GSM 7 bit Default Alphabet Decode",
		&sms_charset_default, test_sms_charset);

	g_test_add_data_func("/testsms/Test "
		"GSM 7 bit Default Alphabet Extension Table Decode",
		&sms_charset_default_ext, test_sms_charset);

	g_test_add_data_func("/testsms/Test "
		"Turkish National Language Locking Shift Table Decode",
		&sms_charset_turkey, test_sms_charset);

	g_test_add_data_func("/testsms/Test "
		"Turkish National Language Single Shift Table Decode",
		&sms_charset_turkey_ext, test_sms_charset);

	g_test_add_data_func("/testsms/Test "
		"Portuguese National Language Locking Shift Table Decode",
		&sms_charset_portugal, test_sms_charset);

	g_test_add_data_func("/testsms/Test "
		"Portuguese National Language Single Shift Table Decode",
		&sms_charset_portugal_ext, test_sms_charset);

	g_test_add_data_func("/testsms/Test "
		"Spanish National Language Single Shift Table Decode",
		&sms_charset_spain, test_sms_charset);

	g_test_add_data_func("/testsms/Test EMS UDH 1",
			&ems_udh_test_1, test_ems_udh);
	g_test_add_data_func("/testsms/Test EMS UDH 2",
			&ems_udh_test_2, test_ems_udh);

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

	g_test_add_func("/testsms/Range minimizer", test_range_minimizer);

	g_test_add_func("/testsms/Status Report Assembly", test_sr_assembly);

	g_test_add_data_func("/testsms/Test WAP Push 1", &wap_push_1,
				test_wap_push);

	return g_test_run();
}
