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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "util.h"
#include "smsutil.h"

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

static void test_serialize_assembly(void)
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

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testsms/Test SMS Assembly Serialize",
			test_serialize_assembly);

	return g_test_run();
}
