/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __GISI_COMMON_H
#define __GISI_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#define PN_COMMGR				0x10
#define PN_NAMESERVICE				0xDB
#define PN_FIREWALL				0x43
#define COMMON_TIMEOUT				5

enum message_id {
	PNS_NAME_ADD_REQ =			0x05,
	PNS_NAME_REMOVE_REQ =			0x07,
	PNS_SUBSCRIBED_RESOURCES_IND =		0x10,
	PNS_SUBSCRIBED_RESOURCES_EXTEND_IND =	0x12,
	COMM_ISI_VERSION_GET_REQ =		0x12,
	COMM_ISI_VERSION_GET_RESP =		0x13,
	COMM_ISA_ENTITY_NOT_REACHABLE_RESP =	0x14,
	COMM_SERVICE_NOT_AUTHENTICATED_RESP =	0x17,
	COMMON_MESSAGE =			0xF0,
};

enum GIsiPhonetDevice {
	PN_DEV_PC =	0x10,	/* PC Suite */
	PN_DEV_HOST =	0x00,	/* Host modem */
	PN_DEV_MODEM =	0x60,	/* Modem */
	PN_DEV_SOS =	0x6C,	/* Symbian or Linux */
};

enum GIsiMessageType {
	GISI_MESSAGE_TYPE_REQ,
	GISI_MESSAGE_TYPE_IND,
	GISI_MESSAGE_TYPE_NTF,
	GISI_MESSAGE_TYPE_RESP,
	GISI_MESSAGE_TYPE_COMMON,	/* ISI version, namely */
};

#ifdef __cplusplus
}
#endif

#endif /* __GISI_COMMON_H */
