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
