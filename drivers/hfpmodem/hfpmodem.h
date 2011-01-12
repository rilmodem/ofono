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

#include <drivers/atmodem/atutil.h>
#include <ofono/dbus.h>

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

#define HF_FEATURE_ECNR 0x1
#define HF_FEATURE_3WAY 0x2
#define HF_FEATURE_CLIP 0x4
#define HF_FEATURE_VOICE_RECOGNITION 0x8
#define HF_FEATURE_REMOTE_VOLUME_CONTROL 0x10
#define HF_FEATURE_ENHANCED_CALL_STATUS 0x20
#define HF_FEATURE_ENHANCED_CALL_CONTROL 0x40

#define AG_CHLD_0 0x01
#define AG_CHLD_1 0x02
#define AG_CHLD_1x 0x04
#define AG_CHLD_2 0x08
#define AG_CHLD_2x 0x10
#define AG_CHLD_3 0x20
#define AG_CHLD_4 0x40

enum hfp_indicator {
	HFP_INDICATOR_SERVICE = 0,
	HFP_INDICATOR_CALL,
	HFP_INDICATOR_CALLSETUP,
	HFP_INDICATOR_CALLHELD,
	HFP_INDICATOR_SIGNAL,
	HFP_INDICATOR_ROAM,
	HFP_INDICATOR_BATTCHG,
	HFP_INDICATOR_LAST
};

struct hfp_data {
	GAtChat *chat;
	char *handsfree_path;
	DBusMessage *slc_msg;
	unsigned int ag_features;
	unsigned int ag_mpty_features;
	unsigned int hf_features;
	unsigned char cind_pos[HFP_INDICATOR_LAST];
	unsigned int cind_val[HFP_INDICATOR_LAST];
	gboolean agent_registered;
};

extern void hfp_netreg_init(void);
extern void hfp_netreg_exit(void);

extern void hfp_call_volume_init(void);
extern void hfp_call_volume_exit(void);

extern void hfp_voicecall_init(void);
extern void hfp_voicecall_exit(void);
