/*
 *
 *  PPP library with GLib integration
 *
 *  Copyright (C) 2009-2011  Intel Corporation. All rights reserved.
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

#define IPCP_SUPPORTED_CODES	((1 << PPPCP_CODE_TYPE_CONFIGURE_REQUEST) | \
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

#define MAX_IPCP_FAILURE	100

struct ipcp_data {
	guint8 options[MAX_CONFIG_OPTION_SIZE];
	guint16 options_len;
	guint8 req_options;
	guint32 local_addr;
	guint32 peer_addr;
	guint32 dns1;
	guint32 dns2;
	guint32 nbns1;
	guint32 nbns2;
	gboolean is_server;
};

#define FILL_IP(options, req, type, var)		\
	if (req) {					\
		options[len] = type;			\
		options[len + 1] = 6;			\
		memcpy(options + len + 2, var, 4);	\
							\
		len += 6;				\
	}						\

static void ipcp_generate_config_options(struct ipcp_data *ipcp)
{
	guint16 len = 0;

	FILL_IP(ipcp->options, ipcp->req_options & REQ_OPTION_IPADDR,
					IP_ADDRESS, &ipcp->local_addr);
	FILL_IP(ipcp->options, ipcp->req_options & REQ_OPTION_DNS1,
					PRIMARY_DNS_SERVER, &ipcp->dns1);
	FILL_IP(ipcp->options, ipcp->req_options & REQ_OPTION_DNS2,
					SECONDARY_DNS_SERVER, &ipcp->dns2);
	FILL_IP(ipcp->options, ipcp->req_options & REQ_OPTION_NBNS1,
					PRIMARY_NBNS_SERVER, &ipcp->nbns1);
	FILL_IP(ipcp->options, ipcp->req_options & REQ_OPTION_NBNS2,
					SECONDARY_NBNS_SERVER, &ipcp->nbns2);

	ipcp->options_len = len;
}

static void ipcp_reset_client_config_options(struct ipcp_data *ipcp)
{
	ipcp->req_options = REQ_OPTION_IPADDR | REQ_OPTION_DNS1 |
				REQ_OPTION_DNS2 | REQ_OPTION_NBNS1 |
				REQ_OPTION_NBNS2;

	ipcp->local_addr = 0;
	ipcp->peer_addr = 0;
	ipcp->dns1 = 0;
	ipcp->dns2 = 0;
	ipcp->nbns1 = 0;
	ipcp->nbns2 = 0;

	ipcp_generate_config_options(ipcp);
}

static void ipcp_reset_server_config_options(struct ipcp_data *ipcp)
{
	if (ipcp->local_addr != 0)
		ipcp->req_options = REQ_OPTION_IPADDR;
	else
		ipcp->req_options = 0;

	ipcp_generate_config_options(ipcp);
}

void ipcp_set_server_info(struct pppcp_data *pppcp, guint32 peer_addr,
				guint32 dns1, guint32 dns2)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);

	ipcp->peer_addr = peer_addr;
	ipcp->dns1 = dns1;
	ipcp->dns2 = dns2;
}

static void ipcp_up(struct pppcp_data *pppcp)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);
	char local[INET_ADDRSTRLEN];
	char peer[INET_ADDRSTRLEN];
	char dns1[INET_ADDRSTRLEN];
	char dns2[INET_ADDRSTRLEN];
	struct in_addr addr;

	memset(local, 0, sizeof(local));
	addr.s_addr = ipcp->local_addr;
	inet_ntop(AF_INET, &addr, local, INET_ADDRSTRLEN);

	memset(peer, 0, sizeof(peer));
	addr.s_addr = ipcp->peer_addr;
	inet_ntop(AF_INET, &addr, peer, INET_ADDRSTRLEN);

	memset(dns1, 0, sizeof(dns1));
	addr.s_addr = ipcp->dns1;
	inet_ntop(AF_INET, &addr, dns1, INET_ADDRSTRLEN);

	memset(dns2, 0, sizeof(dns2));
	addr.s_addr = ipcp->dns2;
	inet_ntop(AF_INET, &addr, dns2, INET_ADDRSTRLEN);

	ppp_ipcp_up_notify(pppcp_get_ppp(pppcp), local[0] ? local : NULL,
					peer[0] ? peer : NULL,
					dns1[0] ? dns1 : NULL,
					dns2[0] ? dns2 : NULL);
}

static void ipcp_down(struct pppcp_data *pppcp)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);

	if (ipcp->is_server)
		ipcp_reset_server_config_options(ipcp);
	else
		ipcp_reset_client_config_options(ipcp);

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

	if (ipcp->is_server)
		return;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);

		switch (ppp_option_iter_get_type(&iter)) {
		case IP_ADDRESS:
			memcpy(&ipcp->local_addr, data, 4);
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

	if (ipcp->is_server)
		return;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);

		switch (ppp_option_iter_get_type(&iter)) {
		case IP_ADDRESS:
			ipcp->req_options |= REQ_OPTION_IPADDR;
			memcpy(&ipcp->local_addr, data, 4);
			break;
		case PRIMARY_DNS_SERVER:
			ipcp->req_options |= REQ_OPTION_DNS1;
			memcpy(&ipcp->dns1, data, 4);
			break;
		case PRIMARY_NBNS_SERVER:
			ipcp->req_options |= REQ_OPTION_NBNS1;
			memcpy(&ipcp->nbns1, data, 4);
			break;
		case SECONDARY_DNS_SERVER:
			ipcp->req_options |= REQ_OPTION_DNS2;
			memcpy(&ipcp->dns2, data, 4);
			break;
		case SECONDARY_NBNS_SERVER:
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

static enum rcr_result ipcp_server_rcr(struct ipcp_data *ipcp,
					const struct pppcp_packet *packet,
					guint8 **new_options, guint16 *new_len)
{
	struct ppp_option_iter iter;
	guint8 nak_options[MAX_CONFIG_OPTION_SIZE];
	guint16 len = 0;
	guint8 *rej_options = NULL;
	guint16 rej_len = 0;
	guint32 addr;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);
		guint8 type = ppp_option_iter_get_type(&iter);

		switch (type) {
		case IP_ADDRESS:
			memcpy(&addr, data, 4);

			FILL_IP(nak_options,
					addr != ipcp->peer_addr || addr == 0,
					type, &ipcp->peer_addr);
			break;
		case PRIMARY_DNS_SERVER:
			memcpy(&addr, data, 4);

			FILL_IP(nak_options, addr != ipcp->dns1 || addr == 0,
					type, &ipcp->dns1);
			break;
		case SECONDARY_DNS_SERVER:
			memcpy(&addr, data, 4);

			FILL_IP(nak_options, addr != ipcp->dns2 || addr == 0,
					type, &ipcp->dns2);
			break;
		default:
			/* Reject */
			if (rej_options == NULL) {
				guint16 max_len = ntohs(packet->length) - 4;
				rej_options = g_new0(guint8, max_len);
			}

			if (rej_options != NULL) {
				guint8 opt_len =
					ppp_option_iter_get_length(&iter);

				rej_options[rej_len] = type;
				rej_options[rej_len + 1] = opt_len + 2;
				memcpy(rej_options + rej_len + 2,
								data, opt_len);
				rej_len += opt_len + 2;
			}
			break;
		}
	}

	if (rej_len > 0) {
		*new_len = rej_len;
		*new_options = rej_options;

		return RCR_REJECT;
	}

	if (len > 0) {
		*new_len = len;
		*new_options = g_memdup(nak_options, len);

		return RCR_NAK;
	}

	return RCR_ACCEPT;
}

static enum rcr_result ipcp_client_rcr(struct ipcp_data *ipcp,
					const struct pppcp_packet *packet,
					guint8 **new_options, guint16 *new_len)
{
	guint8 *options = NULL;
	struct ppp_option_iter iter;
	guint8 len = 0;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);
		guint8 type = ppp_option_iter_get_type(&iter);

		switch (type) {
		case IP_ADDRESS:
			memcpy(&ipcp->peer_addr, data, 4);

			if (ipcp->peer_addr != 0)
				break;

			/*
			 * Fall through, reject IP_ADDRESS if peer sends
			 * us 0 (expecting us to provide its IP address)
			 */
		default:
			if (options == NULL) {
				guint16 max_len = ntohs(packet->length) - 4;
				options = g_new0(guint8, max_len);
			}

			if (options != NULL) {
				guint8 opt_len =
					ppp_option_iter_get_length(&iter);

				options[len] = type;
				options[len + 1] = opt_len + 2;
				memcpy(options + len + 2, data, opt_len);
				len += opt_len + 2;
			}

			break;
		}
	}

	if (len > 0) {
		*new_len = len;
		*new_options = options;

		return RCR_REJECT;
	}

	return RCR_ACCEPT;
}

static enum rcr_result ipcp_rcr(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet,
					guint8 **new_options, guint16 *new_len)
{
	struct ipcp_data *ipcp = pppcp_get_data(pppcp);

	if (ipcp->is_server)
		return ipcp_server_rcr(ipcp, packet, new_options, new_len);
	else
		return ipcp_client_rcr(ipcp, packet, new_options, new_len);
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

struct pppcp_data *ipcp_new(GAtPPP *ppp, gboolean is_server, guint32 ip)
{
	struct ipcp_data *ipcp;
	struct pppcp_data *pppcp;

	ipcp = g_try_new0(struct ipcp_data, 1);
	if (ipcp == NULL)
		return NULL;

	/*
	 * Some 3G modems use repeated IPCP NAKs as the way of stalling
	 * util sending us the client IP address. So we increase the
	 * default number of NAKs we accept before start treating them
	 * as rejects.
	 */
	pppcp = pppcp_new(ppp, &ipcp_proto, FALSE, MAX_IPCP_FAILURE);
	if (pppcp == NULL) {
		g_free(ipcp);
		return NULL;
	}

	pppcp_set_data(pppcp, ipcp);
	ipcp->is_server = is_server;

	if (is_server) {
		ipcp->local_addr = ip;
		ipcp_reset_server_config_options(ipcp);
	} else
		ipcp_reset_client_config_options(ipcp);

	pppcp_set_local_options(pppcp, ipcp->options, ipcp->options_len);

	return pppcp;
}

void ipcp_free(struct pppcp_data *data)
{
	struct ipcp_data *ipcp = pppcp_get_data(data);

	g_free(ipcp);
	pppcp_free(data);
}
