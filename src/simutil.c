/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <glib.h>

#include "simutil.h"
#include "util.h"

/* Parse ASN.1 Basic Encoding Rules TLVs per ISO/IEC 7816 */
const guint8 *ber_tlv_find_by_tag(const guint8 *pdu, guint8 in_tag,
					int in_len, int *out_len)
{
	guint8 tag;
	int len;
	const guint8 *end = pdu + in_len;

	do {
		while (pdu < end && (*pdu == 0x00 || *pdu == 0xff))
			pdu ++;
		if (pdu == end)
			break;

		tag = *pdu ++;
		if (!(0x1f & ~tag))
			while (pdu < end && (*pdu ++ & 0x80));
		if (pdu == end)
			break;

		for (len = 0; pdu + 1 < end && (*pdu & 0x80);
				len = (len | (*pdu ++ & 0x7f)) << 7);
		if (*pdu & 0x80)
			break;
		len |= *pdu ++;

		if (tag == in_tag && pdu + len <= end) {
			if (out_len)
				*out_len = len;
			return pdu;
		}

		pdu += len;
	} while (pdu < end);

	return NULL;
}

char *sim_network_name_parse(const unsigned char *buffer, int length,
				gboolean *add_ci)
{
	char *ret = NULL;
	unsigned char *endp;
	unsigned char dcs;
	int i;
	gboolean ci = FALSE;

	if (length < 1)
		return NULL;

	dcs = *buffer ++;
	length --;

	/* "The MS should add the letters for the Country's Initials and a
	 * separator (e.g. a space)" */
	if (is_bit_set(dcs, 4))
		ci = TRUE;

	switch (dcs & (7 << 4)) {
	case 0x00:
		endp = memchr(buffer, 0xff, length);
		if (endp)
			length = endp - buffer;
		ret = convert_gsm_to_utf8(buffer, length,
				NULL, NULL, 0xff);
		break;
	case 0x10:
		if ((length % 2) == 1) {
			if (buffer[length - 1] != 0xff)
				return NULL;

			length = length - 1;
		}

		for (i = 0; i < length; i += 2)
			if (buffer[i] == 0xff && buffer[i + 1] == 0xff)
				break;

		ret = g_convert(buffer, length, "UTF-8//TRANSLIT", "UCS-2BE",
					NULL, NULL, NULL);
		break;
	}

	if (add_ci)
		*add_ci = ci;

	return ret;
}

struct sim_pnn_operator *sim_pnn_operator_parse(const guint8 *tlv, int length)
{
	const char *name;
	int namelength;
	gboolean add_ci;
	struct sim_pnn_operator *oper;

	name = ber_tlv_find_by_tag(tlv, 0x43, length, &namelength);

	if (!name || !namelength)
		return NULL;

	oper = g_new0(struct sim_pnn_operator, 1);

	oper->longname = sim_network_name_parse(name, namelength,
						&oper->long_ci);

	name = ber_tlv_find_by_tag(tlv, 0x45, length, &namelength);

	if (name && namelength)
		oper->shortname = sim_network_name_parse(name, namelength,
							&oper->short_ci);

	name = ber_tlv_find_by_tag(tlv, 0x80, length, &namelength);

	if (name && namelength)
		oper->info = sim_string_to_utf8(name, namelength);

	return oper;
}

void sim_pnn_operator_free(struct sim_pnn_operator *oper)
{
	g_free(oper->info);
	g_free(oper->shortname);
	g_free(oper->longname);
}
