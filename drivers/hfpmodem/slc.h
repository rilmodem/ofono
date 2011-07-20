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

#define AG_CHLD_0	0x01
#define AG_CHLD_1	0x02
#define AG_CHLD_1x	0x04
#define AG_CHLD_2	0x08
#define AG_CHLD_2x	0x10
#define AG_CHLD_3	0x20
#define AG_CHLD_4	0x40

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

typedef void (*hfp_slc_cb_t)(void *userdata);

struct hfp_slc_info {
	GAtChat *chat;
	unsigned int ag_features;
	unsigned int ag_mpty_features;
	unsigned int hf_features;
	unsigned char cind_pos[HFP_INDICATOR_LAST];
	unsigned int cind_val[HFP_INDICATOR_LAST];
};

void hfp_slc_info_init(struct hfp_slc_info *info);
void hfp_slc_info_free(struct hfp_slc_info *info);

void hfp_slc_establish(struct hfp_slc_info *info, hfp_slc_cb_t connect_cb,
				hfp_slc_cb_t failed_cb, void *userdata);
