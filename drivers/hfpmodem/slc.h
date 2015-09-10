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
	unsigned short hf_indicators[20];
	unsigned char num_hf_indicators;
	unsigned int hf_indicator_active_map;
};

void hfp_slc_info_init(struct hfp_slc_info *info, guint16 version);
void hfp_slc_info_free(struct hfp_slc_info *info);

void hfp_slc_establish(struct hfp_slc_info *info, hfp_slc_cb_t connect_cb,
				hfp_slc_cb_t failed_cb, void *userdata);
