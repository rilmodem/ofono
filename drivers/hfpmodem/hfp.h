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
#ifndef __BLUETOOTH_H__
#define __BLUETOOTH_H__

#include <drivers/atmodem/atutil.h>

/* AG supported features bitmap. Bluetooth HFP 1.5 spec page 77 */
#define AG_FEATURE_3WAY 0x1
#define AG_FEATURE_ECNR 0x2
#define AG_FEATURE_VOICE_RECOG 0x4
#define AG_FEATURE_IN_BAND_RING_TONE 0x8
#define AG_FEATURE_ATTACH_VOICE_TAG 0x10
#define AG_FEATURE_REJECT_CALL 0x20
#define AG_FEATURE_ENHANCED_CALL_STATUS 0x40
#define AG_FEATURE_ENHANCED_CALL_CONTROL 0x80
#define AG_FEATURE_EXTENDED_RES_CODE 0x100

struct hfp_data {
	GAtChat *chat;
	int ag_features;
	char **cind_names;
	int *cind_values;
	int cind_length;
};

extern void hfp_voicecall_init();
extern void hfp_voicecall_exit();

#endif
