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

#include <stdint.h>
#include <gisi/modem.h>

#ifndef __GPHONET_NETLINK_H
#define __GPHONET_NETLINK_H

#ifdef __cplusplus
extern "C" {
#endif

struct _GPhonetNetlink;
typedef struct _GPhonetNetlink GPhonetNetlink;

typedef enum {
	PN_LINK_REMOVED,
	PN_LINK_DOWN,
	PN_LINK_UP
} GPhonetLinkState;

enum {
	PN_DEV_PC = 0x10,	/* PC Suite */
	PN_DEV_HOST = 0x00,	/* Modem */
	PN_DEV_SOS = 0x6C,	/* Symbian or Linux */
};

typedef void (*GPhonetNetlinkFunc)(GIsiModem *idx,
			GPhonetLinkState st,
			char const *iface,
			void *data);

GPhonetNetlink *g_pn_netlink_by_modem(GIsiModem *idx);

GPhonetNetlink *g_pn_netlink_start(GIsiModem *idx,
			GPhonetNetlinkFunc callback,
			void *data);

void g_pn_netlink_stop(GPhonetNetlink *self);

int g_pn_netlink_set_address(GIsiModem *, uint8_t local);

#ifdef __cplusplus
}
#endif

#endif /* __GPHONET_NETLINK_H */
