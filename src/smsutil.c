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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>

#include "util.h"
#include "storage.h"
#include "smsutil.h"

#define uninitialized_var(x) x = x

#define SMS_BACKUP_MODE 0600
#define SMS_BACKUP_PATH STORAGEDIR "/%s/sms_assembly"
#define SMS_BACKUP_PATH_DIR SMS_BACKUP_PATH "/%s-%i-%i"
#define SMS_BACKUP_PATH_FILE SMS_BACKUP_PATH_DIR "/%03i"

#define SMS_SR_BACKUP_PATH STORAGEDIR "/%s/sms_sr"
#define SMS_SR_BACKUP_PATH_FILE SMS_SR_BACKUP_PATH "/%s-%s"

#define SMS_TX_BACKUP_PATH STORAGEDIR "/%s/tx_queue"
#define SMS_TX_BACKUP_PATH_DIR SMS_TX_BACKUP_PATH "/%lu-%lu-%s"
#define SMS_TX_BACKUP_PATH_FILE SMS_TX_BACKUP_PATH_DIR "/%03i"

#define SMS_ADDR_FMT "%24[0-9A-F]"
#define SMS_MSGID_FMT "%40[0-9A-F]"

/*
 * Time zone accounts for daylight saving time, and the two extreme time
 * zones on earth are UTC-12 and UTC+14.
 */
#define MAX_TIMEZONE 56
#define MIN_TIMEZONE -48

static GSList *sms_assembly_add_fragment_backup(struct sms_assembly *assembly,
					const struct sms *sms, time_t ts,
					const struct sms_address *addr,
					guint16 ref, guint8 max, guint8 seq,
					gboolean backup);

/*
 * This function uses the meanings of digits 10..15 according to the rules
 * defined in 23.040 Section 9.1.2.3 and 24.008 Table 10.5.118
 */
void extract_bcd_number(const unsigned char *buf, int len, char *out)
{
	static const char digit_lut[] = "0123456789*#abc\0";
	unsigned char oct;
	int i;

	for (i = 0; i < len; i++) {
		oct = buf[i];

		out[i*2] = digit_lut[oct & 0x0f];
		out[i*2+1] = digit_lut[(oct & 0xf0) >> 4];
	}

	out[i*2] = '\0';
}

static inline int to_semi_oct(char in)
{
	int digit;

	switch (in) {
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		digit = in - '0';
		break;
	case '*':
		digit = 10;
		break;
	case '#':
		digit = 11;
		break;
	case 'A':
	case 'a':
		digit = 12;
		break;
	case 'B':
	case 'b':
		digit = 13;
		break;
	case 'C':
	case 'c':
		digit = 14;
		break;
	default:
		digit = -1;
		break;
	}

	return digit;
}

void encode_bcd_number(const char *number, unsigned char *out)
{
	while (number[0] != '\0' && number[1] != '\0') {
		*out = to_semi_oct(*number++);
		*out++ |= to_semi_oct(*number++) << 4;
	}

	if (*number)
		*out = to_semi_oct(*number) | 0xf0;
}

/*
 * Returns whether the DCS could be parsed successfully, e.g. no reserved
 * values were used
 */
gboolean sms_dcs_decode(guint8 dcs, enum sms_class *cls,
			enum sms_charset *charset,
			gboolean *compressed, gboolean *autodelete)
{
	guint8 upper = (dcs & 0xf0) >> 4;
	enum sms_charset ch;
	enum sms_class cl;
	gboolean comp;
	gboolean autodel;

	/* MWI DCS types are handled in sms_mwi_dcs_decode */
	if (upper >= 0x8 && upper <= 0xE)
		return FALSE;

	upper = (dcs & 0xc0) >> 6;

	switch (upper) {
	case 0:
	case 1:
		autodel = upper;
		comp = (dcs & 0x20) ? TRUE : FALSE;

		if (dcs & 0x10)
			cl = (enum sms_class) (dcs & 0x03);
		else
			cl = SMS_CLASS_UNSPECIFIED;

		if (((dcs & 0x0c) >> 2) < 3)
			ch = (enum sms_charset) ((dcs & 0x0c) >> 2);
		else
			return FALSE;

		break;
	case 3:
		comp = FALSE;
		autodel = FALSE;

		if (dcs & 0x4)
			ch = SMS_CHARSET_8BIT;
		else
			ch = SMS_CHARSET_7BIT;

		cl = (enum sms_class) (dcs & 0x03);

		break;
	default:
		return FALSE;
	};

	if (compressed)
		*compressed = comp;

	if (autodelete)
		*autodelete = autodel;

	if (cls)
		*cls = cl;

	if (charset)
		*charset = ch;

	return TRUE;
}

gboolean sms_mwi_dcs_decode(guint8 dcs, enum sms_mwi_type *type,
				enum sms_charset *charset,
				gboolean *active, gboolean *discard)
{
	guint8 upper = (dcs & 0xf0) >> 4;
	enum sms_mwi_type t;
	enum sms_charset ch;
	gboolean dis;
	gboolean act;

	if (upper < 0xC || upper > 0xE)
		return FALSE;

	upper = (dcs & 0x30) >> 4;

	if (upper == 0)
		dis = TRUE;
	else
		dis = FALSE;

	/*
	 * As per 3GPP TS 23.038 specification, if bits 7..4 set to 1110,
	 * text included in the user data is coded in the uncompresssed
	 * UCS2 character set.
	 */
	if (upper == 2)
		ch = SMS_CHARSET_UCS2;
	else
		ch = SMS_CHARSET_7BIT;

	act = dcs & 0x8;

	t = (enum sms_mwi_type) (dcs & 0x3);

	if (type)
		*type = t;

	if (charset)
		*charset = ch;

	if (active)
		*active = act;

	if (discard)
		*discard = dis;

	return TRUE;
}

int sms_udl_in_bytes(guint8 ud_len, guint8 dcs)
{
	int len_7bit = (ud_len + 1) * 7 / 8;
	int len_8bit = ud_len;
	guint8 upper;

	if (dcs == 0)
		return len_7bit;

	upper = (dcs & 0xc0) >> 6;

	switch (upper) {
	case 0:
	case 1:
		if (dcs & 0x20) /* compressed */
			return len_8bit;

		switch ((dcs & 0x0c) >> 2) {
		case 0:
			return len_7bit;
		case 1:
			return len_8bit;
		case 2:
			return len_8bit;
		}

		return 0;
	case 2:
		return 0;
	case 3:
		switch ((dcs & 0x30) >> 4) {
		case 0:
		case 1:
			return len_7bit;
		case 2:
			return len_8bit;
		case 3:
			if (dcs & 0x4)
				return len_8bit;
			else
				return len_7bit;
		}

		break;
	default:
		break;
	};

	return 0;
}

static inline gboolean next_octet(const unsigned char *pdu, int len,
					int *offset, unsigned char *oct)
{
	if (len == *offset)
		return FALSE;

	*oct = pdu[*offset];

	*offset = *offset + 1;

	return TRUE;
}

static inline gboolean set_octet(unsigned char *pdu, int *offset,
					unsigned char oct)
{
	pdu[*offset] = oct;
	*offset = *offset + 1;

	return TRUE;
}

gboolean sms_encode_scts(const struct sms_scts *in, unsigned char *pdu,
				int *offset)
{
	guint timezone;

	if (in->year > 99)
		return FALSE;

	if (in->month > 12)
		return FALSE;

	if (in->day > 31)
		return FALSE;

	if (in->hour > 23)
		return FALSE;

	if (in->minute > 59)
		return FALSE;

	if (in->second > 59)
		return FALSE;

	if ((in->timezone > MAX_TIMEZONE || in->timezone < MIN_TIMEZONE) &&
			in->has_timezone == TRUE)
		return FALSE;

	pdu = pdu + *offset;

	pdu[0] = ((in->year / 10) & 0x0f) | (((in->year % 10) & 0x0f) << 4);
	pdu[1] = ((in->month / 10) & 0x0f) | (((in->month % 10) & 0x0f) << 4);
	pdu[2] = ((in->day / 10) & 0x0f) | (((in->day % 10) & 0x0f) << 4);
	pdu[3] = ((in->hour / 10) & 0x0f) | (((in->hour % 10) & 0x0f) << 4);
	pdu[4] = ((in->minute / 10) & 0x0f) | (((in->minute % 10) & 0x0f) << 4);
	pdu[5] = ((in->second / 10) & 0x0f) | (((in->second % 10) & 0x0f) << 4);

	if (in->has_timezone == FALSE) {
		pdu[6] = 0xff;
		goto out;
	}

	timezone = abs(in->timezone);

	pdu[6] = ((timezone / 10) & 0x07) | (((timezone % 10) & 0x0f) << 4);

	if (in->timezone < 0)
		pdu[6] |= 0x8;

out:
	*offset += 7;

	return TRUE;
}

guint8 sms_decode_semi_octet(guint8 in)
{
	return (in & 0x0f) * 10 + (in >> 4);
}

gboolean sms_decode_scts(const unsigned char *pdu, int len,
				int *offset, struct sms_scts *out)
{
	unsigned char oct = 0;

	if ((len - *offset) < 7)
		return FALSE;

	next_octet(pdu, len, offset, &oct);
	out->year = sms_decode_semi_octet(oct);

	if (out->year > 99)
		return FALSE;

	next_octet(pdu, len, offset, &oct);
	out->month = sms_decode_semi_octet(oct);

	if (out->month > 12)
		return FALSE;

	next_octet(pdu, len, offset, &oct);
	out->day = sms_decode_semi_octet(oct);

	if (out->day > 31)
		return FALSE;

	next_octet(pdu, len, offset, &oct);
	out->hour = sms_decode_semi_octet(oct);

	if (out->hour > 23)
		return FALSE;

	next_octet(pdu, len, offset, &oct);
	out->minute = sms_decode_semi_octet(oct);

	if (out->minute > 59)
		return FALSE;

	next_octet(pdu, len, offset, &oct);
	out->second = sms_decode_semi_octet(oct);

	if (out->second > 59)
		return FALSE;

	next_octet(pdu, len, offset, &oct);

	/*
	 * Time Zone indicates the difference, expressed in quarters
	 * of an hour, between the local time and GMT. In the first of the two
	 * semi‑octets, the first bit (bit 3 of the seventh octet of the
	 * TP‑Service‑Centre‑Time‑Stamp field) represents the algebraic
	 * sign of this difference (0: positive, 1: negative).
	 */
	out->timezone = (oct & 0x07) * 10 + ((oct & 0xf0) >> 4);

	if (oct & 0x08)
		out->timezone = out->timezone * -1;

	if ((out->timezone > MAX_TIMEZONE) || (out->timezone < MIN_TIMEZONE))
		return FALSE;

	out->has_timezone = TRUE;

	return TRUE;
}

static gboolean decode_validity_period(const unsigned char *pdu, int len,
					int *offset,
					enum sms_validity_period_format vpf,
					struct sms_validity_period *vp)
{
	switch (vpf) {
	case SMS_VALIDITY_PERIOD_FORMAT_ABSENT:
		return TRUE;
	case SMS_VALIDITY_PERIOD_FORMAT_RELATIVE:
		if (!next_octet(pdu, len, offset, &vp->relative))
			return FALSE;

		return TRUE;
	case SMS_VALIDITY_PERIOD_FORMAT_ABSOLUTE:
		if (!sms_decode_scts(pdu, len, offset, &vp->absolute))
			return FALSE;

		return TRUE;
	case SMS_VALIDITY_PERIOD_FORMAT_ENHANCED:
		/*
		 * TODO: Parse out enhanced structure properly
		 * 23.040 Section 9.2.3.12.3
		 */
		if ((len - *offset) < 7)
			return FALSE;

		memcpy(vp->enhanced, pdu + *offset, 7);

		*offset = *offset + 7;

		return TRUE;
	default:
		break;
	}

	return FALSE;
}

static gboolean encode_validity_period(const struct sms_validity_period *vp,
					enum sms_validity_period_format vpf,
					unsigned char *pdu, int *offset)
{
	switch (vpf) {
	case SMS_VALIDITY_PERIOD_FORMAT_ABSENT:
		return TRUE;
	case SMS_VALIDITY_PERIOD_FORMAT_RELATIVE:
		set_octet(pdu, offset, vp->relative);
		return TRUE;
	case SMS_VALIDITY_PERIOD_FORMAT_ABSOLUTE:
		return sms_encode_scts(&vp->absolute, pdu, offset);
	case SMS_VALIDITY_PERIOD_FORMAT_ENHANCED:
		/* TODO: Write out proper enhanced VP structure */
		memcpy(pdu + *offset, vp->enhanced, 7);

		*offset = *offset + 7;

		return TRUE;
	default:
		break;
	}

	return FALSE;
}

gboolean sms_encode_address_field(const struct sms_address *in, gboolean sc,
					unsigned char *pdu, int *offset)
{
	size_t len = strlen(in->address);
	unsigned char addr_len = 0;
	unsigned char p[10];

	pdu = pdu + *offset;

	if (len == 0 && sc) {
		pdu[0] = 0;
		*offset = *offset + 1;

		return TRUE;
	}

	if (len == 0)
		goto out;

	if (in->number_type == SMS_NUMBER_TYPE_ALPHANUMERIC) {
		long written;
		long packed;
		unsigned char *gsm;
		unsigned char *r;

		if (len > 11)
			return FALSE;

		gsm = convert_utf8_to_gsm(in->address, len, NULL, &written, 0);
		if (gsm == NULL)
			return FALSE;

		r = pack_7bit_own_buf(gsm, written, 0, FALSE, &packed, 0, p);

		g_free(gsm);

		if (r == NULL)
			return FALSE;

		if (sc)
			addr_len = packed + 1;
		else
			addr_len = (written * 7 + 3) / 4;
	} else {
		int j = 0;
		int i;
		int c;

		if (len > 20)
			return FALSE;

		for (i = 0; in->address[i]; i++) {
			c = to_semi_oct(in->address[i]);

			if (c < 0)
				return FALSE;

			if ((i % 2) == 0) {
				p[j] = c;
			} else {
				p[j] |= c << 4;
				j++;
			}
		}

		if ((i % 2) == 1) {
			p[j] |= 0xf0;
			j++;
		}

		if (sc)
			addr_len = j + 1;
		else
			addr_len = i;
	}

out:
	pdu[0] = addr_len;
	pdu[1] = (in->number_type << 4) | in->numbering_plan | 0x80;
	memcpy(pdu+2, p, (sc ? addr_len - 1 : (addr_len + 1) / 2));

	*offset = *offset + 2 + (sc ? addr_len - 1 : (addr_len + 1) / 2);

	return TRUE;
}

gboolean sms_decode_address_field(const unsigned char *pdu, int len,
					int *offset, gboolean sc,
					struct sms_address *out)
{
	unsigned char addr_len;
	unsigned char addr_type;
	int byte_len;

	if (!next_octet(pdu, len, offset, &addr_len))
		return FALSE;

	if (sc && addr_len == 0) {
		out->address[0] = '\0';
		return TRUE;
	}

	if (!next_octet(pdu, len, offset, &addr_type))
		return FALSE;

	if (sc)
		byte_len = addr_len - 1;
	else
		byte_len = (addr_len + 1) / 2;

	if ((len - *offset) < byte_len)
		return FALSE;

	out->number_type = bit_field(addr_type, 4, 3);
	out->numbering_plan = bit_field(addr_type, 0, 4);

	if (out->number_type != SMS_NUMBER_TYPE_ALPHANUMERIC) {
		extract_bcd_number(pdu + *offset, byte_len, out->address);
		*offset += byte_len;
	} else {
		int chars;
		long written;
		unsigned char *res;
		char *utf8;

		if (sc)
			chars = byte_len * 8 / 7;
		else
			chars = addr_len * 4 / 7;

		/*
		 * This cannot happen according to 24.011, however
		 * nothing is said in 23.040
		 */
		if (chars == 0) {
			out->address[0] = '\0';
			return TRUE;
		}

		res = unpack_7bit(pdu + *offset, byte_len, 0, FALSE, chars,
					&written, 0);

		*offset = *offset + (addr_len + 1) / 2;

		if (res == NULL)
			return FALSE;

		utf8 = convert_gsm_to_utf8(res, written, NULL, NULL, 0);

		g_free(res);

		if (utf8 == NULL)
			return FALSE;

		if (strlen(utf8) > 20) {
			g_free(utf8);
			return FALSE;
		}

		strcpy(out->address, utf8);

		g_free(utf8);
	}

	return TRUE;
}

static gboolean encode_deliver(const struct sms_deliver *in, unsigned char *pdu,
				int *offset)
{
	int ud_oct_len;
	unsigned char oct;

	oct = 0;

	if (!in->mms)
		oct |= 1 << 2;

	if (in->sri)
		oct |= 1 << 5;

	if (in->rp)
		oct |= 1 << 7;

	if (in->udhi)
		oct |= 1 << 6;

	set_octet(pdu, offset, oct);

	if (sms_encode_address_field(&in->oaddr, FALSE, pdu, offset) == FALSE)
		return FALSE;

	set_octet(pdu, offset, in->pid);
	set_octet(pdu, offset, in->dcs);

	if (sms_encode_scts(&in->scts, pdu, offset) == FALSE)
		return FALSE;

	set_octet(pdu, offset, in->udl);

	ud_oct_len = sms_udl_in_bytes(in->udl, in->dcs);

	memcpy(pdu + *offset, in->ud, ud_oct_len);

	*offset = *offset + ud_oct_len;

	return TRUE;
}

static gboolean decode_deliver(const unsigned char *pdu, int len,
				struct sms *out)
{
	int offset = 0;
	int expected;
	unsigned char octet;

	out->type = SMS_TYPE_DELIVER;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	out->deliver.mms = !is_bit_set(octet, 2);
	out->deliver.sri = is_bit_set(octet, 5);
	out->deliver.udhi = is_bit_set(octet, 6);
	out->deliver.rp = is_bit_set(octet, 7);

	if (!sms_decode_address_field(pdu, len, &offset,
					FALSE, &out->deliver.oaddr))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->deliver.pid))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->deliver.dcs))
		return FALSE;

	if (!sms_decode_scts(pdu, len, &offset, &out->deliver.scts))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->deliver.udl))
		return FALSE;

	expected = sms_udl_in_bytes(out->deliver.udl, out->deliver.dcs);

	if ((len - offset) < expected)
		return FALSE;

	memcpy(out->deliver.ud, pdu+offset, expected);

	return TRUE;
}

static gboolean encode_submit_ack_report(const struct sms_submit_ack_report *in,
						unsigned char *pdu, int *offset)
{
	unsigned char oct;

	oct = 1;

	if (in->udhi)
		oct |= 1 << 6;

	set_octet(pdu, offset, oct);

	set_octet(pdu, offset, in->pi);

	if (!sms_encode_scts(&in->scts, pdu, offset))
		return FALSE;

	if (in->pi & 0x1)
		set_octet(pdu, offset, in->pid);

	if (in->pi & 0x2)
		set_octet(pdu, offset, in->dcs);

	if (in->pi & 0x4) {
		int ud_oct_len = sms_udl_in_bytes(in->udl, in->dcs);

		set_octet(pdu, offset, in->udl);
		memcpy(pdu + *offset, in->ud, ud_oct_len);
		*offset = *offset + ud_oct_len;
	}

	return TRUE;
}

static gboolean encode_submit_err_report(const struct sms_submit_err_report *in,
						unsigned char *pdu, int *offset)
{
	unsigned char oct;

	oct = 0x1;

	if (in->udhi)
		oct |= 1 << 6;

	set_octet(pdu, offset, oct);

	set_octet(pdu, offset, in->fcs);

	set_octet(pdu, offset, in->pi);

	if (!sms_encode_scts(&in->scts, pdu, offset))
		return FALSE;

	if (in->pi & 0x1)
		set_octet(pdu, offset, in->pid);

	if (in->pi & 0x2)
		set_octet(pdu, offset, in->dcs);

	if (in->pi & 0x4) {
		int ud_oct_len = sms_udl_in_bytes(in->udl, in->dcs);

		set_octet(pdu, offset, in->udl);
		memcpy(pdu + *offset, in->ud, ud_oct_len);
		*offset = *offset + ud_oct_len;
	}

	return TRUE;
}

static gboolean decode_submit_report(const unsigned char *pdu, int len,
					struct sms *out)
{
	int offset = 0;
	unsigned char octet;
	gboolean udhi;
	guint8 uninitialized_var(fcs);
	guint8 pi;
	struct sms_scts *scts;
	guint8 pid = 0;
	guint8 dcs = 0;
	guint8 udl = 0;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	udhi = is_bit_set(octet, 6);

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	/*
	 * At this point we don't know whether this is an ACK or an ERROR.
	 * FCS can only have values 0x80 and above, as 0x00 - 0x7F are reserved
	 * according to 3GPP 23.040.  For PI, the values can be only in
	 * bit 0, 1, 2 with the 7th bit reserved as an extension.  Since
	 * bits 3-6 are not used, assume no extension is feasible, so if the
	 * value of this octet is >= 0x80, this is an FCS and thus an error
	 * report tpdu.
	 */

	if (octet >= 0x80) {
		out->type = SMS_TYPE_SUBMIT_REPORT_ERROR;
		fcs = octet;

		if (!next_octet(pdu, len, &offset, &octet))
			return FALSE;

		scts = &out->submit_err_report.scts;
	} else {
		scts = &out->submit_ack_report.scts;
		out->type = SMS_TYPE_SUBMIT_REPORT_ACK;
	}

	pi = octet & 0x07;

	if (!sms_decode_scts(pdu, len, &offset, scts))
		return FALSE;

	if (pi & 0x01) {
		if (!next_octet(pdu, len, &offset, &pid))
			return FALSE;
	}

	if (pi & 0x02) {
		if (!next_octet(pdu, len, &offset, &dcs))
			return FALSE;
	}

	if (out->type == SMS_TYPE_SUBMIT_REPORT_ERROR) {
		out->submit_err_report.udhi = udhi;
		out->submit_err_report.fcs = fcs;
		out->submit_err_report.pi = pi;
		out->submit_err_report.pid = pid;
		out->submit_err_report.dcs = dcs;
	} else {
		out->submit_ack_report.udhi = udhi;
		out->submit_ack_report.pi = pi;
		out->submit_ack_report.pid = pid;
		out->submit_ack_report.dcs = dcs;
	}

	if (pi & 0x04) {
		int expected;

		if (!next_octet(pdu, len, &offset, &udl))
			return FALSE;

		expected = sms_udl_in_bytes(udl, dcs);

		if ((len - offset) < expected)
			return FALSE;

		if (out->type == SMS_TYPE_SUBMIT_REPORT_ERROR) {
			out->submit_err_report.udl = udl;
			memcpy(out->submit_err_report.ud,
					pdu+offset, expected);
		} else {
			out->submit_ack_report.udl = udl;
			memcpy(out->submit_ack_report.ud,
					pdu+offset, expected);
		}
	}

	return TRUE;
}

static gboolean encode_status_report(const struct sms_status_report *in,
					unsigned char *pdu, int *offset)
{
	unsigned char octet;

	octet = 0x2;

	if (!in->mms)
		octet |= 1 << 2;

	if (!in->srq)
		octet |= 1 << 5;

	if (!in->udhi)
		octet |= 1 << 6;

	set_octet(pdu, offset, octet);

	set_octet(pdu, offset, in->mr);

	if (!sms_encode_address_field(&in->raddr, FALSE, pdu, offset))
		return FALSE;

	if (!sms_encode_scts(&in->scts, pdu, offset))
		return FALSE;

	if (!sms_encode_scts(&in->dt, pdu, offset))
		return FALSE;

	octet = in->st;
	set_octet(pdu, offset, octet);

	if (in->pi == 0)
		return TRUE;

	set_octet(pdu, offset, in->pi);

	if (in->pi & 0x01)
		set_octet(pdu, offset, in->pid);

	if (in->pi & 0x02)
		set_octet(pdu, offset, in->dcs);

	if (in->pi & 0x4) {
		int ud_oct_len = sms_udl_in_bytes(in->udl, in->dcs);

		set_octet(pdu, offset, in->udl);
		memcpy(pdu + *offset, in->ud, ud_oct_len);
		*offset = *offset + ud_oct_len;
	}

	return TRUE;
}

static gboolean decode_status_report(const unsigned char *pdu, int len,
					struct sms *out)
{
	int offset = 0;
	unsigned char octet;

	out->type = SMS_TYPE_STATUS_REPORT;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	out->status_report.mms = !is_bit_set(octet, 2);
	out->status_report.srq = is_bit_set(octet, 5);
	out->status_report.udhi = is_bit_set(octet, 6);

	if (!next_octet(pdu, len, &offset, &out->status_report.mr))
		return FALSE;

	if (!sms_decode_address_field(pdu, len, &offset, FALSE,
					&out->status_report.raddr))
		return FALSE;

	if (!sms_decode_scts(pdu, len, &offset, &out->status_report.scts))
		return FALSE;

	if (!sms_decode_scts(pdu, len, &offset, &out->status_report.dt))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	out->status_report.st = octet;

	/*
	 * We have to be careful here, PI is labeled as Optional in 23.040
	 * which is different from RP-ERR & RP-ACK for both Deliver & Submit
	 * reports
	 */

	if ((len - offset) == 0)
		return TRUE;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	out->status_report.pi = octet & 0x07;

	if (out->status_report.pi & 0x01) {
		if (!next_octet(pdu, len, &offset, &out->status_report.pid))
			return FALSE;
	}

	if (out->status_report.pi & 0x02) {
		if (!next_octet(pdu, len, &offset, &out->status_report.dcs))
			return FALSE;
	}

	if (out->status_report.pi & 0x04) {
		int expected;

		if (!next_octet(pdu, len, &offset, &out->status_report.udl))
			return FALSE;

		expected = sms_udl_in_bytes(out->status_report.udl,
						out->status_report.dcs);

		if ((len - offset) < expected)
			return FALSE;

		memcpy(out->status_report.ud, pdu+offset, expected);
	}

	return TRUE;
}

static gboolean encode_deliver_ack_report(const struct sms_deliver_ack_report *in,
						unsigned char *pdu,
						int *offset)
{
	unsigned char oct;

	oct = 0;

	if (in->udhi)
		oct |= 1 << 6;

	set_octet(pdu, offset, oct);

	set_octet(pdu, offset, in->pi);

	if (in->pi & 0x1)
		set_octet(pdu, offset, in->pid);

	if (in->pi & 0x2)
		set_octet(pdu, offset, in->dcs);

	if (in->pi & 0x4) {
		int ud_oct_len = sms_udl_in_bytes(in->udl, in->dcs);

		set_octet(pdu, offset, in->udl);
		memcpy(pdu + *offset, in->ud, ud_oct_len);
		*offset = *offset + ud_oct_len;
	}

	return TRUE;
}

static gboolean encode_deliver_err_report(const struct sms_deliver_err_report *in,
						unsigned char *pdu,
						int *offset)
{
	unsigned char oct;

	oct = 0;

	if (in->udhi)
		oct |= 1 << 6;

	set_octet(pdu, offset, oct);

	set_octet(pdu, offset, in->fcs);

	set_octet(pdu, offset, in->pi);

	if (in->pi & 0x1)
		set_octet(pdu, offset, in->pid);

	if (in->pi & 0x2)
		set_octet(pdu, offset, in->dcs);

	if (in->pi & 0x4) {
		int ud_oct_len = sms_udl_in_bytes(in->udl, in->dcs);

		set_octet(pdu, offset, in->udl);
		memcpy(pdu + *offset, in->ud, ud_oct_len);
		*offset = *offset + ud_oct_len;
	}

	return TRUE;
}

static gboolean decode_deliver_report(const unsigned char *pdu, int len,
					struct sms *out)
{
	int offset = 0;
	unsigned char octet;
	gboolean udhi;
	guint8 uninitialized_var(fcs);
	guint8 pi;
	guint8 pid = 0;
	guint8 dcs = 0;
	guint8 udl = 0;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	udhi = is_bit_set(octet, 6);

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	/*
	 * At this point we don't know whether this is an ACK or an ERROR.
	 * FCS can only have values 0x80 and above, as 0x00 - 0x7F are reserved
	 * according to 3GPP 23.040.  For PI, the values can be only in
	 * bit 0, 1, 2 with the 7th bit reserved as an extension.  Since
	 * bits 3-6 are not used, assume no extension is feasible, so if the
	 * value of this octet is >= 0x80, this is an FCS and thus an error
	 * report tpdu.
	 */

	if (octet >= 0x80) {
		out->type = SMS_TYPE_DELIVER_REPORT_ERROR;
		fcs = octet;

		if (!next_octet(pdu, len, &offset, &octet))
			return FALSE;
	} else {
		out->type = SMS_TYPE_DELIVER_REPORT_ACK;
	}

	pi = octet & 0x07;

	if (pi & 0x01) {
		if (!next_octet(pdu, len, &offset, &pid))
			return FALSE;
	}

	if (pi & 0x02) {
		if (!next_octet(pdu, len, &offset, &dcs))
			return FALSE;
	}

	if (out->type == SMS_TYPE_DELIVER_REPORT_ERROR) {
		out->deliver_err_report.udhi = udhi;
		out->deliver_err_report.fcs = fcs;
		out->deliver_err_report.pi = pi;
		out->deliver_err_report.pid = pid;
		out->deliver_err_report.dcs = dcs;
	} else {
		out->deliver_ack_report.udhi = udhi;
		out->deliver_ack_report.pi = pi;
		out->deliver_ack_report.pid = pid;
		out->deliver_ack_report.dcs = dcs;
	}

	if (pi & 0x04) {
		int expected;

		if (!next_octet(pdu, len, &offset, &udl))
			return FALSE;

		expected = sms_udl_in_bytes(udl, dcs);

		if ((len - offset) < expected)
			return FALSE;

		if (out->type == SMS_TYPE_DELIVER_REPORT_ERROR) {
			out->deliver_err_report.udl = udl;
			memcpy(out->deliver_err_report.ud,
					pdu+offset, expected);
		} else {
			out->deliver_ack_report.udl = udl;
			memcpy(out->deliver_ack_report.ud,
					pdu+offset, expected);
		}
	}

	return TRUE;
}

static gboolean encode_submit(const struct sms_submit *in,
					unsigned char *pdu, int *offset)
{
	unsigned char octet;
	int ud_oct_len;

	/* SMS Submit */
	octet = 0x1;

	if (in->rd)
		octet |= 1 << 2;

	if (in->rp)
		octet |= 1 << 7;

	octet |= in->vpf << 3;

	if (in->udhi)
		octet |= 1 << 6;

	if (in->srr)
		octet |= 1 << 5;

	set_octet(pdu, offset, octet);

	set_octet(pdu, offset, in->mr);

	if (sms_encode_address_field(&in->daddr, FALSE, pdu, offset) == FALSE)
		return FALSE;

	set_octet(pdu, offset, in->pid);

	set_octet(pdu, offset, in->dcs);

	if (!encode_validity_period(&in->vp, in->vpf, pdu, offset))
		return FALSE;

	set_octet(pdu, offset, in->udl);

	ud_oct_len = sms_udl_in_bytes(in->udl, in->dcs);

	memcpy(pdu + *offset, in->ud, ud_oct_len);

	*offset = *offset + ud_oct_len;

	return TRUE;
}

gboolean sms_decode_unpacked_stk_pdu(const unsigned char *pdu, int len,
					struct sms *out)
{
	unsigned char octet;
	int offset = 0;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	if ((octet & 0x3) != 1)
		return FALSE;

	out->type = SMS_TYPE_SUBMIT;

	out->submit.rd = is_bit_set(octet, 2);
	out->submit.vpf = bit_field(octet, 3, 2);
	out->submit.rp = is_bit_set(octet, 7);
	out->submit.udhi = is_bit_set(octet, 6);
	out->submit.srr = is_bit_set(octet, 5);

	if (!next_octet(pdu, len, &offset, &out->submit.mr))
		return FALSE;

	if (!sms_decode_address_field(pdu, len, &offset,
					FALSE, &out->submit.daddr))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->submit.pid))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->submit.dcs))
		return FALSE;

	/* Now we override the DCS */
	out->submit.dcs = 0xF0;

	if (!decode_validity_period(pdu, len, &offset, out->submit.vpf,
					&out->submit.vp))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->submit.udl))
		return FALSE;

	if ((len - offset) < out->submit.udl)
		return FALSE;

	pack_7bit_own_buf(pdu + offset, out->submit.udl, 0, FALSE,
				NULL, 0, out->submit.ud);

	return TRUE;
}

static gboolean decode_submit(const unsigned char *pdu, int len,
					struct sms *out)
{
	unsigned char octet;
	int offset = 0;
	int expected;

	out->type = SMS_TYPE_SUBMIT;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	out->submit.rd = is_bit_set(octet, 2);
	out->submit.vpf = bit_field(octet, 3, 2);
	out->submit.rp = is_bit_set(octet, 7);
	out->submit.udhi = is_bit_set(octet, 6);
	out->submit.srr = is_bit_set(octet, 5);

	if (!next_octet(pdu, len, &offset, &out->submit.mr))
		return FALSE;

	if (!sms_decode_address_field(pdu, len, &offset,
					FALSE, &out->submit.daddr))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->submit.pid))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->submit.dcs))
		return FALSE;

	if (!decode_validity_period(pdu, len, &offset, out->submit.vpf,
					&out->submit.vp))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->submit.udl))
		return FALSE;

	expected = sms_udl_in_bytes(out->submit.udl, out->submit.dcs);

	if ((len - offset) < expected)
		return FALSE;

	if (expected > (int) sizeof(out->submit.ud))
		return FALSE;

	memcpy(out->submit.ud, pdu+offset, expected);

	return TRUE;
}

static gboolean encode_command(const struct sms_command *in,
					unsigned char *pdu, int *offset)
{
	unsigned char octet;

	octet = 0x2;

	if (in->udhi)
		octet |= 1 << 6;

	if (in->srr)
		octet |= 1 << 5;

	set_octet(pdu, offset, octet);

	set_octet(pdu, offset, in->mr);

	set_octet(pdu, offset, in->pid);

	octet = in->ct;
	set_octet(pdu, offset, octet);

	set_octet(pdu, offset, in->mn);

	if (!sms_encode_address_field(&in->daddr, FALSE, pdu, offset))
		return FALSE;

	set_octet(pdu, offset, in->cdl);

	memcpy(pdu + *offset, in->cd, in->cdl);

	*offset = *offset + in->cdl;

	return TRUE;
}

static gboolean decode_command(const unsigned char *pdu, int len,
					struct sms *out)
{
	unsigned char octet;
	int offset = 0;

	out->type = SMS_TYPE_COMMAND;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	out->command.udhi = is_bit_set(octet, 6);
	out->command.srr = is_bit_set(octet, 5);

	if (!next_octet(pdu, len, &offset, &out->command.mr))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->command.pid))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &octet))
		return FALSE;

	out->command.ct = octet;

	if (!next_octet(pdu, len, &offset, &out->command.mn))
		return FALSE;

	if (!sms_decode_address_field(pdu, len, &offset,
					FALSE, &out->command.daddr))
		return FALSE;

	if (!next_octet(pdu, len, &offset, &out->command.cdl))
		return FALSE;

	if ((len - offset) < out->command.cdl)
		return FALSE;

	memcpy(out->command.cd, pdu+offset, out->command.cdl);

	return TRUE;
}

/* Buffer must be at least 164 (tpud) + 12 (SC address) bytes long */
gboolean sms_encode(const struct sms *in, int *len, int *tpdu_len,
			unsigned char *pdu)
{
	int offset = 0;
	int tpdu_start;

	if (in->type == SMS_TYPE_DELIVER || in->type == SMS_TYPE_SUBMIT ||
			in->type == SMS_TYPE_COMMAND ||
			in->type == SMS_TYPE_STATUS_REPORT)
		if (!sms_encode_address_field(&in->sc_addr, TRUE, pdu, &offset))
			return FALSE;

	tpdu_start = offset;

	switch (in->type) {
	case SMS_TYPE_DELIVER:
		if (encode_deliver(&in->deliver, pdu, &offset) == FALSE)
			return FALSE;
		break;
	case SMS_TYPE_DELIVER_REPORT_ACK:
		if (!encode_deliver_ack_report(&in->deliver_ack_report, pdu,
						&offset))
			return FALSE;
		break;
	case SMS_TYPE_DELIVER_REPORT_ERROR:
		if (!encode_deliver_err_report(&in->deliver_err_report, pdu,
						&offset))
			return FALSE;
		break;
	case SMS_TYPE_STATUS_REPORT:
		if (!encode_status_report(&in->status_report, pdu, &offset))
			return FALSE;
		break;
	case SMS_TYPE_SUBMIT:
		if (!encode_submit(&in->submit, pdu, &offset))
			return FALSE;
		break;
	case SMS_TYPE_SUBMIT_REPORT_ACK:
		if (!encode_submit_ack_report(&in->submit_ack_report, pdu,
						&offset))
			return FALSE;
		break;
	case SMS_TYPE_SUBMIT_REPORT_ERROR:
		if (!encode_submit_err_report(&in->submit_err_report, pdu,
						&offset))
			return FALSE;
		break;
	case SMS_TYPE_COMMAND:
		if (!encode_command(&in->command, pdu, &offset))
			return FALSE;
		break;
	default:
		return FALSE;
	};

	if (tpdu_len)
		*tpdu_len = offset - tpdu_start;

	if (len)
		*len = offset;

	return TRUE;
}

gboolean sms_decode(const unsigned char *pdu, int len, gboolean outgoing,
			int tpdu_len, struct sms *out)
{
	unsigned char type;
	int offset = 0;

	if (out == NULL)
		return FALSE;

	if (len == 0)
		return FALSE;

	memset(out, 0, sizeof(*out));

	if (tpdu_len < len) {
		if (!sms_decode_address_field(pdu, len, &offset,
						TRUE, &out->sc_addr))
			return FALSE;
	}

	if ((len - offset) < tpdu_len)
		return FALSE;

	/* 23.040 9.2.3.1 */
	type = pdu[offset] & 0x3;

	if (outgoing)
		type |= 0x4;

	pdu = pdu + offset;

	switch (type) {
	case 0:
		return decode_deliver(pdu, tpdu_len, out);
	case 1:
		return decode_submit_report(pdu, tpdu_len, out);
	case 2:
		return decode_status_report(pdu, tpdu_len, out);
	case 3:
		/* According to 9.2.3.1, Reserved treated as deliver */
		return decode_deliver(pdu, tpdu_len, out);
	case 4:
		return decode_deliver_report(pdu, tpdu_len, out);
	case 5:
		return decode_submit(pdu, tpdu_len, out);
	case 6:
		return decode_command(pdu, tpdu_len, out);
	}

	return FALSE;
}

const guint8 *sms_extract_common(const struct sms *sms, gboolean *out_udhi,
					guint8 *out_dcs, guint8 *out_udl,
					guint8 *out_max)
{
	const guint8 *ud = NULL;
	guint8 uninitialized_var(udl);
	guint8 uninitialized_var(max);
	gboolean uninitialized_var(udhi);
	guint8 uninitialized_var(dcs);

	switch (sms->type) {
	case SMS_TYPE_DELIVER:
		udhi = sms->deliver.udhi;
		ud = sms->deliver.ud;
		udl = sms->deliver.udl;
		dcs = sms->deliver.dcs;
		max = sizeof(sms->deliver.ud);
		break;
	case SMS_TYPE_DELIVER_REPORT_ACK:
		udhi = sms->deliver_ack_report.udhi;
		ud = sms->deliver_ack_report.ud;
		udl = sms->deliver_ack_report.udl;
		dcs = sms->deliver_ack_report.dcs;
		max = sizeof(sms->deliver_ack_report.ud);
		break;
	case SMS_TYPE_DELIVER_REPORT_ERROR:
		udhi = sms->deliver_err_report.udhi;
		ud = sms->deliver_err_report.ud;
		udl = sms->deliver_err_report.udl;
		dcs = sms->deliver_err_report.dcs;
		max = sizeof(sms->deliver_err_report.ud);
		break;
	case SMS_TYPE_STATUS_REPORT:
		udhi = sms->status_report.udhi;
		ud = sms->status_report.ud;
		udl = sms->status_report.udl;
		dcs = sms->status_report.dcs;
		max = sizeof(sms->status_report.ud);
		break;
	case SMS_TYPE_SUBMIT:
		udhi = sms->submit.udhi;
		ud = sms->submit.ud;
		udl = sms->submit.udl;
		dcs = sms->submit.dcs;
		max = sizeof(sms->submit.ud);
		break;
	case SMS_TYPE_SUBMIT_REPORT_ACK:
		udhi = sms->submit_ack_report.udhi;
		ud = sms->submit_ack_report.ud;
		udl = sms->submit_ack_report.udl;
		dcs = sms->submit_ack_report.dcs;
		max = sizeof(sms->submit_ack_report.ud);
		break;
	case SMS_TYPE_SUBMIT_REPORT_ERROR:
		udhi = sms->submit_err_report.udhi;
		ud = sms->submit_err_report.ud;
		udl = sms->submit_err_report.udl;
		dcs = sms->submit_err_report.dcs;
		max = sizeof(sms->submit_err_report.ud);
		break;
	case SMS_TYPE_COMMAND:
		udhi = sms->command.udhi;
		ud = sms->command.cd;
		udl = sms->command.cdl;
		dcs = 0;
		max = sizeof(sms->command.cd);
		break;
	};

	if (ud == NULL)
		return NULL;

	if (out_udhi)
		*out_udhi = udhi;

	if (out_dcs)
		*out_dcs = dcs;

	if (out_udl)
		*out_udl = udl;

	if (out_max)
		*out_max = max;

	return ud;
}

static gboolean verify_udh(const guint8 *hdr, guint8 max_len)
{
	guint8 max_offset;
	guint8 offset;

	/* Must have at least one information-element if udhi is true */
	if (hdr[0] < 2)
		return FALSE;

	if (hdr[0] >= max_len)
		return FALSE;

	/*
	 * According to 23.040: If the length of the User Data Header is
	 * such that there are too few or too many octets in the final
	 * Information Element then the whole User Data Header shall be
	 * ignored.
	 */

	max_offset = hdr[0] + 1;
	offset = 1;
	do {
		if ((offset + 2) > max_offset)
			return FALSE;

		if ((offset + 2 + hdr[offset + 1]) > max_offset)
			return FALSE;

		offset = offset + 2 + hdr[offset + 1];
	} while (offset < max_offset);

	if (offset != max_offset)
		return FALSE;

	return TRUE;
}

gboolean sms_udh_iter_init(const struct sms *sms, struct sms_udh_iter *iter)
{
	gboolean udhi = FALSE;
	const guint8 *hdr;
	guint8 udl;
	guint8 dcs;
	guint8 max_len;
	guint8 max_ud_len;

	hdr = sms_extract_common(sms, &udhi, &dcs, &udl, &max_ud_len);
	if (hdr == NULL)
		return FALSE;

	if (!udhi)
		return FALSE;

	if (sms->type == SMS_TYPE_COMMAND)
		max_len = udl;
	else
		max_len = sms_udl_in_bytes(udl, dcs);

	/* Can't actually store the HDL + IEI / IEL */
	if (max_len < 3)
		return FALSE;

	if (max_len > max_ud_len)
		return FALSE;

	if (!verify_udh(hdr, max_len))
		return FALSE;

	iter->data = hdr;
	iter->offset = 1;

	return TRUE;
}

gboolean sms_udh_iter_init_from_cbs(const struct cbs *cbs,
					struct sms_udh_iter *iter)
{
	gboolean udhi = FALSE;
	const guint8 *hdr;
	guint8 max_ud_len;

	cbs_dcs_decode(cbs->dcs, &udhi, NULL, NULL, NULL, NULL, NULL);

	if (!udhi)
		return FALSE;

	hdr = cbs->ud;
	max_ud_len = 82;

	/* Must have at least one information-element if udhi is true */
	if (hdr[0] < 2)
		return FALSE;

	if (hdr[0] >= max_ud_len)
		return FALSE;

	if (!verify_udh(hdr, max_ud_len))
		return FALSE;

	iter->data = hdr;
	iter->offset = 1;

	return TRUE;
}
guint8 sms_udh_iter_get_udh_length(struct sms_udh_iter *iter)
{
	return iter->data[0];
}

const guint8 *sms_udh_iter_get_ud_after_header(struct sms_udh_iter *iter)
{
	return iter->data + iter->data[0] + 1;
}

enum sms_iei sms_udh_iter_get_ie_type(struct sms_udh_iter *iter)
{
	if (iter->offset > iter->data[0])
		return SMS_IEI_INVALID;

	return (enum sms_iei) iter->data[iter->offset];
}

guint8 sms_udh_iter_get_ie_length(struct sms_udh_iter *iter)
{
	guint8 ie_len;

	ie_len = iter->data[iter->offset + 1];

	return ie_len;
}

void sms_udh_iter_get_ie_data(struct sms_udh_iter *iter, guint8 *data)
{
	guint8 ie_len;

	ie_len = iter->data[iter->offset + 1];

	memcpy(data, &iter->data[iter->offset + 2], ie_len);
}

gboolean sms_udh_iter_has_next(struct sms_udh_iter *iter)
{
	guint8 total_len = iter->data[0];
	guint8 cur_ie_len = iter->data[iter->offset + 1];

	if ((iter->offset + 2 + cur_ie_len) > total_len)
		return FALSE;

	return TRUE;
}

gboolean sms_udh_iter_next(struct sms_udh_iter *iter)
{
	if (iter->offset > iter->data[0])
		return FALSE;

	iter->offset = iter->offset + 2 + iter->data[iter->offset + 1];

	if (iter->offset > iter->data[0])
		return FALSE;

	return TRUE;
}

/*
 * Returns both forms of time.  The time_t value returns the time in local
 * timezone.  The struct tm is filled out with the remote time information
 */
time_t sms_scts_to_time(const struct sms_scts *scts, struct tm *remote)
{
	struct tm t;
	time_t ret;

	t.tm_sec = scts->second;
	t.tm_min = scts->minute;
	t.tm_hour = scts->hour;
	t.tm_mday = scts->day;
	t.tm_mon = scts->month - 1;
	t.tm_isdst = -1;

	if (scts->year > 80)
		t.tm_year = scts->year;
	else
		t.tm_year = scts->year + 100;

	ret = mktime(&t);

	/* Adjust local time by the local timezone information */
	ret += t.tm_gmtoff;

	/* Set the proper timezone on the remote side */
	t.tm_gmtoff = scts->timezone * 15 * 60;

	/* Now adjust by the remote timezone information */
	ret -= t.tm_gmtoff;

	if (remote)
		memcpy(remote, &t, sizeof(struct tm));

	return ret;
}

void sms_address_from_string(struct sms_address *addr, const char *str)
{
	addr->numbering_plan = SMS_NUMBERING_PLAN_ISDN;
	if (str[0] == '+') {
		addr->number_type = SMS_NUMBER_TYPE_INTERNATIONAL;
		strcpy(addr->address, str+1);
	} else {
		addr->number_type = SMS_NUMBER_TYPE_UNKNOWN;
		strcpy(addr->address, str);
	}
}

const char *sms_address_to_string(const struct sms_address *addr)
{
	static char buffer[64];

	if (addr->number_type == SMS_NUMBER_TYPE_INTERNATIONAL &&
			(strlen(addr->address) > 0) &&
				addr->address[0] != '+') {
		buffer[0] = '+';
		strcpy(buffer + 1, addr->address);
	} else {
		strcpy(buffer, addr->address);
	}

	return buffer;
}

static gboolean extract_app_port_common(struct sms_udh_iter *iter, int *dst,
					int *src, gboolean *is_8bit)
{
	enum sms_iei iei;
	guint8 addr_hdr[4];
	int srcport = -1;
	int dstport = -1;
	gboolean uninitialized_var(is_addr_8bit);

	/*
	 * According to the specification, we have to use the last
	 * useable header.  Also, we have to ignore ports that are reserved:
	 * A receiving entity shall ignore (i.e. skip over and commence
	 * processing at the next information element) any information element
	 * where the value of the Information-Element-Data is Reserved or not
	 * supported.
	 */
	while ((iei = sms_udh_iter_get_ie_type(iter)) !=
			SMS_IEI_INVALID) {
		switch (iei) {
		case SMS_IEI_APPLICATION_ADDRESS_8BIT:
			if (sms_udh_iter_get_ie_length(iter) != 2)
				break;

			sms_udh_iter_get_ie_data(iter, addr_hdr);

			if (addr_hdr[0] < 240)
				break;

			if (addr_hdr[1] < 240)
				break;

			dstport = addr_hdr[0];
			srcport = addr_hdr[1];
			is_addr_8bit = TRUE;
			break;

		case SMS_IEI_APPLICATION_ADDRESS_16BIT:
			if (sms_udh_iter_get_ie_length(iter) != 4)
				break;

			sms_udh_iter_get_ie_data(iter, addr_hdr);

			if (((addr_hdr[0] << 8) | addr_hdr[1]) > 49151)
				break;

			if (((addr_hdr[2] << 8) | addr_hdr[3]) > 49151)
				break;

			dstport = (addr_hdr[0] << 8) | addr_hdr[1];
			srcport = (addr_hdr[2] << 8) | addr_hdr[3];
			is_addr_8bit = FALSE;
			break;

		default:
			break;
		}

		sms_udh_iter_next(iter);
	}

	if (dstport == -1 || srcport == -1)
		return FALSE;

	if (dst)
		*dst = dstport;

	if (src)
		*src = srcport;

	if (is_8bit)
		*is_8bit = is_addr_8bit;

	return TRUE;

}

gboolean sms_extract_app_port(const struct sms *sms, int *dst, int *src,
				gboolean *is_8bit)
{
	struct sms_udh_iter iter;

	if (!sms_udh_iter_init(sms, &iter))
		return FALSE;

	return extract_app_port_common(&iter, dst, src, is_8bit);
}

gboolean sms_extract_concatenation(const struct sms *sms, guint16 *ref_num,
					guint8 *max_msgs, guint8 *seq_num)
{
	struct sms_udh_iter iter;
	enum sms_iei iei;
	guint8 concat_hdr[4];
	guint16 uninitialized_var(rn);
	guint8 uninitialized_var(max), uninitialized_var(seq);
	gboolean concatenated = FALSE;

	/*
	 * We must ignore the entire user_data header here:
	 * If the length of the User Data Header is such that there
	 * are too few or too many octets in the final Information
	 * Element then the whole User Data Header shall be ignored.
	 */
	if (!sms_udh_iter_init(sms, &iter))
		return FALSE;

	/*
	 * According to the specification, we have to use the last
	 * useable header:
	 * In the event that IEs determined as not repeatable are
	 * duplicated, the last occurrence of the IE shall be used.
	 * In the event that two or more IEs occur which have mutually
	 * exclusive meanings (e.g. an 8bit port address and a 16bit
	 * port address), then the last occurring IE shall be used.
	 */
	while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
			SMS_IEI_INVALID) {
		switch (iei) {
		case SMS_IEI_CONCATENATED_8BIT:
			if (sms_udh_iter_get_ie_length(&iter) != 3)
				break;

			sms_udh_iter_get_ie_data(&iter, concat_hdr);

			if (concat_hdr[1] == 0)
				break;

			if (concat_hdr[2] == 0 || concat_hdr[2] > concat_hdr[1])
				break;

			rn = concat_hdr[0];
			max = concat_hdr[1];
			seq = concat_hdr[2];
			concatenated = TRUE;
			break;

		case SMS_IEI_CONCATENATED_16BIT:
			if (sms_udh_iter_get_ie_length(&iter) != 4)
				break;

			sms_udh_iter_get_ie_data(&iter, concat_hdr);

			if (concat_hdr[2] == 0)
				break;

			if (concat_hdr[3] == 0 ||
					concat_hdr[3] > concat_hdr[2])
				break;

			rn = (concat_hdr[0] << 8) | concat_hdr[1];
			max = concat_hdr[2];
			seq = concat_hdr[3];
			concatenated = TRUE;
			break;
		default:
			break;
		}

		sms_udh_iter_next(&iter);
	}

	if (!concatenated)
		return FALSE;

	if (ref_num)
		*ref_num = rn;

	if (max_msgs)
		*max_msgs = max;

	if (seq_num)
		*seq_num = seq;

	return TRUE;
}

gboolean sms_extract_language_variant(const struct sms *sms, guint8 *locking,
					guint8 *single)
{
	struct sms_udh_iter iter;
	enum sms_iei iei;
	guint8 variant;

	/*
	 * We must ignore the entire user_data header here:
	 * If the length of the User Data Header is such that there
	 * are too few or too many octets in the final Information
	 * Element then the whole User Data Header shall be ignored.
	 */
	if (!sms_udh_iter_init(sms, &iter))
		return FALSE;

	/*
	 * According to the specification, we have to use the last
	 * useable header:
	 * In the event that IEs determined as not repeatable are
	 * duplicated, the last occurrence of the IE shall be used.
	 * In the event that two or more IEs occur which have mutually
	 * exclusive meanings (e.g. an 8bit port address and a 16bit
	 * port address), then the last occurring IE shall be used.
	 */
	while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
			SMS_IEI_INVALID) {
		switch (iei) {
		case SMS_IEI_NATIONAL_LANGUAGE_SINGLE_SHIFT:
			if (sms_udh_iter_get_ie_length(&iter) != 1)
				break;

			sms_udh_iter_get_ie_data(&iter, &variant);
			if (single)
				*single = variant;
			break;

		case SMS_IEI_NATIONAL_LANGUAGE_LOCKING_SHIFT:
			if (sms_udh_iter_get_ie_length(&iter) != 1)
				break;

			sms_udh_iter_get_ie_data(&iter, &variant);
			if (locking)
				*locking = variant;
			break;
		default:
			break;
		}

		sms_udh_iter_next(&iter);
	}

	return TRUE;
}

/*!
 * Decodes a list of SMSes that contain a datagram.  The list must be
 * sorted in order of the sequence number.  This function assumes that
 * all fragments are coded using 8-bit character set.
 *
 * Returns a pointer to a newly allocated array or NULL if the
 * conversion could not be performed
 */
unsigned char *sms_decode_datagram(GSList *sms_list, long *out_len)
{
	GSList *l;
	const struct sms *sms;
	unsigned char *buf;
	long len = 0;

	for (l = sms_list; l; l = l->next) {
		guint8 taken = 0;
		guint8 udl;
		const guint8 *ud;
		struct sms_udh_iter iter;

		sms = l->data;

		ud = sms_extract_common(sms, NULL, NULL, &udl, NULL);
		if (ud == NULL)
			return NULL;

		/*
		 * Note we do this because we must check whether the UDH
		 * is properly formatted.  If not, the entire UDH is ignored
		 */
		if (sms_udh_iter_init(sms, &iter))
			taken = sms_udh_iter_get_udh_length(&iter) + 1;

		len += udl - taken;
	}

	/* Data is probably in headers we can't understand */
	if (len == 0)
		return NULL;

	buf = g_try_new(unsigned char, len);
	if (buf == NULL)
		return NULL;

	len = 0;
	for (l = sms_list; l; l = l->next) {
		guint8 taken = 0;
		guint8 udl;
		const guint8 *ud;
		struct sms_udh_iter iter;

		sms = l->data;

		ud = sms_extract_common(sms, NULL, NULL, &udl, NULL);

		if (sms_udh_iter_init(sms, &iter))
			taken = sms_udh_iter_get_udh_length(&iter) + 1;

		memcpy(buf + len, ud + taken, udl - taken);
		len += udl - taken;
	}

	if (out_len)
		*out_len = len;

	return buf;
}

static inline int sms_text_capacity_gsm(int max, int offset)
{
	return max - (offset * 8 + 6) / 7;
}

/*!
 * Decodes a list of SMSes that contain a text in either 7bit or UCS2 encoding.
 * The list must be sorted in order of the sequence number.  This function
 * assumes that all fragments have a proper DCS.
 *
 * Returns a pointer to a newly allocated string or NULL if the conversion
 * failed.
 */
char *sms_decode_text(GSList *sms_list)
{
	GSList *l;
	GString *str;
	const struct sms *sms;
	int guess_size = g_slist_length(sms_list);
	char *utf8;

	if (guess_size == 1)
		guess_size = 160;
	else
		guess_size = (guess_size - 1) * 160;

	str = g_string_sized_new(guess_size);

	for (l = sms_list; l; l = l->next) {
		guint8 taken = 0;
		guint8 dcs;
		guint8 udl;
		enum sms_charset charset;
		int udl_in_bytes;
		const guint8 *ud;
		struct sms_udh_iter iter;
		char *converted;

		sms = l->data;

		ud = sms_extract_common(sms, NULL, &dcs, &udl, NULL);

		if (!sms_mwi_dcs_decode(dcs, NULL, &charset, NULL, NULL) &&
			!sms_dcs_decode(dcs, NULL, &charset, NULL, NULL))
			continue;

		if (charset == SMS_CHARSET_8BIT)
			continue;

		if (sms_udh_iter_init(sms, &iter))
			taken = sms_udh_iter_get_udh_length(&iter) + 1;

		udl_in_bytes = sms_udl_in_bytes(udl, dcs);

		if (udl_in_bytes == taken)
			continue;

		if (charset == SMS_CHARSET_7BIT) {
			unsigned char buf[160];
			long written;
			guint8 locking_shift = 0;
			guint8 single_shift = 0;
			int max_chars = sms_text_capacity_gsm(udl, taken);

			if (unpack_7bit_own_buf(ud + taken,
						udl_in_bytes - taken,
						taken, FALSE, max_chars,
						&written, 0, buf) == NULL)
				continue;

			/* Take care of improperly split fragments */
			if (buf[written-1] == 0x1b)
				written = written - 1;

			sms_extract_language_variant(sms, &locking_shift,
								&single_shift);

			/*
			 * If language is not defined in 3GPP TS 23.038,
			 * implementations are instructed to ignore it
			 */
			if (locking_shift > SMS_ALPHABET_PORTUGUESE)
				locking_shift = GSM_DIALECT_DEFAULT;

			if (single_shift > SMS_ALPHABET_PORTUGUESE)
				single_shift = GSM_DIALECT_DEFAULT;

			converted = convert_gsm_to_utf8_with_lang(buf, written,
								NULL, NULL, 0,
								locking_shift,
								single_shift);
		} else {
			const gchar *from = (const gchar *) (ud + taken);
			/*
			 * According to the spec: A UCS2 character shall not be
			 * split in the middle; if the length of the User Data
			 * Header is odd, the maximum length of the whole TP-UD
			 * field is 139 octets
			 */
			gssize num_ucs2_chars = (udl_in_bytes - taken) >> 1;
			num_ucs2_chars = num_ucs2_chars << 1;

			converted = g_convert(from, num_ucs2_chars,
						"UTF-8//TRANSLIT", "UCS-2BE",
						NULL, NULL, NULL);
		}

		if (converted) {
			g_string_append(str, converted);
			g_free(converted);
		}
	}

	utf8 = g_string_free(str, FALSE);

	return utf8;
}

static int sms_serialize(unsigned char *buf, const struct sms *sms)
{
	int len, tpdu_len;

	sms_encode(sms, &len, &tpdu_len, buf + 1);
	buf[0] = tpdu_len;

	return len + 1;
}

static gboolean sms_deserialize(const unsigned char *buf,
		struct sms *sms, int len)
{
	if (len < 1)
		return FALSE;

	return sms_decode(buf + 1, len - 1, FALSE, buf[0], sms);
}

static gboolean sms_deserialize_outgoing(const unsigned char *buf,
		struct sms *sms, int len)
{
	if (len < 1)
		return FALSE;

	return sms_decode(buf + 1, len - 1, TRUE, buf[0], sms);
}

static gboolean sms_assembly_extract_address(const char *straddr,
						struct sms_address *out)
{
	unsigned char pdu[12];
	long len;
	int offset = 0;

	if (decode_hex_own_buf(straddr, -1, &len, 0, pdu) == NULL)
		return FALSE;

	return sms_decode_address_field(pdu, len, &offset, FALSE, out);
}

gboolean sms_address_to_hex_string(const struct sms_address *in, char *straddr)
{
	unsigned char pdu[12];
	int offset = 0;

	if (sms_encode_address_field(in, FALSE, pdu, &offset) == FALSE)
		return FALSE;

	if (encode_hex_own_buf(pdu, offset, 0, straddr) == NULL)
		return FALSE;

	straddr[offset * 2 + 1] = '\0';

	return TRUE;
}

static void sms_assembly_load(struct sms_assembly *assembly,
				const struct dirent *dir)
{
	struct sms_address addr;
	DECLARE_SMS_ADDR_STR(straddr);
	guint16 ref;
	guint8 max;
	guint8 seq;
	char *path;
	int len;
	struct stat segment_stat;
	struct dirent **segments;
	char *endp;
	int r;
	int i;
	unsigned char buf[177];
	struct sms segment;

	if (dir->d_type != DT_DIR)
		return;

	/* Max of SMS address size is 12 bytes, hex encoded */
	if (sscanf(dir->d_name, SMS_ADDR_FMT "-%hi-%hhi",
				straddr, &ref, &max) < 3)
		return;

	if (sms_assembly_extract_address(straddr, &addr) == FALSE)
		return;

	path = g_strdup_printf(SMS_BACKUP_PATH "/%s",
			assembly->imsi, dir->d_name);
	len = scandir(path, &segments, NULL, versionsort);
	g_free(path);

	if (len < 0)
		return;

	for (i = 0; i < len; i++) {
		if (segments[i]->d_type != DT_REG)
			continue;

		seq = strtol(segments[i]->d_name, &endp, 10);
		if (*endp != '\0')
			continue;

		r = read_file(buf, sizeof(buf), SMS_BACKUP_PATH "/%s/%s",
				assembly->imsi,
				dir->d_name, segments[i]->d_name);
		if (r < 0)
			continue;

		if (!sms_deserialize(buf, &segment, r))
			continue;

		path = g_strdup_printf(SMS_BACKUP_PATH "/%s/%s",
				assembly->imsi,
				dir->d_name, segments[i]->d_name);
		r = stat(path, &segment_stat);
		g_free(path);

		if (r != 0)
			continue;

		/* Errors cannot occur here */
		sms_assembly_add_fragment_backup(assembly, &segment,
						segment_stat.st_mtime,
						&addr, ref, max, seq, FALSE);
	}

	for (i = 0; i < len; i++)
		free(segments[i]);

	free(segments);
}

static gboolean sms_assembly_store(struct sms_assembly *assembly,
				struct sms_assembly_node *node,
				const struct sms *sms, guint8 seq)
{
	unsigned char buf[177];
	int len;
	DECLARE_SMS_ADDR_STR(straddr);

	if (assembly->imsi == NULL)
		return FALSE;

	if (sms_address_to_hex_string(&node->addr, straddr) == FALSE)
		return FALSE;

	len = sms_serialize(buf, sms);

	if (write_file(buf, len, SMS_BACKUP_MODE,
				SMS_BACKUP_PATH_FILE, assembly->imsi, straddr,
				node->ref, node->max_fragments, seq) != len)
		return FALSE;

	return TRUE;
}

static void sms_assembly_backup_free(struct sms_assembly *assembly,
					struct sms_assembly_node *node)
{
	char *path;
	int seq;
	DECLARE_SMS_ADDR_STR(straddr);

	if (assembly->imsi == NULL)
		return;

	if (sms_address_to_hex_string(&node->addr, straddr) == FALSE)
		return;

	for (seq = 0; seq < node->max_fragments; seq++) {
		int offset = seq / 32;
		int bit = 1 << (seq % 32);

		if (node->bitmap[offset] & bit) {
			path = g_strdup_printf(SMS_BACKUP_PATH_FILE,
					assembly->imsi, straddr,
					node->ref, node->max_fragments, seq);
			unlink(path);
			g_free(path);
		}
	}

	path = g_strdup_printf(SMS_BACKUP_PATH_DIR, assembly->imsi, straddr,
				node->ref, node->max_fragments);
	rmdir(path);
	g_free(path);
}

struct sms_assembly *sms_assembly_new(const char *imsi)
{
	struct sms_assembly *ret = g_new0(struct sms_assembly, 1);
	char *path;
	struct dirent **entries;
	int len;

	if (imsi) {
		ret->imsi = imsi;

		/* Restore state from backup */

		path = g_strdup_printf(SMS_BACKUP_PATH, imsi);
		len = scandir(path, &entries, NULL, alphasort);
		g_free(path);

		if (len < 0)
			return ret;

		while (len--) {
			sms_assembly_load(ret, entries[len]);
			free(entries[len]);
		}

		free(entries);
	}

	return ret;
}

void sms_assembly_free(struct sms_assembly *assembly)
{
	GSList *l;

	for (l = assembly->assembly_list; l; l = l->next) {
		struct sms_assembly_node *node = l->data;

		g_slist_foreach(node->fragment_list, (GFunc) g_free, 0);
		g_slist_free(node->fragment_list);
		g_free(node);
	}

	g_slist_free(assembly->assembly_list);
	g_free(assembly);
}

GSList *sms_assembly_add_fragment(struct sms_assembly *assembly,
					const struct sms *sms, time_t ts,
					const struct sms_address *addr,
					guint16 ref, guint8 max, guint8 seq)
{
	return sms_assembly_add_fragment_backup(assembly, sms,
						ts, addr, ref, max, seq, TRUE);
}

static GSList *sms_assembly_add_fragment_backup(struct sms_assembly *assembly,
					const struct sms *sms, time_t ts,
					const struct sms_address *addr,
					guint16 ref, guint8 max, guint8 seq,
					gboolean backup)
{
	unsigned int offset = seq / 32;
	unsigned int bit = 1 << (seq % 32);
	GSList *l;
	GSList *prev;
	struct sms *newsms;
	struct sms_assembly_node *node;
	GSList *completed;
	unsigned int position;
	unsigned int i;
	unsigned int j;

	prev = NULL;

	for (l = assembly->assembly_list; l; prev = l, l = l->next) {
		node = l->data;

		if (node->addr.number_type != addr->number_type)
			continue;

		if (node->addr.numbering_plan != addr->numbering_plan)
			continue;

		if (strcmp(node->addr.address, addr->address))
			continue;

		if (ref != node->ref)
			continue;

		/*
		 * Message Reference and address the same, but max is not
		 * ignore the SMS completely
		 */
		if (max != node->max_fragments)
			return NULL;

		/* Now check if we already have this seq number */
		if (node->bitmap[offset] & bit)
			return NULL;

		/*
		 * Iterate over the bitmap to find in which position
		 * should the fragment be inserted -- basically we
		 * walk each bit in the bitmap until the bit we care
		 * about (offset:bit) and count which are stored --
		 * that gives us in which position we have to insert.
		 */
		position = 0;
		for (i = 0; i < offset; i++)
			for (j = 0; j < 32; j++)
				if (node->bitmap[i] & (1 << j))
					position += 1;

		for (j = 1; j < bit; j = j << 1)
			if (node->bitmap[offset] & j)
				position += 1;

		goto out;
	}

	node = g_new0(struct sms_assembly_node, 1);
	memcpy(&node->addr, addr, sizeof(struct sms_address));
	node->ts = ts;
	node->ref = ref;
	node->max_fragments = max;

	assembly->assembly_list = g_slist_prepend(assembly->assembly_list,
							node);

	prev = NULL;
	l = assembly->assembly_list;
	position = 0;

out:
	newsms = g_new(struct sms, 1);

	memcpy(newsms, sms, sizeof(struct sms));
	node->fragment_list = g_slist_insert(node->fragment_list,
						newsms, position);
	node->bitmap[offset] |= bit;
	node->num_fragments += 1;

	if (node->num_fragments < node->max_fragments) {
		if (backup)
			sms_assembly_store(assembly, node, sms, seq);

		return NULL;
	}

	completed = node->fragment_list;

	sms_assembly_backup_free(assembly, node);

	if (prev)
		prev->next = l->next;
	else
		assembly->assembly_list = l->next;

	g_free(node);
	g_slist_free_1(l);
	return completed;
}

/*!
 * Expires all incomplete messages that have been received at time prior
 * to one given by before argument.  The fragment list is freed and the
 * SMSes are vaporized.
 */
void sms_assembly_expire(struct sms_assembly *assembly, time_t before)
{
	GSList *cur;
	GSList *prev;
	GSList *tmp;

	prev = NULL;
	cur = assembly->assembly_list;

	while (cur) {
		struct sms_assembly_node *node = cur->data;

		if (node->ts > before) {
			prev = cur;
			cur = cur->next;
			continue;
		}

		sms_assembly_backup_free(assembly, node);

		g_slist_foreach(node->fragment_list, (GFunc) g_free, 0);
		g_slist_free(node->fragment_list);
		g_free(node);

		if (prev)
			prev->next = cur->next;
		else
			assembly->assembly_list = cur->next;

		tmp = cur;
		cur = cur->next;
		g_slist_free_1(tmp);
	}
}

static gboolean sha1_equal(gconstpointer v1, gconstpointer v2)
{
	return memcmp(v1, v2, SMS_MSGID_LEN) == 0;
}

static guint sha1_hash(gconstpointer v)
{
	guint h;

	memcpy(&h, v, sizeof(h));

	return h;
}

static void sr_assembly_load_backup(GHashTable *assembly_table,
					const char *imsi,
					const struct dirent *addr_dir)
{
	struct sms_address addr;
	DECLARE_SMS_ADDR_STR(straddr);
	struct id_table_node *node;
	GHashTable *id_table;
	int r;
	char *assembly_table_key;
	unsigned int *id_table_key;
	char msgid_str[SMS_MSGID_LEN * 2 + 1];
	unsigned char msgid[SMS_MSGID_LEN];
	char endc;

	if (addr_dir->d_type != DT_REG)
		return;

	/*
	 * All SMS-messages under the same IMSI-code are
	 * included in the same directory.
	 * So, SMS-address and message ID are included in the same file name
	 * Max of SMS address size is 12 bytes, hex encoded
	 * Max of SMS SHA1 hash is 20 bytes, hex encoded
	 */
	if (sscanf(addr_dir->d_name, SMS_ADDR_FMT "-" SMS_MSGID_FMT "%c",
				straddr, msgid_str, &endc) != 2)
		return;

	if (sms_assembly_extract_address(straddr, &addr) == FALSE)
		return;

	if (strlen(msgid_str) != 2 * SMS_MSGID_LEN)
		return;

	if (decode_hex_own_buf(msgid_str, 2 * SMS_MSGID_LEN,
				NULL, 0, msgid) == NULL)
		return;

	node = g_new0(struct id_table_node, 1);

	r = read_file((unsigned char *) node,
			sizeof(struct id_table_node),
			SMS_SR_BACKUP_PATH "/%s",
			imsi, addr_dir->d_name);

	if (r < 0) {
		g_free(node);
		return;
	}

	id_table = g_hash_table_lookup(assembly_table,
					sms_address_to_string(&addr));

	/* Create hashtable keyed by the to address if required */
	if (id_table == NULL) {
		id_table = g_hash_table_new_full(sha1_hash, sha1_equal,
							g_free, g_free);

		assembly_table_key = g_strdup(sms_address_to_string(&addr));
		g_hash_table_insert(assembly_table, assembly_table_key,
					id_table);
	}

	/* Node ready, create key and add them to the table */
	id_table_key = g_memdup(msgid, SMS_MSGID_LEN);

	g_hash_table_insert(id_table, id_table_key, node);
}

struct status_report_assembly *status_report_assembly_new(const char *imsi)
{
	char *path;
	int len;
	struct dirent **addresses;
	struct status_report_assembly *ret =
				g_new0(struct status_report_assembly, 1);

	ret->assembly_table = g_hash_table_new_full(g_str_hash, g_str_equal,
				g_free, (GDestroyNotify) g_hash_table_destroy);

	if (imsi) {
		ret->imsi = imsi;

		/* Restore state from backup */
		path = g_strdup_printf(SMS_SR_BACKUP_PATH, imsi);
		len = scandir(path, &addresses, NULL, alphasort);

		g_free(path);

		if (len < 0)
			return ret;

		/*
		 * Go through different addresses. Each address can relate to
		 * 1-n msg_ids.
		 */

		while (len--) {
			sr_assembly_load_backup(ret->assembly_table, imsi,
								addresses[len]);
			g_free(addresses[len]);
		}

		g_free(addresses);
	}

	return ret;
}

static gboolean sr_assembly_add_fragment_backup(const char *imsi,
					const struct id_table_node *node,
					const struct sms_address *addr,
					const unsigned char *msgid)
{
	int len = sizeof(struct id_table_node);
	DECLARE_SMS_ADDR_STR(straddr);
	char msgid_str[SMS_MSGID_LEN * 2 + 1];

	if (imsi == NULL)
		return FALSE;

	if (sms_address_to_hex_string(addr, straddr) == FALSE)
		return FALSE;

	if (encode_hex_own_buf(msgid, SMS_MSGID_LEN, 0, msgid_str) == NULL)
		return FALSE;

	/* storagedir/%s/sms_sr/%s-%s */
	if (write_file((unsigned char *) node, len, SMS_BACKUP_MODE,
			SMS_SR_BACKUP_PATH_FILE, imsi,
			straddr, msgid_str) != len)
		return FALSE;

	return TRUE;
}

static gboolean sr_assembly_remove_fragment_backup(const char *imsi,
					const struct sms_address *addr,
					const unsigned char *sha1)
{
	char *path;
	DECLARE_SMS_ADDR_STR(straddr);
	char msgid_str[SMS_MSGID_LEN * 2 + 1];

	if (imsi == NULL)
		return FALSE;

	if (sms_address_to_hex_string(addr, straddr) == FALSE)
		return FALSE;

	if (encode_hex_own_buf(sha1, SMS_MSGID_LEN, 0, msgid_str) == FALSE)
		return FALSE;

	path = g_strdup_printf(SMS_SR_BACKUP_PATH_FILE,
					imsi, straddr, msgid_str);

	unlink(path);
	g_free(path);

	return TRUE;
}

void status_report_assembly_free(struct status_report_assembly *assembly)
{
	g_hash_table_destroy(assembly->assembly_table);
	g_free(assembly);
}

static gboolean sr_st_to_delivered(enum sms_st st, gboolean *delivered)
{
	if (st >= SMS_ST_TEMPFINAL_CONGESTION && st <= SMS_ST_TEMPFINAL_LAST)
		return FALSE;

	if (st >= SMS_ST_TEMPORARY_CONGESTION && st <= SMS_ST_TEMPORARY_LAST)
		return FALSE;

	if (st <= SMS_ST_COMPLETED_LAST) {
		*delivered = TRUE;
		return TRUE;
	}

	if (st >= SMS_ST_PERMANENT_RP_ERROR && st <= SMS_ST_PERMANENT_LAST) {
		*delivered = FALSE;
		return TRUE;
	}

	return FALSE;
}

static struct id_table_node *find_by_mr_and_mark(GHashTable *id_table,
						unsigned char mr,
						GHashTableIter *out_iter,
						unsigned char **out_id)
{
	unsigned int offset = mr / 32;
	unsigned int bit = 1 << (mr % 32);
	gpointer key, value;
	struct id_table_node *node;

	g_hash_table_iter_init(out_iter, id_table);
	while (g_hash_table_iter_next(out_iter, &key, &value)) {
		node = value;

		/* Address and MR matched */
		if (node->mrs[offset] & bit) {
			node->mrs[offset] ^= bit;
			*out_id = key;

			return node;
		}
	}

	return NULL;
}

/*
 * Key (receiver address) does not exist in assembly. Some networks can change
 * address to international format, although address is sent in the national
 * format. Handle also change from national to international format.
 * Notify these special cases by comparing only last six digits of the assembly
 * addresses and received address. If address contains less than six digits,
 * compare only existing digits.
 */
static struct id_table_node *fuzzy_lookup(struct status_report_assembly *assy,
						const struct sms *sr,
						const char **out_addr,
						GHashTableIter *out_iter,
						unsigned char **out_msgid)
{
	GHashTableIter iter_addr;
	gpointer key, value;
	const char *r_addr;

	r_addr = sms_address_to_string(&sr->status_report.raddr);
	g_hash_table_iter_init(&iter_addr, assy->assembly_table);

	while (g_hash_table_iter_next(&iter_addr, &key, &value)) {
		const char *s_addr = key;
		GHashTable *id_table = value;
		unsigned int len, r_len, s_len;
		unsigned int i;
		struct id_table_node *node;

		if (r_addr[0] == '+' && s_addr[0] == '+')
			continue;

		if (r_addr[0] != '+' && s_addr[0] != '+')
			continue;

		r_len = strlen(r_addr);
		s_len = strlen(s_addr);

		len = MIN(6, MIN(r_len, s_len));

		for (i = 0; i < len; i++)
			if (s_addr[s_len - i - 1] != r_addr[r_len - i - 1])
				break;

		/* Not all digits matched. */
		if (i < len)
			continue;

		/* Address matched. Check message reference. */
		node = find_by_mr_and_mark(id_table, sr->status_report.mr,
						out_iter, out_msgid);
		if (node != NULL) {
			*out_addr = s_addr;
			return node;
		}
	}

	return NULL;
}

gboolean status_report_assembly_report(struct status_report_assembly *assembly,
					const struct sms *sr,
					unsigned char *out_msgid,
					gboolean *out_delivered)
{
	const char *straddr;
	GHashTable *id_table;
	GHashTableIter iter;
	struct sms_address addr;
	struct id_table_node *node;
	gboolean delivered;
	gboolean pending;
	unsigned char *msgid;
	int i;

	/* We ignore temporary or tempfinal status reports */
	if (sr_st_to_delivered(sr->status_report.st, &delivered) == FALSE)
		return FALSE;

	straddr = sms_address_to_string(&sr->status_report.raddr);
	id_table = g_hash_table_lookup(assembly->assembly_table, straddr);

	if (id_table != NULL)
		node = find_by_mr_and_mark(id_table, sr->status_report.mr,
						&iter, &msgid);
	else
		node = fuzzy_lookup(assembly, sr, &straddr, &iter, &msgid);

	/* Unable to find a message reference belonging to this address */
	if (node == NULL)
		return FALSE;

	node->deliverable = node->deliverable && delivered;

	/* If we haven't sent the entire message yet, wait until sent */
	if (node->sent_mrs < node->total_mrs)
		return FALSE;

	/* Figure out if we are expecting more status reports */
	for (i = 0, pending = FALSE; i < 8; i++) {
		/* There are still pending mr(s). */
		if (node->mrs[i] != 0) {
			pending = TRUE;
			break;
		}
	}

	sms_address_from_string(&addr, straddr);

	if (pending == TRUE && node->deliverable == TRUE) {
		/*
		 * More status reports expected, and already received
		 * reports completed. Update backup file.
		 */
		sr_assembly_add_fragment_backup(assembly->imsi, node,
						&addr, msgid);

		return FALSE;
	}

	if (out_delivered)
		*out_delivered = node->deliverable;

	if (out_msgid)
		memcpy(out_msgid, msgid, SMS_MSGID_LEN);

	sr_assembly_remove_fragment_backup(assembly->imsi, &addr, msgid);
	id_table = g_hash_table_iter_get_hash_table(&iter);
	g_hash_table_iter_remove(&iter);

	if (g_hash_table_size(id_table) == 0)
		g_hash_table_remove(assembly->assembly_table, straddr);

	return TRUE;
}

void status_report_assembly_add_fragment(
					struct status_report_assembly *assembly,
					const unsigned char *msgid,
					const struct sms_address *to,
					unsigned char mr, time_t expiration,
					unsigned char total_mrs)
{
	unsigned int offset = mr / 32;
	unsigned int bit = 1 << (mr % 32);
	GHashTable *id_table;
	struct id_table_node *node;
	unsigned char *id_table_key;

	id_table = g_hash_table_lookup(assembly->assembly_table,
					sms_address_to_string(to));

	/* Create hashtable keyed by the to address if required */
	if (id_table == NULL) {
		id_table = g_hash_table_new_full(sha1_hash, sha1_equal,
								g_free, g_free);
		g_hash_table_insert(assembly->assembly_table,
					g_strdup(sms_address_to_string(to)),
					id_table);
	}

	node = g_hash_table_lookup(id_table, msgid);

	/* Create node in the message id hashtable if required */
	if (node == NULL) {
		id_table_key = g_memdup(msgid, SMS_MSGID_LEN);

		node = g_new0(struct id_table_node, 1);
		node->total_mrs = total_mrs;
		node->deliverable = TRUE;

		g_hash_table_insert(id_table, id_table_key, node);
	}

	/* id_table and node both exists */
	node->mrs[offset] |= bit;
	node->expiration = expiration;
	node->sent_mrs++;
	sr_assembly_add_fragment_backup(assembly->imsi, node, to, msgid);
}

void status_report_assembly_expire(struct status_report_assembly *assembly,
					time_t before)
{
	GHashTable *id_table;
	GHashTableIter iter_addr, iter_node;
	struct sms_address addr;
	char *straddr;
	gpointer key;
	struct id_table_node *node;

	g_hash_table_iter_init(&iter_addr, assembly->assembly_table);

	/*
	 * Go through different addresses. Each address can relate to
	 * 1-n msg_ids.
	 */
	while (g_hash_table_iter_next(&iter_addr, (gpointer) &straddr,
					(gpointer) &id_table)) {

		sms_address_from_string(&addr, straddr);
		g_hash_table_iter_init(&iter_node, id_table);

		/* Go through different messages. */
		while (g_hash_table_iter_next(&iter_node, &key,
						(gpointer) &node)) {
			/*
			 * If message is expired, removed it from the
			 * hash-table and remove the backup-file
			 */
			if (node->expiration <= before) {
				g_hash_table_iter_remove(&iter_node);

				sr_assembly_remove_fragment_backup(
								assembly->imsi,
								&addr,
								key);
			}
		}

		/*
		 * If all messages are removed, remove address
		 * from the hash-table.
		 */
		if (g_hash_table_size(id_table) == 0)
			g_hash_table_iter_remove(&iter_addr);
	}
}

static int sms_tx_load_filter(const struct dirent *dent)
{
	char *endp;
	guint8 seq __attribute__ ((unused));

	if (dent->d_type != DT_REG)
		return 0;

	seq = strtol(dent->d_name, &endp, 10);
	if (*endp != '\0')
		return 0;

	return 1;
}

/*
 * Each directory contains a file per pdu.
 */
static GSList *sms_tx_load(const char *imsi, const struct dirent *dir)
{
	GSList *list = NULL;
	struct dirent **pdus;
	char *path;
	int len, r;
	unsigned char buf[177];
	struct sms s;

	if (dir->d_type != DT_DIR)
		return NULL;

	path = g_strdup_printf(SMS_TX_BACKUP_PATH "/%s", imsi, dir->d_name);
	len = scandir(path, &pdus, sms_tx_load_filter, versionsort);
	g_free(path);

	if (len < 0)
		return NULL;

	while (len--) {
		r = read_file(buf, sizeof(buf), SMS_TX_BACKUP_PATH "/%s/%s",
					imsi, dir->d_name, pdus[len]->d_name);

		if (r < 0)
			goto free_pdu;

		if (sms_deserialize_outgoing(buf, &s, r) == FALSE)
			goto free_pdu;

		list = g_slist_prepend(list, g_memdup(&s, sizeof(s)));

free_pdu:
		g_free(pdus[len]);
	}

	g_free(pdus);

	return list;
}

static int sms_tx_queue_filter(const struct dirent *dirent)
{
	if (dirent->d_type != DT_DIR)
		return 0;

	if (!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))
		return 0;

	return 1;
}

/*
 * populate the queue with tx_backup_entry from stored backup
 * data.
 */
GQueue *sms_tx_queue_load(const char *imsi)
{
	GQueue *retq = 0;
	char *path;
	struct dirent **entries;
	int len;
	int i;
	unsigned long id;

	if (imsi == NULL)
		return NULL;

	path = g_strdup_printf(SMS_TX_BACKUP_PATH, imsi);

	len = scandir(path, &entries, sms_tx_queue_filter, versionsort);
	if (len < 0)
		goto nodir_exit;

	retq = g_queue_new();

	for (i = 0, id = 0; i < len; i++) {
		char uuid[SMS_MSGID_LEN * 2 + 1];
		GSList *msg_list;
		unsigned long oldid;
		unsigned long flags;
		char *oldpath, *newpath;
		struct txq_backup_entry *entry;
		struct dirent *dir = entries[i];
		char endc;

		if (sscanf(dir->d_name, "%lu-%lu-" SMS_MSGID_FMT "%c",
					&oldid, &flags, uuid, &endc) != 3)
			continue;

		if (strlen(uuid) !=  2 * SMS_MSGID_LEN)
			continue;

		msg_list = sms_tx_load(imsi, dir);
		if (msg_list == NULL)
			continue;

		entry = g_new0(struct txq_backup_entry, 1);
		entry->msg_list = msg_list;
		entry->flags = flags;
		decode_hex_own_buf(uuid, -1, NULL, 0, entry->uuid);

		g_queue_push_tail(retq, entry);

		/* Don't bother re-shuffling the ids if they are the same */
		if (oldid == id) {
			id++;
			continue;
		}

		oldpath = g_strdup_printf("%s/%s", path, dir->d_name);
		newpath = g_strdup_printf(SMS_TX_BACKUP_PATH_DIR,
						imsi, id++, flags, uuid);

		/* rename directory to reflect new position in queue */
		rename(oldpath, newpath);

		g_free(newpath);
		g_free(oldpath);
	}

	for (i = 0; i < len; i++)
		g_free(entries[i]);

	g_free(entries);

nodir_exit:
	g_free(path);
	return retq;
}

gboolean sms_tx_backup_store(const char *imsi, unsigned long id,
				unsigned long flags, const char *uuid,
				guint8 seq, const unsigned char *pdu,
				int pdu_len, int tpdu_len)
{
	unsigned char buf[177];
	int len;

	if (!imsi)
		return FALSE;

	memcpy(buf + 1, pdu, pdu_len);
	buf[0] = tpdu_len;
	len = pdu_len + 1;

	/*
	 * file name is: imsi/tx_queue/order-flags-uuid/pdu
	 */
	if (write_file(buf, len, SMS_BACKUP_MODE, SMS_TX_BACKUP_PATH_FILE,
					imsi, id, flags, uuid, seq) != len)
		return FALSE;

	return TRUE;
}

void sms_tx_backup_free(const char *imsi, unsigned long id,
				unsigned long flags, const char *uuid)
{
	char *path;
	struct dirent **entries;
	int len;

	path = g_strdup_printf(SMS_TX_BACKUP_PATH_DIR,
					imsi, id, flags, uuid);

	len = scandir(path, &entries, NULL, versionsort);

	if (len < 0)
		goto nodir_exit;

	/* skip '..' and '.' entries */
	while (len-- > 2) {
		struct dirent *dir = entries[len];
		char *file = g_strdup_printf("%s/%s", path, dir->d_name);

		unlink(file);
		g_free(file);

		g_free(entries[len]);
	}

	g_free(entries[1]);
	g_free(entries[0]);
	g_free(entries);

	rmdir(path);

nodir_exit:
	g_free(path);
}

void sms_tx_backup_remove(const char *imsi, unsigned long id,
				unsigned long flags, const char *uuid,
				guint8 seq)
{
	char *path;

	path = g_strdup_printf(SMS_TX_BACKUP_PATH_FILE,
					imsi, id, flags, uuid, seq);
	unlink(path);

	g_free(path);
}

static inline GSList *sms_list_append(GSList *l, const struct sms *in)
{
	struct sms *sms;

	sms = g_new(struct sms, 1);
	memcpy(sms, in, sizeof(struct sms));
	l = g_slist_prepend(l, sms);

	return l;
}

/*
 * Prepares a datagram for transmission.  Breaks up into fragments if
 * necessary using ref as the concatenated message reference number.
 * Returns a list of sms messages in order.
 *
 * @use_delivery_reports: value for the Status-Report-Request field
 *     (23.040 3.2.9, 9.2.2.2)
 */
GSList *sms_datagram_prepare(const char *to,
				const unsigned char *data, unsigned int len,
				guint16 ref, gboolean use_16bit_ref,
				unsigned short src, unsigned short dst,
				gboolean use_16bit_port,
				gboolean use_delivery_reports)
{
	struct sms template;
	unsigned int offset;
	unsigned int written;
	unsigned int left;
	guint8 seq;
	GSList *r = NULL;

	memset(&template, 0, sizeof(struct sms));
	template.type = SMS_TYPE_SUBMIT;
	template.submit.rd = FALSE;
	template.submit.vpf = SMS_VALIDITY_PERIOD_FORMAT_RELATIVE;
	template.submit.rp = FALSE;
	template.submit.srr = use_delivery_reports;
	template.submit.mr = 0;
	template.submit.vp.relative = 0xA7; /* 24 Hours */
	template.submit.dcs = 0x04; /* Class Unspecified, 8 Bit */
	template.submit.udhi = TRUE;
	sms_address_from_string(&template.submit.daddr, to);

	offset = 1;

	if (use_16bit_port) {
		template.submit.ud[0] += 6;
		template.submit.ud[offset] = SMS_IEI_APPLICATION_ADDRESS_16BIT;
		template.submit.ud[offset + 1] = 4;
		template.submit.ud[offset + 2] = (dst & 0xff00) >> 8;
		template.submit.ud[offset + 3] = dst & 0xff;
		template.submit.ud[offset + 4] = (src & 0xff00) >> 8;
		template.submit.ud[offset + 5] = src & 0xff;

		offset += 6;
	} else {
		template.submit.ud[0] += 4;
		template.submit.ud[offset] = SMS_IEI_APPLICATION_ADDRESS_8BIT;
		template.submit.ud[offset + 1] = 2;
		template.submit.ud[offset + 2] = dst & 0xff;
		template.submit.ud[offset + 3] = src & 0xff;

		offset += 4;
	}

	if (len <= (140 - offset)) {
		template.submit.udl = len + offset;
		memcpy(template.submit.ud + offset, data, len);

		return sms_list_append(NULL, &template);
	}

	if (use_16bit_ref) {
		template.submit.ud[0] += 6;
		template.submit.ud[offset] = SMS_IEI_CONCATENATED_16BIT;
		template.submit.ud[offset + 1] = 4;
		template.submit.ud[offset + 2] = (ref & 0xff00) >> 8;
		template.submit.ud[offset + 3] = ref & 0xff;

		offset += 6;
	} else {
		template.submit.ud[0] += 5;
		template.submit.ud[offset] = SMS_IEI_CONCATENATED_8BIT;
		template.submit.ud[offset + 1] = 3;
		template.submit.ud[offset + 2] = ref & 0xff;

		offset += 5;
	}

	seq = 0;
	left = len;
	written = 0;

	while (left > 0) {
		unsigned int chunk;

		seq += 1;

		chunk = 140 - offset;
		if (left < chunk)
			chunk = left;

		template.submit.udl = chunk + offset;
		memcpy(template.submit.ud + offset, data + written, chunk);

		written += chunk;
		left -= chunk;

		template.submit.ud[offset - 1] = seq;

		r = sms_list_append(r, &template);

		if (seq == 255)
			break;
	}

	if (left > 0) {
		g_slist_foreach(r, (GFunc) g_free, NULL);
		g_slist_free(r);

		return NULL;
	} else {
		GSList *l;

		for (l = r; l; l = l->next) {
			struct sms *sms = l->data;

			sms->submit.ud[offset - 2] = seq;
		}
	}

	r = g_slist_reverse(r);

	return r;
}

/*
 * Prepares the text for transmission.  Breaks up into fragments if
 * necessary using ref as the concatenated message reference number.
 * Returns a list of sms messages in order.
 *
 * @use_delivery_reports: value for the Status-Report-Request field
 *     (23.040 3.2.9, 9.2.2.2)
 */
GSList *sms_text_prepare_with_alphabet(const char *to, const char *utf8,
					guint16 ref, gboolean use_16bit,
					gboolean use_delivery_reports,
					enum sms_alphabet alphabet)
{
	struct sms template;
	int offset = 0;
	unsigned char *gsm_encoded = NULL;
	char *ucs2_encoded = NULL;
	long written;
	long left;
	guint8 seq;
	GSList *r = NULL;
	enum gsm_dialect used_locking;
	enum gsm_dialect used_single;

	memset(&template, 0, sizeof(struct sms));
	template.type = SMS_TYPE_SUBMIT;
	template.submit.rd = FALSE;
	template.submit.vpf = SMS_VALIDITY_PERIOD_FORMAT_RELATIVE;
	template.submit.rp = FALSE;
	template.submit.srr = use_delivery_reports;
	template.submit.mr = 0;
	template.submit.vp.relative = 0xA7; /* 24 Hours */
	sms_address_from_string(&template.submit.daddr, to);

	/*
	 * UDHI, UDL, UD and DCS actually depend on the contents of
	 * the text, and also on the GSM dialect we use to encode it.
	 */
	gsm_encoded = convert_utf8_to_gsm_best_lang(utf8, -1, NULL, &written, 0,
							alphabet, &used_locking,
							&used_single);
	if (gsm_encoded == NULL) {
		gsize converted;

		ucs2_encoded = g_convert(utf8, -1, "UCS-2BE//TRANSLIT", "UTF-8",
						NULL, &converted, NULL);
		written = converted;
	}

	if (gsm_encoded == NULL && ucs2_encoded == NULL)
		return NULL;

	if (gsm_encoded != NULL)
		template.submit.dcs = 0x00; /* Class Unspecified, 7 Bit */
	else
		template.submit.dcs = 0x08; /* Class Unspecified, UCS2 */

	if (gsm_encoded != NULL && used_single != GSM_DIALECT_DEFAULT) {
		if (!offset)
			offset = 1;

		template.submit.ud[0] += 3;
		template.submit.ud[offset] = SMS_IEI_NATIONAL_LANGUAGE_SINGLE_SHIFT;
		template.submit.ud[offset + 1] = 1;
		template.submit.ud[offset + 2] = used_single;
		offset += 3;
	}

	if (gsm_encoded != NULL && used_locking != GSM_DIALECT_DEFAULT) {
		if (!offset)
			offset = 1;

		template.submit.ud[0] += 3;
		template.submit.ud[offset] = SMS_IEI_NATIONAL_LANGUAGE_LOCKING_SHIFT;
		template.submit.ud[offset + 1] = 1;
		template.submit.ud[offset + 2] = used_locking;
		offset += 3;
	}

	if (offset != 0)
		template.submit.udhi = TRUE;

	if (gsm_encoded && (written <= sms_text_capacity_gsm(160, offset))) {
		template.submit.udl = written + (offset * 8 + 6) / 7;
		pack_7bit_own_buf(gsm_encoded, written, offset, FALSE, NULL,
					0, template.submit.ud + offset);

		g_free(gsm_encoded);
		return sms_list_append(NULL, &template);
	}

	if (ucs2_encoded && (written <= (140 - offset))) {
		template.submit.udl = written + offset;
		memcpy(template.submit.ud + offset, ucs2_encoded, written);

		g_free(ucs2_encoded);
		return sms_list_append(NULL, &template);
	}

	template.submit.udhi = TRUE;

	if (!offset)
		offset = 1;

	if (use_16bit) {
		template.submit.ud[0] += 6;
		template.submit.ud[offset] = SMS_IEI_CONCATENATED_16BIT;
		template.submit.ud[offset + 1] = 4;
		template.submit.ud[offset + 2] = (ref & 0xff00) >> 8;
		template.submit.ud[offset + 3] = ref & 0xff;

		offset += 6;
	} else {
		template.submit.ud[0] += 5;
		template.submit.ud[offset] = SMS_IEI_CONCATENATED_8BIT;
		template.submit.ud[offset + 1] = 3;
		template.submit.ud[offset + 2] = ref & 0xff;

		offset += 5;
	}

	seq = 0;
	left = written;
	written = 0;

	while (left > 0) {
		long chunk;

		seq += 1;

		if (gsm_encoded) {
			chunk = sms_text_capacity_gsm(160, offset);

			if (left < chunk)
				chunk = left;

			if (gsm_encoded[written + chunk - 1] == 0x1b)
				chunk -= 1;

			template.submit.udl = chunk + (offset * 8 + 6) / 7;
			pack_7bit_own_buf(gsm_encoded + written, chunk,
						offset, FALSE, NULL, 0,
						template.submit.ud + offset);
		} else {
			chunk = 140 - offset;
			chunk &= ~0x1;

			if (left < chunk)
				chunk = left;

			template.submit.udl = chunk + offset;
			memcpy(template.submit.ud + offset,
				ucs2_encoded + written, chunk);
		}

		written += chunk;
		left -= chunk;

		template.submit.ud[offset - 1] = seq;

		r = sms_list_append(r, &template);

		if (seq == 255)
			break;
	}

	if (gsm_encoded)
		g_free(gsm_encoded);

	if (ucs2_encoded)
		g_free(ucs2_encoded);

	if (left > 0) {
		g_slist_foreach(r, (GFunc) g_free, NULL);
		g_slist_free(r);

		return NULL;
	} else {
		GSList *l;

		for (l = r; l; l = l->next) {
			struct sms *sms = l->data;

			sms->submit.ud[offset - 2] = seq;
		}
	}

	r = g_slist_reverse(r);

	return r;
}

GSList *sms_text_prepare(const char *to, const char *utf8, guint16 ref,
				gboolean use_16bit,
				gboolean use_delivery_reports)
{
	return sms_text_prepare_with_alphabet(to, utf8, ref, use_16bit,
						use_delivery_reports,
						SMS_ALPHABET_DEFAULT);
}

gboolean cbs_dcs_decode(guint8 dcs, gboolean *udhi, enum sms_class *cls,
			enum sms_charset *charset, gboolean *compressed,
			enum cbs_language *language, gboolean *iso639)
{
	guint8 upper = (dcs & 0xf0) >> 4;
	guint8 lower = dcs & 0xf;
	enum sms_charset ch;
	enum sms_class cl;
	enum cbs_language lang = CBS_LANGUAGE_UNSPECIFIED;
	gboolean iso = FALSE;
	gboolean comp = FALSE;
	gboolean udh = FALSE;

	if (upper == 0x3 || upper == 0x8 || (upper >= 0xA && upper <= 0xE))
		return FALSE;

	switch (upper) {
	case 0:
		ch = SMS_CHARSET_7BIT;
		cl = SMS_CLASS_UNSPECIFIED;
		lang = (enum cbs_language) lower;
		break;
	case 1:
		if (lower > 1)
			return FALSE;

		if (lower == 0)
			ch = SMS_CHARSET_7BIT;
		else
			ch = SMS_CHARSET_UCS2;

		cl = SMS_CLASS_UNSPECIFIED;
		iso = TRUE;

		break;
	case 2:
		if (lower > 4)
			return FALSE;

		ch = SMS_CHARSET_7BIT;
		cl = SMS_CLASS_UNSPECIFIED;
		lang = (enum cbs_language) dcs;
		break;
	case 4:
	case 5:
	case 6:
	case 7:
		comp = (dcs & 0x20) ? TRUE : FALSE;

		if (dcs & 0x10)
			cl = (enum sms_class) (dcs & 0x03);
		else
			cl = SMS_CLASS_UNSPECIFIED;

		if (((dcs & 0x0c) >> 2) < 3)
			ch = (enum sms_charset) ((dcs & 0x0c) >> 2);
		else
			return FALSE;

		break;
	case 9:
		udh = TRUE;
		cl = (enum sms_class) (dcs & 0x03);
		if (((dcs & 0x0c) >> 2) < 3)
			ch = (enum sms_charset) ((dcs & 0x0c) >> 2);
		else
			return FALSE;

		break;
	case 15:
		if (lower & 0x8)
			return FALSE;

		if (lower & 0x4)
			ch = SMS_CHARSET_8BIT;
		else
			ch = SMS_CHARSET_7BIT;

		if (lower & 0x3)
			cl = (enum sms_class) (lower & 0x3);
		else
			cl = SMS_CLASS_UNSPECIFIED;

		break;
	default:
		return FALSE;
	};

	if (udhi)
		*udhi = udh;

	if (cls)
		*cls = cl;

	if (charset)
		*charset = ch;

	if (compressed)
		*compressed = comp;

	if (language)
		*language = lang;

	if (iso639)
		*iso639 = iso;

	return TRUE;
}

gboolean cbs_decode(const unsigned char *pdu, int len, struct cbs *out)
{
	/* CBS is always a fixed length of 88 bytes */
	if (len != 88)
		return FALSE;

	out->gs = (enum cbs_geo_scope) ((pdu[0] >> 6) & 0x03);
	out->message_code = ((pdu[0] & 0x3f) << 4) | ((pdu[1] >> 4) & 0xf);
	out->update_number = (pdu[1] & 0xf);
	out->message_identifier = (pdu[2] << 8) | pdu[3];
	out->dcs = pdu[4];
	out->max_pages = pdu[5] & 0xf;
	out->page = (pdu[5] >> 4) & 0xf;

	/*
	 * If a mobile receives the code 0000 in either the first field or
	 * the second field then it shall treat the CBS message exactly the
	 * same as a CBS message with page parameter 0001 0001 (i.e. a single
	 * page message).
	 */
	if (out->max_pages == 0 || out->page == 0) {
		out->max_pages = 1;
		out->page = 1;
	}

	memcpy(out->ud, pdu + 6, 82);

	return TRUE;
}

gboolean cbs_encode(const struct cbs *cbs, int *len, unsigned char *pdu)
{
	pdu[0] = (cbs->gs << 6) | ((cbs->message_code >> 4) & 0x3f);
	pdu[1] = ((cbs->message_code & 0xf) << 4) | cbs->update_number;
	pdu[2] = cbs->message_identifier >> 8;
	pdu[3] = cbs->message_identifier & 0xff;
	pdu[4] = cbs->dcs;
	pdu[5] = cbs->max_pages | (cbs->page << 4);

	memcpy(pdu + 6, cbs->ud, 82);

	if (len)
		*len = 88;

	return TRUE;
}

gboolean cbs_extract_app_port(const struct cbs *cbs, int *dst, int *src,
				gboolean *is_8bit)
{
	struct sms_udh_iter iter;

	if (!sms_udh_iter_init_from_cbs(cbs, &iter))
		return FALSE;

	return extract_app_port_common(&iter, dst, src, is_8bit);
}

gboolean iso639_2_from_language(enum cbs_language lang, char *iso639)
{
	switch (lang) {
	case CBS_LANGUAGE_GERMAN:
		iso639[0] = 'd';
		iso639[1] = 'e';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_ENGLISH:
		iso639[0] = 'e';
		iso639[1] = 'n';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_ITALIAN:
		iso639[0] = 'i';
		iso639[1] = 't';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_FRENCH:
		iso639[0] = 'f';
		iso639[1] = 'r';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_SPANISH:
		iso639[0] = 'e';
		iso639[1] = 's';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_DUTCH:
		iso639[0] = 'n';
		iso639[1] = 'l';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_SWEDISH:
		iso639[0] = 's';
		iso639[1] = 'v';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_DANISH:
		iso639[0] = 'd';
		iso639[1] = 'a';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_PORTUGESE:
		iso639[0] = 'p';
		iso639[1] = 't';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_FINNISH:
		iso639[0] = 'f';
		iso639[1] = 'i';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_NORWEGIAN:
		iso639[0] = 'n';
		iso639[1] = 'o';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_GREEK:
		iso639[0] = 'e';
		iso639[1] = 'l';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_TURKISH:
		iso639[0] = 't';
		iso639[1] = 'r';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_HUNGARIAN:
		iso639[0] = 'h';
		iso639[1] = 'u';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_POLISH:
		iso639[0] = 'p';
		iso639[1] = 'l';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_CZECH:
		iso639[0] = 'c';
		iso639[1] = 's';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_HEBREW:
		iso639[0] = 'h';
		iso639[1] = 'e';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_ARABIC:
		iso639[0] = 'a';
		iso639[1] = 'r';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_RUSSIAN:
		iso639[0] = 'r';
		iso639[1] = 'u';
		iso639[2] = '\0';
		return TRUE;
	case CBS_LANGUAGE_ICELANDIC:
		iso639[0] = 'i';
		iso639[1] = 's';
		iso639[2] = '\0';
		return TRUE;
	default:
		iso639[0] = '\0';
		break;
	}

	return FALSE;
}

char *cbs_decode_text(GSList *cbs_list, char *iso639_lang)
{
	GSList *l;
	const struct cbs *cbs;
	enum sms_charset uninitialized_var(charset);
	enum cbs_language lang;
	gboolean uninitialized_var(iso639);
	int bufsize = 0;
	unsigned char *buf;
	char *utf8;

	if (cbs_list == NULL)
		return NULL;

	/*
	 * CBS can only come from the network, so we're much less lenient
	 * on what we support.  Namely we require the same charset to be
	 * used across all pages.
	 */
	for (l = cbs_list; l; l = l->next) {
		enum sms_charset curch;
		gboolean curiso;

		cbs = l->data;

		if (!cbs_dcs_decode(cbs->dcs, NULL, NULL,
					&curch, NULL, &lang, &curiso))
			return NULL;

		if (l == cbs_list) {
			iso639 = curiso;
			charset = curch;
		}

		if (curch != charset)
			return NULL;

		if (curiso != iso639)
			return NULL;

		if (curch == SMS_CHARSET_8BIT)
			return NULL;

		if (curch == SMS_CHARSET_7BIT) {
			bufsize += CBS_MAX_GSM_CHARS;

			if (iso639)
				bufsize -= 3;
		} else {
			bufsize += 82;

			if (iso639)
				bufsize -= 2;
		}
	}

	if (lang) {
		cbs = cbs_list->data;

		if (iso639) {
			struct sms_udh_iter iter;
			int taken = 0;

			if (sms_udh_iter_init_from_cbs(cbs, &iter))
				taken = sms_udh_iter_get_udh_length(&iter) + 1;

			unpack_7bit_own_buf(cbs->ud + taken, 82 - taken,
						taken, FALSE, 2,
						NULL, 0,
						(unsigned char *)iso639_lang);
			iso639_lang[2] = '\0';
		} else {
			iso639_2_from_language(lang, iso639_lang);
		}
	}

	buf = g_new(unsigned char, bufsize);
	bufsize = 0;

	for (l = cbs_list; l; l = l->next) {
		const guint8 *ud;
		struct sms_udh_iter iter;
		int taken = 0;

		cbs = l->data;
		ud = cbs->ud;

		if (sms_udh_iter_init_from_cbs(cbs, &iter))
			taken = sms_udh_iter_get_udh_length(&iter) + 1;

		if (charset == SMS_CHARSET_7BIT) {
			unsigned char unpacked[CBS_MAX_GSM_CHARS];
			long written;
			int max_chars;
			int i;

			max_chars =
				sms_text_capacity_gsm(CBS_MAX_GSM_CHARS, taken);

			unpack_7bit_own_buf(ud + taken, 82 - taken,
						taken, FALSE, max_chars,
						&written, 0, unpacked);

			i = iso639 ? 3 : 0;

			/*
			 * CR is a padding character, which means we can
			 * safely discard everything afterwards
			 */
			for (; i < written; i++, bufsize++) {
				if (unpacked[i] == '\r')
					break;

				buf[bufsize] = unpacked[i];
			}

			/*
			 * It isn't clear whether extension sequences
			 * (2 septets) must be wholly present in the page
			 * and not broken over multiple pages.  The behavior
			 * is probably the same as SMS, but we don't make
			 * the check here since the specification isn't clear
			 */
		} else {
			int num_ucs2_chars = (82 - taken) >> 1;
			int i = taken;
			int max_offset = taken + num_ucs2_chars * 2;

			/*
			 * It is completely unclear how UCS2 chars are handled
			 * especially across pages or when the UDH is present.
			 * For now do the best we can.
			 */
			if (iso639) {
				i += 2;
				num_ucs2_chars -= 1;
			}

			while (i < max_offset) {
				if (ud[i] == 0x00 && ud[i+1] == '\r')
					break;

				buf[bufsize] = ud[i];
				buf[bufsize + 1] = ud[i+1];

				bufsize += 2;
				i += 2;
			}
		}
	}

	if (charset == SMS_CHARSET_7BIT)
		utf8 = convert_gsm_to_utf8(buf, bufsize, NULL, NULL, 0);
	else
		utf8 = g_convert((char *) buf, bufsize, "UTF-8//TRANSLIT",
					"UCS-2BE", NULL, NULL, NULL);

	g_free(buf);
	return utf8;
}

static inline gboolean cbs_is_update_newer(unsigned int n, unsigned int o)
{
	unsigned int old_update = o & 0xf;
	unsigned int new_update = n & 0xf;

	if (new_update == old_update)
		return FALSE;

	/*
	 * Any Update Number eight or less higher (modulo 16) than the last
	 * received Update Number will be considered more recent, and shall be
	 * treated as a new CBS message, provided the mobile has not been
	 * switched off.
	 */
	if (new_update <= ((old_update + 8) % 16))
		return TRUE;

	return FALSE;
}

struct cbs_assembly *cbs_assembly_new(void)
{
	return g_new0(struct cbs_assembly, 1);
}

void cbs_assembly_free(struct cbs_assembly *assembly)
{
	GSList *l;

	for (l = assembly->assembly_list; l; l = l->next) {
		struct cbs_assembly_node *node = l->data;

		g_slist_foreach(node->pages, (GFunc) g_free, 0);
		g_slist_free(node->pages);
		g_free(node);
	}

	g_slist_free(assembly->assembly_list);
	g_slist_free(assembly->recv_plmn);
	g_slist_free(assembly->recv_loc);
	g_slist_free(assembly->recv_cell);

	g_free(assembly);
}

static gint cbs_compare_node_by_gs(gconstpointer a, gconstpointer b)
{
	const struct cbs_assembly_node *node = a;
	unsigned int gs = GPOINTER_TO_UINT(b);

	if (((node->serial >> 14) & 0x3) == gs)
		return 0;

	return 1;
}

static gint cbs_compare_node_by_update(gconstpointer a, gconstpointer b)
{
	const struct cbs_assembly_node *node = a;
	unsigned int serial = GPOINTER_TO_UINT(b);

	if ((serial & (~0xf)) != (node->serial & (~0xf)))
		return 1;

	if (cbs_is_update_newer(node->serial, serial))
		return 1;

	return 0;
}

static gint cbs_compare_recv_by_serial(gconstpointer a, gconstpointer b)
{
	unsigned int old_serial = GPOINTER_TO_UINT(a);
	unsigned int new_serial = GPOINTER_TO_UINT(b);

	if ((old_serial & (~0xf)) == (new_serial & (~0xf)))
		return 0;

	return 1;
}

static void cbs_assembly_expire(struct cbs_assembly *assembly,
				GCompareFunc func, gconstpointer *userdata)
{
	GSList *l;
	GSList *prev;
	GSList *tmp;

	/*
	 * Take care of the case where several updates are being
	 * reassembled at the same time. If the newer one is assembled
	 * first, then the subsequent old update is discarded, make
	 * sure that we're also discarding the assembly node for the
	 * partially assembled ones
	 */
	prev = NULL;
	l = assembly->assembly_list;

	while (l) {
		struct cbs_assembly_node *node = l->data;

		if (func(node, userdata) != 0) {
			prev = l;
			l = l->next;
			continue;
		}

		if (prev)
			prev->next = l->next;
		else
			assembly->assembly_list = l->next;

		g_slist_foreach(node->pages, (GFunc) g_free, NULL);
		g_slist_free(node->pages);
		g_free(node->pages);
		tmp = l;
		l = l->next;
		g_slist_free_1(tmp);
	}
}

void cbs_assembly_location_changed(struct cbs_assembly *assembly, gboolean plmn,
					gboolean lac, gboolean ci)
{
	/*
	 * Location Area wide (in GSM) (which means that a CBS message with the
	 * same Message Code and Update Number may or may not be "new" in the
	 * next cell according to whether the next cell is in the same Location
	 * Area as the current cell), or
	 *
	 * Service Area Wide (in UMTS) (which means that a CBS message with the
	 * same Message Code and Update Number may or may not be "new" in the
	 * next cell according to whether the next cell is in the same Service
	 * Area as the current cell)
	 *
	 * NOTE 4: According to 3GPP TS 23.003 [2] a Service Area consists of
	 * one cell only.
	 */

	if (plmn) {
		lac = TRUE;
		g_slist_free(assembly->recv_plmn);
		assembly->recv_plmn = NULL;

		cbs_assembly_expire(assembly, cbs_compare_node_by_gs,
				GUINT_TO_POINTER(CBS_GEO_SCOPE_PLMN));
	}

	if (lac) {
		/* If LAC changed, then cell id has changed */
		ci = TRUE;
		g_slist_free(assembly->recv_loc);
		assembly->recv_loc = NULL;

		cbs_assembly_expire(assembly, cbs_compare_node_by_gs,
				GUINT_TO_POINTER(CBS_GEO_SCOPE_SERVICE_AREA));
	}

	if (ci) {
		g_slist_free(assembly->recv_cell);
		assembly->recv_cell = NULL;
		cbs_assembly_expire(assembly, cbs_compare_node_by_gs,
				GUINT_TO_POINTER(CBS_GEO_SCOPE_CELL_IMMEDIATE));
		cbs_assembly_expire(assembly, cbs_compare_node_by_gs,
				GUINT_TO_POINTER(CBS_GEO_SCOPE_CELL_NORMAL));
	}
}

GSList *cbs_assembly_add_page(struct cbs_assembly *assembly,
				const struct cbs *cbs)
{
	struct cbs *newcbs;
	struct cbs_assembly_node *node;
	GSList *completed;
	unsigned int new_serial;
	GSList **recv;
	GSList *l;
	GSList *prev;
	int position;

	new_serial = cbs->gs << 14;
	new_serial |= cbs->message_code << 4;
	new_serial |= cbs->update_number;
	new_serial |= cbs->message_identifier << 16;

	if (cbs->gs == CBS_GEO_SCOPE_PLMN)
		recv = &assembly->recv_plmn;
	else if (cbs->gs == CBS_GEO_SCOPE_SERVICE_AREA)
		recv = &assembly->recv_loc;
	else
		recv = &assembly->recv_cell;

	/* Have we seen this message before? */
	l = g_slist_find_custom(*recv, GUINT_TO_POINTER(new_serial),
				cbs_compare_recv_by_serial);

	/* If we have, is the message newer? */
	if (l && !cbs_is_update_newer(new_serial, GPOINTER_TO_UINT(l->data)))
		return NULL;

	/* Easy case first, page 1 of 1 */
	if (cbs->max_pages == 1 && cbs->page == 1) {
		if (l)
			l->data = GUINT_TO_POINTER(new_serial);
		else
			*recv = g_slist_prepend(*recv,
						GUINT_TO_POINTER(new_serial));

		newcbs = g_new(struct cbs, 1);
		memcpy(newcbs, cbs, sizeof(struct cbs));
		completed = g_slist_append(NULL, newcbs);

		return completed;
	}

	prev = NULL;
	position = 0;

	for (l = assembly->assembly_list; l; prev = l, l = l->next) {
		int j;
		node = l->data;

		if (new_serial != node->serial)
			continue;

		if (node->bitmap & (1 << cbs->page))
			return NULL;

		for (j = 1; j < cbs->page; j++)
			if (node->bitmap & (1 << j))
				position += 1;

		goto out;
	}

	node = g_new0(struct cbs_assembly_node, 1);
	node->serial = new_serial;

	assembly->assembly_list = g_slist_prepend(assembly->assembly_list,
							node);

	prev = NULL;
	l = assembly->assembly_list;
	position = 0;

out:
	newcbs = g_new(struct cbs, 1);
	memcpy(newcbs, cbs, sizeof(struct cbs));
	node->pages = g_slist_insert(node->pages, newcbs, position);
	node->bitmap |= 1 << cbs->page;

	if (g_slist_length(node->pages) < cbs->max_pages)
		return NULL;

	completed = node->pages;

	if (prev)
		prev->next = l->next;
	else
		assembly->assembly_list = l->next;

	g_free(node);
	g_slist_free_1(l);

	cbs_assembly_expire(assembly, cbs_compare_node_by_update,
				GUINT_TO_POINTER(new_serial));
	*recv = g_slist_prepend(*recv, GUINT_TO_POINTER(new_serial));

	return completed;
}

static inline int skip_to_next_field(const char *str, int pos, int len)
{
	if (pos < len && str[pos] == ',')
		pos += 1;

	while (pos < len && str[pos] == ' ')
		pos += 1;

	return pos;
}

static gboolean next_range(const char *str, int *offset, gint *min, gint *max)
{
	int pos;
	int end;
	int len;
	int low = 0;
	int high = 0;

	len = strlen(str);

	pos = *offset;

	while (pos < len && str[pos] == ' ')
		pos += 1;

	end = pos;

	while (str[end] >= '0' && str[end] <= '9') {
		low = low * 10 + (int)(str[end] - '0');
		end += 1;
	}

	if (pos == end)
		return FALSE;

	if (str[end] != '-') {
		high = low;
		goto out;
	}

	pos = end = end + 1;

	while (str[end] >= '0' && str[end] <= '9') {
		high = high * 10 + (int)(str[end] - '0');
		end += 1;
	}

	if (pos == end)
		return FALSE;

out:
	*offset = skip_to_next_field(str, end, len);

	if (min)
		*min = low;

	if (max)
		*max = high;

	return TRUE;
}

GSList *cbs_optimize_ranges(GSList *ranges)
{
	struct cbs_topic_range *range;
	unsigned char bitmap[125];
	GSList *l;
	unsigned short i;
	GSList *ret = NULL;

	memset(bitmap, 0, sizeof(bitmap));

	for (l = ranges; l; l = l->next) {
		range = l->data;

		for (i = range->min; i <= range->max; i++) {
			int byte_offset = i / 8;
			int bit = i % 8;

			bitmap[byte_offset] |= 1 << bit;
		}
	}

	range = NULL;

	for (i = 0; i <= 999; i++) {
		int byte_offset = i / 8;
		int bit = i % 8;

		if (is_bit_set(bitmap[byte_offset], bit) == FALSE) {
			if (range) {
				ret = g_slist_prepend(ret, range);
				range = NULL;
			}

			continue;
		}

		if (range) {
			range->max = i;
			continue;
		}

		range = g_new0(struct cbs_topic_range, 1);
		range->min = i;
		range->max = i;
	}

	if (range != NULL)
		ret = g_slist_prepend(ret, range);

	ret = g_slist_reverse(ret);

	return ret;
}

GSList *cbs_extract_topic_ranges(const char *ranges)
{
	int min;
	int max;
	int offset = 0;
	GSList *ret = NULL;
	GSList *tmp;

	while (next_range(ranges, &offset, &min, &max) == TRUE) {
		if (min < 0 || min > 999)
			return NULL;

		if (max < 0 || max > 999)
			return NULL;

		if (max < min)
			return NULL;
	}

	if (ranges[offset] != '\0')
		return NULL;

	offset = 0;

	while (next_range(ranges, &offset, &min, &max) == TRUE) {
		struct cbs_topic_range *range = g_new0(struct cbs_topic_range, 1);

		range->min = min;
		range->max = max;

		ret = g_slist_prepend(ret, range);
	}

	tmp = cbs_optimize_ranges(ret);
	g_slist_foreach(ret, (GFunc) g_free, NULL);
	g_slist_free(ret);

	return tmp;
}

static inline int element_length(unsigned short element)
{
	if (element <= 9)
		return 1;

	if (element <= 99)
		return 2;

	if (element <= 999)
		return 3;

	if (element <= 9999)
		return 4;

	return 5;
}

static inline int range_length(struct cbs_topic_range *range)
{
	if (range->min == range->max)
		return element_length(range->min);

	return element_length(range->min) + element_length(range->max) + 1;
}

char *cbs_topic_ranges_to_string(GSList *ranges)
{
	int len = 0;
	int nelem = 0;
	struct cbs_topic_range *range;
	GSList *l;
	char *ret;

	if (ranges == NULL)
		return g_new0(char, 1);

	for (l = ranges; l; l = l->next) {
		range = l->data;

		len += range_length(range);
		nelem += 1;
	}

	/* Space for ranges, commas and terminator null */
	ret = g_new(char, len + nelem);

	len = 0;

	for (l = ranges; l; l = l->next) {
		range = l->data;

		if (range->min != range->max)
			len += sprintf(ret + len, "%hu-%hu",
					range->min, range->max);
		else
			len += sprintf(ret + len, "%hu", range->min);

		if (l->next != NULL)
			ret[len++] = ',';
	}

	return ret;
}

static gint cbs_topic_compare(gconstpointer a, gconstpointer b)
{
	const struct cbs_topic_range *range = a;
	unsigned short topic = GPOINTER_TO_UINT(b);

	if (topic >= range->min && topic <= range->max)
		return 0;

	return 1;
}

gboolean cbs_topic_in_range(unsigned int topic, GSList *ranges)
{
	if (ranges == NULL)
		return FALSE;

	return g_slist_find_custom(ranges, GUINT_TO_POINTER(topic),
					cbs_topic_compare) != NULL;
}

char *ussd_decode(int dcs, int len, const unsigned char *data)
{
	gboolean udhi;
	enum sms_charset charset;
	gboolean compressed;
	gboolean iso639;
	char *utf8;

	if (!cbs_dcs_decode(dcs, &udhi, NULL, &charset,
				&compressed, NULL, &iso639))
		return NULL;

	if (udhi || compressed || iso639)
		return NULL;

	switch (charset) {
	case SMS_CHARSET_7BIT:
	{
		long written;
		unsigned char *unpacked = unpack_7bit(data, len, 0, TRUE, 0,
							&written, 0);
		if (unpacked == NULL)
			return NULL;

		utf8 = convert_gsm_to_utf8(unpacked, written, NULL, NULL, 0);
		g_free(unpacked);

		break;
	}
	case SMS_CHARSET_8BIT:
		utf8 = convert_gsm_to_utf8(data, len, NULL, NULL, 0);
		break;
	case SMS_CHARSET_UCS2:
		utf8 = g_convert((const gchar *) data, len,
					"UTF-8//TRANSLIT", "UCS-2BE",
					NULL, NULL, NULL);
		break;
	default:
		utf8 = NULL;
	}

	return utf8;
}

gboolean ussd_encode(const char *str, long *items_written, unsigned char *pdu)
{
	unsigned char *converted = NULL;
	long written;
	long num_packed;

	if (pdu == NULL)
		return FALSE;

	converted = convert_utf8_to_gsm(str, -1, NULL, &written, 0);
	if (converted == NULL || written > 182) {
		g_free(converted);
		return FALSE;
	}

	pack_7bit_own_buf(converted, written, 0, TRUE, &num_packed, 0, pdu);
	g_free(converted);

	if (num_packed < 1)
		return FALSE;

	if (items_written)
		*items_written = num_packed;

	return TRUE;
}
