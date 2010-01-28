/* linux/caif_socket.h
 * CAIF Definitions for CAIF socket and network layer
 * Copyright (C) ST-Ericsson AB 2009
 * Author:	 Sjur Brendeland/ sjur.brandeland@stericsson.com
 * License terms: GNU General Public License (GPL) version 2
 */

#ifndef _LINUX_CAIF_SOCKET_H
#define _LINUX_CAIF_SOCKET_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/socket.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
#ifndef AF_CAIF
#define AF_CAIF    37          /* CAIF Socket Address Family */
#endif
#ifndef PF_CAIF
#define PF_CAIF    AF_CAIF      /* CAIF Socket Protocol Family */
#endif
#ifndef SOL_CAIF
#define SOL_CAIF   278		/* CAIF Socket Option Level */
#endif

/**
 * enum caif_link_selector -   Physical Link Selection.
 * @CAIF_LINK_HIGH_BANDW:	Default physical interface for high-bandwidth
 *				traffic.
 * @CAIF_LINK_LOW_LATENCY:	Default physical interface for low-latency
 *				traffic.
 */
enum caif_link_selector {
	CAIF_LINK_HIGH_BANDW,
	CAIF_LINK_LOW_LATENCY
};

/**
 * enum caif_protocol_type  -	Types of CAIF protocols in the CAIF Stack.
 * @CAIFPROTO_AT:		Classic AT channel.
 * @CAIFPROTO_DATAGRAM:		Datagram channel.
 * @CAIFPROTO_DATAGRAM_LOOP:	Datagram loopback channel, used for testing.
 * @CAIFPROTO_UTIL:		Utility (Psock) channel.
 * @CAIFPROTO_RFM:		Remote File Manager
 */
enum caif_protocol_type {
	CAIFPROTO_AT,
	CAIFPROTO_DATAGRAM,
	CAIFPROTO_DATAGRAM_LOOP,
	CAIFPROTO_UTIL,
	CAIFPROTO_RFM,
	_CAIFPROTO_MAX
};
#define	CAIFPROTO_MAX _CAIFPROTO_MAX

/**
 * enum caif_at_type - AT Service Endpoint
 * @CAIF_ATTYPE_PLAIN:	     Connects to a plain vanilla AT channel.
 */
enum caif_at_type {
	CAIF_ATTYPE_PLAIN
};

/**
 * struct sockaddr_caif - the sockaddr structure for CAIF sockets.
 * @family:		     Address family number, must be AF_CAIF.
 * @u:			     Union of address data 'switched' by familty.
 * @at:			     Applies when family = CAIFPROTO_AT.
 * @at.type:		     Type of AT link to set up (enum caif_at_type).
 * @util:		     Applies when family = CAIFPROTO_UTIL
 * @util.service:	     Service name.
 * @dgm:		     Applies when family = CAIFPROTO_DATAGRAM
 * @dgm.connection_id:	     Datagram connection id.
 * @dgm.nsapi:		     NSAPI of the PDP-Context.
 * @rfm:		     Applies when family = CAIFPROTO_RFM
 * @rfm.connection_id:       Connection ID for RFM.
 * @rfm.volume:	     	     Volume to mount.
 */
struct sockaddr_caif {
	sa_family_t  family;
	union {
		struct {
			u_int8_t  type;		/* type: enum caif_at_type */
		} at;				/* CAIFPROTO_AT */
		struct {
			char	  service[16];
		} util;				/* CAIFPROTO_UTIL */
		union {
			u_int32_t connection_id;
			u_int8_t  nsapi;
		} dgm;				/* CAIFPROTO_DATAGRAM(_LOOP)*/
		struct {
			u_int32_t connection_id;
			char	  volume[16];
		} rfm;				/* CAIFPROTO_RFM */
	} u;
};

/**
 * struct caif_channel_opt - CAIF channel connect options.
 * @priority:		Priority of the channel (between 0 and 0x1f)
 * @link_selector:	Selector for the physical link.
 *			(see enum caif_phy_preference in caif_config.h)
 * @link_name:		Physical link to use. This is the instance name of the
 *			CAIF Physical Driver.
 */
struct caif_channel_opt {
	u_int16_t  priority;
	u_int16_t  link_selector;
	char	   link_name[16];
};

/**
 * struct caif_param - CAIF parameters.
 * @size:	Length of data
 * @data:	Binary Data Blob
 */
struct caif_param {
	u_int16_t  size;
	u_int8_t   data[256];
};


/** enum caif_socket_opts - CAIF option values for getsockopt and setsockopt
 * @CAIFSO_CHANNEL:		Used to set the connect options on a CAIF
 *				socket. (struct caif_config_opt). This can only
 *				be set before connecting.
 * @CAIFSO_REQ_PARAM:		Used to set the request parameters for a
 *				utility channel. (struct caif_param). This
 *				can only be set before connecting.
 *
 * @CAIFSO_RSP_PARAM:		Gets the request parameters for a utility
 *				channel. (struct caif_param). This can only be
 *				fetched after connecting the socket.
 *
 * @CAIFSO_UTIL_FLOW:		Sets the utility channels flow options.
 *				This can only be set before connecting.
 *				(struct caif_util_modem_flow_opt)
 *
 * @CAIFSO_CONN_ID:		Gets the channel id on a CAIF Channel.
 *				This can only be done after connect.
 *				( u_int32_t)
 *
 * @CAIFSO_NEXT_PAKCET_LEN:	Gets the size of next received packet.
 *				Value is 0 if no packet is available.
 *				This can only be done after connect.
 *				( u_int32_t)
 *
 * @CAIFSO_MAX_PAKCET_LEN:	Gets the maximum packet size for this
 *				connection. ( u_int32_t)
 */
enum caif_socket_opts {
	CAIFSO_CHANNEL_CONFIG	= 127,
	CAIFSO_REQ_PARAM	= 128,
	CAIFSO_RSP_PARAM	= 129,
	CAIFSO_UTIL_FLOW	= 130,
	CAIFSO_CONN_ID		= 131,
	CAIFSO_NEXT_PACKET_LEN	= 132,
	CAIFSO_MAX_PACKET_LEN	= 133,
};

#ifdef __cplusplus
}				/* extern "C" */
#endif
#endif /* _LINUX_CAIF_SOCKET_H */
