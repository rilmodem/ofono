/*
 *
 *  PPP library with GLib integration
 *
 *  Copyright (C) 2009-2010  Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <arpa/inet.h>
#include <glib.h>

#include "gatutil.h"
#include "gatppp.h"
#include "ppp.h"

#define IPCP_SUPPORTED_CODES	  ((1 << PPPCP_CODE_TYPE_CONFIGURE_REQUEST) | \
				  (1 << PPPCP_CODE_TYPE_CONFIGURE_ACK) | \
				  (1 << PPPCP_CODE_TYPE_CONFIGURE_NAK) | \
				  (1 << PPPCP_CODE_TYPE_CONFIGURE_REJECT) | \
				  (1 << PPPCP_CODE_TYPE_TERMINATE_REQUEST) | \
				  (1 << PPPCP_CODE_TYPE_TERMINATE_ACK) | \
				  (1 << PPPCP_CODE_TYPE_CODE_REJECT))

enum ipcp_option_types {
	IP_ADDRESSES		= 1,
	IP_COMPRESSION_PROTO	= 2,
	IP_ADDRESS		= 3,
	MOBILE_IPV4		= 4,
	PRIMARY_DNS_SERVER	= 129,
	PRIMARY_NBNS_SERVER	= 130,
	SECONDARY_DNS_SERVER	= 131,
	SECONDARY_NBNS_SERVER	= 132,
};

/* We request IP_ADDRESS, PRIMARY/SECONDARY DNS & NBNS */
#define MAX_CONFIG_OPTION_SIZE 5*6

#define REQ_OPTION_IPADDR	0x01
#define REQ_OPTION_DNS1		0x02
#define REQ_OPTION_DNS2		0x04
#define REQ_OPTION_NBNS1	0x08
#define REQ_OPTION_NBNS2	0x10

struct ipcp_data {
	guint8 options[MAX_CONFIG_OPTION_SIZE];
	guint16 options_len;
	guint8 req_options;
	guint32 ipaddr;
	guint32 dns1;
	guint32 dns2;
	guint32 nbns1;
	guint32 nbns2;
};

#define FILL_IP(req, type, var) 				\
	if (req) {						\
		ipcp->options[len] = type;			\
		ipcp->options[len + 1] = 6;			\
		memcpy(ipcp->options + len + 2, var, 4);	\
								\
		len += 6;					\
	}							\

static void ipcp_generate_config_options(struct ipcp_data *ipcp)
{
	guint16 len = 0;

	FILL_IP(ipcp->req_options & REQ_OPTION_IPADDR,
					IP_ADDRESS, &ipcp->ipaddr);
	FILL_IP(ipcp->req_options & REQ_OPTION_DNS1,
					PRIMARY_DNS_SERVER, &ipcp->dns1);
	FILL_IP(ipcp->req_options & REQ_OPTION_DNS2,
					SECONDARY_DNS_SERVER, &ipcp->dns2);
	FILL_IP(ipcp->req_options & REQ_OPTION_NBNS1,
					PRIMARY_NBNS_SERVER, &ipcp->nbns1);
	FILL_IP(ipcp->req_options & REQ_OPTION_NBNS2,
					SECONDARY_NBNS_SERVER, &ipcp->nbns2);

	ipcp->options_len = len;
}

static void ipcp_reset_config_options(struct ipcp_data *ipcp)
{
	ipcp->req_options = REQ_OPTION_IPADDR | REQ_OPTION_DNS1 |
				REQ_OPTION_DNS2 | REQ_OPTION_NBNS1 |
				REQ_OPTION_NBNS2;
	ipcp->ipaddr = 0;
	ipcp->dns1 = 0;
	ipcp->dns2 = 0;
	ipcp->nbns1 = 0;
	ipcp->nbns2 = 0;

	ipcp_generate_config_options(ipcp);
}

static void ipcp_up(struct pppcp_data *pppcp)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);
	char ip[INET_ADDRSTRLEN];
	char dns1[INET_ADDRSTRLEN];
	char dns2[INET_ADDRSTRLEN];
	struct in_addr addr;

	memset(ip, 0, sizeof(ip));
	addr.s_addr = ipcp->ipaddr;
	inet_ntop(AF_INET, &addr, ip, INET_ADDRSTRLEN);

	memset(dns1, 0, sizeof(dns1));
	addr.s_addr = ipcp->dns1;
	inet_ntop(AF_INET, &addr, dns1, INET_ADDRSTRLEN);

	memset(dns2, 0, sizeof(dns2));
	addr.s_addr = ipcp->dns2;
	inet_ntop(AF_INET, &addr, dns2, INET_ADDRSTRLEN);

	ppp_ipcp_up_notify(pppcp_get_ppp(pppcp), ip[0] ? ip : NULL,
					dns1[0] ? dns1 : NULL,
					dns2[0] ? dns2 : NULL);
}

static void ipcp_down(struct pppcp_data *pppcp)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);

	ipcp_reset_config_options(ipcp);
	pppcp_set_local_options(pppcp, ipcp->options, ipcp->options_len);
	ppp_ipcp_down_notify(pppcp_get_ppp(pppcp));
}

static void ipcp_finished(struct pppcp_data *pppcp)
{
	ppp_ipcp_finished_notify(pppcp_get_ppp(pppcp));
}

static void ipcp_rca(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);
	struct ppp_option_iter iter;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);

		switch (ppp_option_iter_get_type(&iter)) {
		case IP_ADDRESS:
			memcpy(&ipcp->ipaddr, data, 4);
			break;
		case PRIMARY_DNS_SERVER:
			memcpy(&ipcp->dns1, data, 4);
			break;
		case PRIMARY_NBNS_SERVER:
			memcpy(&ipcp->nbns1, data, 4);
			break;
		case SECONDARY_DNS_SERVER:
			memcpy(&ipcp->dns2, data, 4);
			break;
		case SECONDARY_NBNS_SERVER:
			memcpy(&ipcp->nbns2, data, 4);
			break;
		default:
			break;
		}
	}
}

static void ipcp_rcn_nak(struct pppcp_data *pppcp,
				const struct pppcp_packet *packet)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);
	struct ppp_option_iter iter;

	g_print("Received IPCP NAK\n");

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);

		switch (ppp_option_iter_get_type(&iter)) {
		case IP_ADDRESS:
			g_print("Setting suggested ip addr\n");
			ipcp->req_options |= REQ_OPTION_IPADDR;
			memcpy(&ipcp->ipaddr, data, 4);
			break;
		case PRIMARY_DNS_SERVER:
			g_print("Setting suggested dns1\n");
			ipcp->req_options |= REQ_OPTION_DNS1;
			memcpy(&ipcp->dns1, data, 4);
			break;
		case PRIMARY_NBNS_SERVER:
			g_print("Setting suggested nbns1\n");
			ipcp->req_options |= REQ_OPTION_NBNS1;
			memcpy(&ipcp->nbns1, data, 4);
			break;
		case SECONDARY_DNS_SERVER:
			g_print("Setting suggested dns2\n");
			ipcp->req_options |= REQ_OPTION_DNS2;
			memcpy(&ipcp->dns2, data, 4);
			break;
		case SECONDARY_NBNS_SERVER:
			g_print("Setting suggested nbns2\n");
			ipcp->req_options |= REQ_OPTION_NBNS2;
			memcpy(&ipcp->nbns2, data, 4);
			break;
		default:
			break;
		}
	}

	ipcp_generate_config_options(ipcp);
	pppcp_set_local_options(pppcp, ipcp->options, ipcp->options_len);
}

static void ipcp_rcn_rej(struct pppcp_data *pppcp,
				const struct pppcp_packet *packet)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);
	struct ppp_option_iter iter;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		switch (ppp_option_iter_get_type(&iter)) {
		case IP_ADDRESS:
			ipcp->req_options &= ~REQ_OPTION_IPADDR;
			break;
		case PRIMARY_DNS_SERVER:
			ipcp->req_options &= ~REQ_OPTION_DNS1;
			break;
		case PRIMARY_NBNS_SERVER:
			ipcp->req_options &= ~REQ_OPTION_NBNS1;
			break;
		case SECONDARY_DNS_SERVER:
			ipcp->req_options &= ~REQ_OPTION_DNS2;
			break;
		case SECONDARY_NBNS_SERVER:
			ipcp->req_options &= ~REQ_OPTION_NBNS2;
			break;
		default:
			break;
		}
	}

	ipcp_generate_config_options(ipcp);
	pppcp_set_local_options(pppcp, ipcp->options, ipcp->options_len);
}

static enum rcr_result ipcp_rcr(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet,
					guint8 **new_options, guint16 *new_len)
{
	struct ppp_option_iter iter;

	ppp_option_iter_init(&iter, packet);

	if (ppp_option_iter_next(&iter) == FALSE)
		return RCR_ACCEPT;

	/* Reject all options */
	*new_len = ntohs(packet->length) - sizeof(*packet);
	*new_options = g_memdup(packet->data, *new_len);

	return RCR_REJECT;
}

struct pppcp_proto ipcp_proto = {
	.proto			= IPCP_PROTO,
	.name			= "ipcp",
	.supported_codes	= IPCP_SUPPORTED_CODES,
	.this_layer_up		= ipcp_up,
	.this_layer_down	= ipcp_down,
	.this_layer_finished	= ipcp_finished,
	.rca			= ipcp_rca,
	.rcn_nak		= ipcp_rcn_nak,
	.rcn_rej		= ipcp_rcn_rej,
	.rcr			= ipcp_rcr,
};

struct pppcp_data *ipcp_new(GAtPPP *ppp)
{
	struct ipcp_data *ipcp;
	struct pppcp_data *pppcp;

	ipcp = g_try_new0(struct ipcp_data, 1);
	if (!ipcp)
		return NULL;

	pppcp = pppcp_new(ppp, &ipcp_proto);
	if (!pppcp) {
		g_printerr("Failed to allocate PPPCP struct\n");
		g_free(ipcp);
		return NULL;
	}

	pppcp_set_data(pppcp, ipcp);
	ipcp_reset_config_options(ipcp);
	pppcp_set_local_options(pppcp, ipcp->options, ipcp->options_len);

	return pppcp;
}

void ipcp_free(struct pppcp_data *data)
{
	struct ipcp_data *ipcp = pppcp_get_data(data);

	g_free(ipcp);
	pppcp_free(data);
}
