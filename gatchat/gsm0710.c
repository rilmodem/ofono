/*
 *
 *  AT chat library with GLib integration
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009  Trolltech ASA.
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

#include "gsm0710.h"

static const unsigned char crc_table[256] = {
	0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75,
	0x0E, 0x9F, 0xED, 0x7C, 0x09, 0x98, 0xEA, 0x7B,
	0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69,
	0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67,
	0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D,
	0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43,
	0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0, 0x51,
	0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,
	0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05,
	0x7E, 0xEF, 0x9D, 0x0C, 0x79, 0xE8, 0x9A, 0x0B,
	0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19,
	0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17,
	0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC, 0x3D,
	0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33,
	0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21,
	0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,
	0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95,
	0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A, 0x9B,
	0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89,
	0xF2, 0x63, 0x11, 0x80, 0xF5, 0x64, 0x16, 0x87,
	0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C, 0xAD,
	0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
	0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1,
	0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,
	0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5,
	0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB,
	0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9,
	0x82, 0x13, 0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7,
	0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C, 0xDD,
	0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3,
	0xB4, 0x25, 0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1,
	0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF
};

static inline guint8 gsm0710_crc(const guint8 *data, int len)
{
	guint8 crc = 0xFF;
	int i;

	for (i = 0; i < len; i++)
		crc = crc_table[crc ^ data[i]];

	return crc;
}

static inline guint8 gsm0710_fcs(const guint8 *data, int len)
{
	return 0xff - gsm0710_crc(data, len);
}

static inline gboolean gsm0710_check_fcs(const guint8 *data, int len,
						guint8 cfcs)
{
	guint8 fcs = gsm0710_crc(data, len);

	fcs = crc_table[fcs ^ cfcs];

	if (fcs == 0xcf)
		return TRUE;

	return FALSE;
}

int gsm0710_advanced_extract_frame(guint8 *buf, int len,
					guint8 *out_dlc, guint8 *out_control,
					guint8 **out_frame, int *out_len)
{
	int posn = 0;
	int posn2;
	int framelen;
	guint8 dlc;
	guint8 control;

	while (posn < len) {
		if (buf[posn] != 0x7E) {
			posn += 1;
			continue;
		}

		/* Skip additional 0x7E bytes between frames */
		while ((posn + 1) < len && buf[posn + 1] == 0x7E)
			posn += 1;

		/* Search for the end of the packet (the next 0x7E byte) */
		framelen = posn + 1;
		while (framelen < len && buf[framelen] != 0x7E)
			framelen += 1;

		if (framelen >= len)
			break;

		if (framelen < 4) {
			posn = framelen;
			continue;
		}

		/* Undo control byte quoting in the packet */
		posn2 = 0;
		++posn;
		while (posn < framelen) {
			if (buf[posn] == 0x7D) {
				++posn;

				if (posn >= framelen)
					break;

				buf[posn2++] = buf[posn++] ^ 0x20;
			} else {
				buf[posn2++] = buf[posn++];
			}
		}

		/* Validate the checksum on the packet header */
		if (!gsm0710_check_fcs(buf, 2, buf[posn2 - 1]))
			continue;

		/* Decode and dispatch the packet */
		dlc = (buf[0] >> 2) & 0x3F;
		control = buf[1] & 0xEF; /* Strip "PF" bit */

		if (out_frame)
			*out_frame = buf + 2;

		if (out_len)
			*out_len = posn2 - 3;

		if (out_dlc)
			*out_dlc = dlc;

		if (out_control)
			*out_control = control;

		break;
	}

	return posn;
}

int gsm0710_advanced_fill_frame(guint8 *frame, guint8 dlc, guint8 type,
					const guint8 *data, int len)
{
	int temp, crc;
	int size;

	frame[0] = 0x7E;
	frame[1] = ((dlc << 2) | 0x03);
	frame[2] = type;

	crc = gsm0710_fcs(frame + 1, 2);

	/* The Address field might need to be escaped if this is a response
	 * frame
	 */

	/* Need to quote the type field now that crc has been computed */
	if (type == 0x7E || type == 0x7D) {
		frame[2] = 0x7D;
		frame[3] = (type ^ 0x20);
		size = 4;
	} else {
		size = 3;
	}

	while (len > 0) {
		temp = *data++ & 0xFF;
		--len;

		if (temp != 0x7E && temp != 0x7D) {
			frame[size++] = temp;
		} else {
			frame[size++] = 0x7D;
			frame[size++] = (temp ^ 0x20);
		}
	}

	if (crc != 0x7E && crc != 0x7D) {
		frame[size++] = crc;
	} else {
		frame[size++] = 0x7D;
		frame[size++] = (crc ^ 0x20);
	}

	frame[size++] = 0x7E;

	return size;
}

int gsm0710_basic_extract_frame(guint8 *buf, int len,
					guint8 *out_dlc, guint8 *out_control,
					guint8 **out_frame, int *out_len)
{
	int posn = 0;
	int framelen;
	int header_size;
	guint8 fcs;
	guint8 dlc;
	guint8 type;

	while (posn < len) {
		if (buf[posn] != 0xF9) {
			posn += 1;
			continue;
		}

		/* Skip additional 0xF9 bytes between frames */
		while ((posn + 1) < len && buf[posn + 1] == 0xF9)
			posn += 1;

		/* We need at least 4 bytes for the flag + header */
		if ((posn + 4) > len)
			break;

		/* The low bit of the second byte should be 1,
		   which indicates a short channel number.  According to
		   27.010 Section 5.2.3, if this is not true, then
		   the frame is invalid and should be discarded
		*/
		if ((buf[posn + 1] & 0x01) == 0) {
			++posn;
			continue;
		}

		/* Get the packet length and validate it */
		framelen = buf[posn + 3] >> 1;

		if ((buf[posn + 3] & 0x01) != 0) {
			/* Single-byte length indication */
			header_size = 3;
		} else {
			/* Double-byte length indication */
			if ((posn + 5) > len)
				break;

			framelen |= buf[posn + 4] << 7;
			header_size = 4;
		}

		/* Total size of the packet is the flag + 3 or 4 byte header
		 * Address Control Length followed by Information and FCS.
		 * However, we must check the presence of the end flag
		 * according to 27.010 Section 5.2.3
		 */
		if ((posn + header_size + 3 + framelen) > len)
			break;

		fcs = buf[posn + 1 + header_size + framelen];

		/*
		 * The end flag is not guaranteed to be only ours
		 * according to 27.010 Section 5.2.6.1:
		 * "The closing flag may also be the opening flag of the
		 * following frame", thus we do not consume it in the following
		 * stages
		 */

		/*
		 * If FCS is invalid, discard the packet in accordance to
		 * Section 5.2.3 of 27.010
		 */
		if (!gsm0710_check_fcs(buf + posn + 1, header_size, fcs)) {
			posn += header_size + framelen + 2;
			continue;
		}

		if (buf[posn + header_size + framelen + 2] != 0xF9) {
			posn += header_size + framelen + 2;
			continue;
		}

		/* Get the channel number and packet type from the header */
		dlc = buf[posn + 1] >> 2;
		type = buf[posn + 2] & 0xEF;	/* Strip "PF" bit */

		if (out_frame)
			*out_frame = buf + posn + 1 + header_size;

		if (out_len)
			*out_len = framelen;

		if (out_dlc)
			*out_dlc = dlc;

		if (out_control)
			*out_control = type;

		posn += header_size + framelen + 2;

		break;
	}

	return posn;
}

int gsm0710_basic_fill_frame(guint8 *frame, guint8 dlc, guint8 type,
				const guint8 *data, int len)
{
	int size;
	int header_size;

	frame[0] = 0xF9;
	frame[1] = ((dlc << 2) | 0x03);
	frame[2] = type;

	if (len <= 127) {
		frame[3] = ((len << 1) | 0x01);
		header_size = 4;
	} else {
		frame[3] = (len << 1);
		frame[4] = (len >> 7);
		header_size = 5;
	}

	size = header_size;

	if (len > 0) {
		memcpy(frame + header_size, data, len);
		size += len;
	}

	/* Note: GSM 07.10 says that the CRC is only computed over the header */
	frame[size++] = gsm0710_fcs(frame + 1, header_size - 1);
	frame[size++] = 0xF9;

	return size;
}
