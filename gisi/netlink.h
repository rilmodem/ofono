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

#ifndef __GISI_PN_NETLINK_H
#define __GISI_PN_NETLINK_H

#ifdef __cplusplus
extern "C" {
#endif

struct _GIsiPhonetNetlink;
typedef struct _GIsiPhonetNetlink GIsiPhonetNetlink;

typedef enum {
	PN_LINK_REMOVED,
	PN_LINK_DOWN,
	PN_LINK_UP
} GIsiPhonetLinkState;

typedef enum {
	PN_DEV_PC = 0x10,	/* PC Suite */
	PN_DEV_HOST = 0x00,	/* Modem */
	PN_DEV_SOS = 0x6C,	/* Symbian or Linux */
} GIsiPhonetDevice;

typedef void (*GIsiPhonetNetlinkFunc)(GIsiModem *modem, GIsiPhonetLinkState st,
					char const *iface, void *data);

GIsiPhonetNetlink *g_isi_pn_netlink_by_modem(GIsiModem *modem);

GIsiPhonetNetlink *g_isi_pn_netlink_start(GIsiModem *idx,
						GIsiPhonetNetlinkFunc cb,
						void *data);

void g_isi_pn_netlink_stop(GIsiPhonetNetlink *self);
int g_isi_pn_netlink_set_address(GIsiModem *modem, uint8_t local);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_PN_NETLINK_H */
