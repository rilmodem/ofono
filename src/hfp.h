/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
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

/* HFP AG supported features bitmap. Bluetooth HFP 1.6 spec page 88 */
enum hfp_ag_feature {
	HFP_AG_FEATURE_3WAY =			0x1,
	HFP_AG_FEATURE_ECNR =			0x2,
	HFP_AG_FEATURE_VOICE_RECOG =		0x4,
	HFP_AG_FEATURE_IN_BAND_RING_TONE =	0x8,
	HFP_AG_FEATURE_ATTACH_VOICE_TAG =	0x10,
	HFP_AG_FEATURE_REJECT_CALL =		0x20,
	HFP_AG_FEATURE_ENHANCED_CALL_STATUS =	0x40,
	HFP_AG_FEATURE_ENHANCED_CALL_CONTROL =	0x80,
	HFP_AG_FEATURE_EXTENDED_RES_CODE =	0x100,
	HFP_AG_FEATURE_CODEC_NEGOTIATION =	0x200,
};

/* HFP HF supported features bitmap. Bluetooth HFP 1.6 spec page 88 */
enum hfp_hf_feature {
	HFP_HF_FEATURE_ECNR =			0x1,
	HFP_HF_FEATURE_3WAY =			0x2,
	HFP_HF_FEATURE_CLIP =			0x4,
	HFP_HF_FEATURE_VOICE_RECOGNITION =	0x8,
	HFP_HF_FEATURE_REMOTE_VOLUME_CONTROL =	0x10,
	HFP_HF_FEATURE_ENHANCED_CALL_STATUS =	0x20,
	HFP_HF_FEATURE_ENHANCED_CALL_CONTROL =	0x40,
	HFP_HF_FEATURE_CODEC_NEGOTIATION =	0x80,
};

/* Supported agent codecs */
enum hfp_codec {
	HFP_CODEC_CVSD = 0x01,
	HFP_CODEC_MSBC = 0x02,
};

enum hfp_version {
	HFP_VERSION_1_5 =	0x0105,
	HFP_VERSION_1_6 =	0x0106,
	HFP_VERSION_LATEST =	HFP_VERSION_1_6,
};
