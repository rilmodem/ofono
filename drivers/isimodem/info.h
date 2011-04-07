/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __ISIMODEM_INFO_H
#define __ISIMODEM_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

#define PN_PHONE_INFO				0x1B
#define PN_MODEM_INFO				0xC5

#define PN_EPOC_INFO				0x62
#define INFO_TIMEOUT				5

enum info_isi_cause {
	INFO_OK =				0x00,
	INFO_FAIL =				0x01,
	INFO_NO_NUMBER =			0x02,
	INFO_NOT_SUPPORTED =			0x03,
};

enum info_message_id {
	INFO_SERIAL_NUMBER_READ_REQ =		0x00,
	INFO_SERIAL_NUMBER_READ_RESP =		0x01,
	INFO_PP_READ_REQ =			0x02,
	INFO_PP_READ_RESP =			0x03,
	INFO_VERSION_READ_REQ =			0x07,
	INFO_VERSION_READ_RESP =		0x08,
	INFO_PRODUCT_INFO_READ_REQ =		0x15,
	INFO_PRODUCT_INFO_READ_RESP =		0x16,
};

enum info_subblock {
	INFO_SB_MODEMSW_VERSION =		0x00,
	INFO_SB_PRODUCT_INFO_NAME =		0x01,
	INFO_SB_PRODUCT_INFO_MANUFACTURER =	0x07,
	INFO_SB_SN_IMEI_PLAIN =			0x41,
	INFO_SB_SN_IMEI_SV_TO_NET =		0x43,
	INFO_SB_PP =				0x47,
	INFO_SB_MCUSW_VERSION =			0x48,
};

enum info_product_info_type {
	INFO_PRODUCT_NAME =			0x01,
	INFO_PRODUCT_MANUFACTURER =		0x07,
};

enum info_serial_number_type {
	INFO_SN_IMEI_PLAIN =			0x41,
};

enum info_version_type {
	INFO_MCUSW =				0x01,
};

enum info_pp_feature {
	INFO_PP_MAX_PDP_CONTEXTS =		0xCA
};

#ifdef __cplusplus
};
#endif

#endif /* !__ISIMODEM_INFO_H */
