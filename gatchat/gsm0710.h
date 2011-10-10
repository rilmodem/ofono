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

#ifndef __GSM0710_H
#define __GSM0710_H

#ifdef __cplusplus
extern "C" {
#endif

/* Frame types and subtypes */
#define GSM0710_OPEN_CHANNEL		0x3F
#define GSM0710_CLOSE_CHANNEL		0x53
#define GSM0710_DATA			0xEF
#define GSM0710_DATA_ALT		0x03
#define GSM0710_STATUS_SET		0xE3
#define GSM0710_STATUS_ACK		0xE1

int gsm0710_basic_extract_frame(guint8 *data, int len,
					guint8 *out_dlc, guint8 *out_type,
					guint8 **frame, int *out_len);

int gsm0710_basic_fill_frame(guint8 *frame, guint8 dlc, guint8 type,
				const guint8 *data, int len);

int gsm0710_advanced_extract_frame(guint8 *data, int len,
					guint8 *out_dlc, guint8 *out_type,
					guint8 **frame, int *out_len);

int gsm0710_advanced_fill_frame(guint8 *frame, guint8 dlc, guint8 type,
					const guint8 *data, int len);
#ifdef __cplusplus
};
#endif

#endif
