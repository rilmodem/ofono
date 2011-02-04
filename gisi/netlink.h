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
#include <gisi/common.h>

#ifndef __GISI_PN_NETLINK_H
#define __GISI_PN_NETLINK_H

#ifdef __cplusplus
extern "C" {
#endif

struct _GIsiPhonetNetlink;
typedef struct _GIsiPhonetNetlink GIsiPhonetNetlink;

enum GIsiPhonetLinkState {
	PN_LINK_REMOVED,
	PN_LINK_DOWN,
	PN_LINK_UP,
};

typedef void (*GIsiPhonetNetlinkFunc)(GIsiModem *modem,
					enum GIsiPhonetLinkState st,
					const char *iface, void *data);

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
