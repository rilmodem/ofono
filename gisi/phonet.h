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

#ifndef NETPHONET_PHONET_H
#define NETPHONET_PHONET_H

#include <sys/types.h>

#include <sys/socket.h>
#ifndef AF_PHONET
#define AF_PHONET 35
#define PF_PHONET AF_PHONET
#endif

#define PN_PROTO_TRANSPORT	0
#define PN_PROTO_PHONET		1
#define PN_PROTO_PIPE		2

#define SOL_PNPIPE		275

#define PNPIPE_ENCAP		1
#define PNPIPE_IFINDEX		2

#define PNPIPE_ENCAP_NONE	0
#define PNPIPE_ENCAP_IP		1

#define SIOCPNGETOBJECT		(SIOCPROTOPRIVATE + 0)
#define SIOCPNADDRESOURCE	(SIOCPROTOPRIVATE + 14)
#define SIOCPNDELRESOURCE	(SIOCPROTOPRIVATE + 15)

struct sockaddr_pn {
	sa_family_t spn_family;
	uint8_t spn_obj;
	uint8_t spn_dev;
	uint8_t spn_resource;
	uint8_t __pad[sizeof(struct sockaddr) - (sizeof(sa_family_t) + 3)];
} __attribute__ ((packed));

#include <linux/rtnetlink.h>
#ifndef RTNLGRP_PHONET_IFADDR
#define RTNLGRP_PHONET_IFADDR 21
#endif

#endif
