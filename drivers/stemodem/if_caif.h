/*
 * Copyright (C) ST-Ericsson AB 2009
 * Author:	Sjur Brendeland/ sjur.brandeland@stericsson.com
 * License terms: GNU General Public License (GPL) version 2
 */

#ifndef IF_CAIF_H_
#define IF_CAIF_H_
#include <linux/sockios.h>
#include <linux/types.h>
#include <linux/socket.h>
/**
 * enum sioc_caif -	SOCKIO for creating new CAIF Net Devices.
 * @SIOCCAIFNETNEW:	Used to create a new instance of the CAIF IP Interface.
 *			struct ifreq containing struct ifcaif_param are used
 *			as parameters. ifr_name must be filled in.
 * @SIOCCAIFNETCHANGE:	As above, but changes a disconnected CAIF IP Inteface.
 * @SIOCCAIFNETREMOVE:	Removes a CAIF IP Interface.
 *
 * CAIF IP Interface can be created, changed and deleted,
 * by this enum. In addition standard Socket IO Controls (SIGIOC*)
 * can be used to manage standard IP Interface parameters.
 * The struct ifreq are used to carry parameters.
 */
enum sioc_caif {
	SIOCCAIFNETNEW = SIOCPROTOPRIVATE,
	SIOCCAIFNETCHANGE,
	SIOCCAIFNETREMOVE
};


/**
 * struct ifcaif_param - Parameters for creating CAIF Network Interface.
 *
 * When using SIOCCAIFNETNEW to create a CAIF IP interface, this structure
 * is used for configuration data.
 * The attribute ifr_ifru.ifru_data in struct struct ifreq must be set
 * point at an instance of struct ifcaif_param.
 *
 * @ipv4_connid:  Connection ID for IPv4 PDP Context.
 * @ipv6_connid:  Connection ID for IPv6 PDP Context.
 * @loop:	  If different from zero, device is doing loopback
 */
struct ifcaif_param {
	__u32			ipv4_connid;
	__u32			ipv6_connid;
	__u8			loop;
};

/**
 * enum ifla_caif
 * When using RT Netlink to create, destroy or configure a CAIF IP interface,
 * enum ifla_caif is used to specify the configuration attributes.
 *
 * @IFLA_CAIF_IPV4_CONNID:  Connection ID for IPv4 PDP Context.
 *			    The type of attribute is NLA_U32.
 * @IFLA_CAIF_IPV6_CONNID:  Connection ID for IPv6 PDP Context.
 *			    The type of attribute is NLA_U32.
 * @IFLA_CAIF_LOOPBACK:	    If different from zero, device is doing loopback
 *			    The type of attribute is NLA_U8.
 */
enum ifla_caif {
	IFLA_CAIF_IPV4_CONNID,
	IFLA_CAIF_IPV6_CONNID,
	IFLA_CAIF_LOOPBACK,
	__IFLA_CAIF_MAX
};
#define	IFLA_CAIF_MAX (__IFLA_CAIF_MAX-1)

#endif /*IF_CAIF_H_*/
