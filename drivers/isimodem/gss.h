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

#ifndef __ISIMODEM_GSS_H
#define __ISIMODEM_GSS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PN_GSS				0x32
#define GSS_TIMEOUT			5

enum gss_message_id {
	GSS_CS_SERVICE_REQ =		0x00,
	GSS_CS_SERVICE_RESP =		0x01,
	GSS_CS_SERVICE_FAIL_RESP =	0x02,
};

enum gss_subblock {
	GSS_RAT_INFO =			0x0B,
};

enum gss_selection_mode {
	GSS_DUAL_RAT =			0x00,
	GSS_GSM_RAT =			0x01,
	GSS_UMTS_RAT =			0x02,
};

enum gss_operation {
	GSS_SELECTED_RAT_WRITE =	0x0E,
	GSS_SELECTED_RAT_READ =		0x9C,
};

#ifdef __cplusplus
};
#endif

#endif /* !__ISIMODEM_GSS_H */
