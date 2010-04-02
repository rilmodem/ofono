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

struct ipcp_data {
	guint8 ip_address[4];
	guint8 primary_dns[4];
	guint8 secondary_dns[4];
	guint8 primary_nbns[4];
	guint8 secondary_nbns[4];
	struct pppcp_data *pppcp;
};

#define IPCP_SUPPORTED_CODES	  ((1 << CONFIGURE_REQUEST) | \
				  (1 << CONFIGURE_ACK) | \
				  (1 << CONFIGURE_NAK) | \
				  (1 << CONFIGURE_REJECT) | \
				  (1 << TERMINATE_REQUEST) | \
				  (1 << TERMINATE_ACK) | \
				  (1 << CODE_REJECT))

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

static void ipcp_up(struct pppcp_data *pppcp)
{
	struct ipcp_data *data = pppcp->priv;
	GAtPPP *ppp = pppcp->ppp;
	char ip[INET_ADDRSTRLEN];
	char dns1[INET_ADDRSTRLEN];
	char dns2[INET_ADDRSTRLEN];
	struct in_addr addr;

	if (ppp->connect_cb == NULL)
		return;

	memset(ip, 0, sizeof(ip));
	addr.s_addr = __get_unaligned_long(data->ip_address);
	inet_ntop(AF_INET, &addr, ip, INET_ADDRSTRLEN);

	memset(dns1, 0, sizeof(dns1));
	addr.s_addr = __get_unaligned_long(data->primary_dns);
	inet_ntop(AF_INET, &addr, dns1, INET_ADDRSTRLEN);

	memset(dns2, 0, sizeof(dns2));
	addr.s_addr = __get_unaligned_long(data->secondary_dns);
	inet_ntop(AF_INET, &addr, dns2, INET_ADDRSTRLEN);

	ppp->connect_cb(G_AT_PPP_CONNECT_SUCCESS,
			pppcp->ppp->net->if_name,
			ip[0] ? ip : NULL,
			dns1[0] ? dns1 : NULL,
			dns2[0] ? dns2 : NULL,
			ppp->connect_data);
}

static void ipcp_down(struct pppcp_data *data)
{
	g_print("ipcp down\n");

	/* re-add what default config options we want negotiated */
}

/*
 * Tell the protocol to start the handshake
 */
static void ipcp_started(struct pppcp_data *data)
{
	pppcp_generate_event(data, UP, NULL, 0);
}

static void ipcp_finished(struct pppcp_data *data)
{
	g_print("ipcp finished\n");
}

/*
 * Scan the option to see if it is acceptable, unacceptable, or rejected
 */
static guint ipcp_option_scan(struct ppp_option *option, gpointer user)
{
	switch (option->type) {
	case IP_ADDRESS:
	case PRIMARY_DNS_SERVER:
	case PRIMARY_NBNS_SERVER:
	case SECONDARY_DNS_SERVER:
	case SECONDARY_NBNS_SERVER:
		return OPTION_ACCEPT;
	default:
		g_printerr("Unknown ipcp option type %d\n", option->type);
		return OPTION_REJECT;
	}
}

/*
 * act on an acceptable option
 */
static void ipcp_option_process(gpointer data, gpointer user)
{
	struct ppp_option *option = data;
	struct ipcp_data *ipcp = user;

	switch (option->type) {
	case IP_ADDRESS:
		memcpy(ipcp->ip_address, option->data, 4);
		break;
	case PRIMARY_DNS_SERVER:
		memcpy(ipcp->primary_dns, option->data, 4);
		break;
	case PRIMARY_NBNS_SERVER:
		memcpy(ipcp->primary_nbns, option->data, 4);
		break;
	case SECONDARY_DNS_SERVER:
		memcpy(ipcp->secondary_dns, option->data, 4);
		break;
	case SECONDARY_NBNS_SERVER:
		memcpy(ipcp->secondary_nbns, option->data, 4);
		break;
	default:
		g_printerr("Unable to process unknown option %d\n", option->type);
		break;
	}
}

struct pppcp_action ipcp_action = {
	.this_layer_up =	ipcp_up,
	.this_layer_down = 	ipcp_down,
	.this_layer_started = 	ipcp_started,
	.this_layer_finished =	ipcp_finished,
	.option_scan = 		ipcp_option_scan,
	.option_process = 	ipcp_option_process,
};

static const char *ipcp_option_strings[256] = {
	[IP_ADDRESSES]		= "IP-Addresses (deprecated)",
	[IP_COMPRESSION_PROTO]	= "IP-Compression-Protocol",
	[IP_ADDRESS]		= "IP-Address",
	[MOBILE_IPV4]		= "Mobile-IPv4",
	[PRIMARY_DNS_SERVER]	= "Primary DNS Server Address",
	[PRIMARY_NBNS_SERVER]	= "Primary NBNS Server Address",
	[SECONDARY_DNS_SERVER]	= "Secondary DNS Server Address",
	[SECONDARY_NBNS_SERVER]	= "Secondary NBNS Server Address",
};

struct pppcp_data *ipcp_new(GAtPPP *ppp)
{
	struct ipcp_data *data;
	struct pppcp_data *pppcp;
	struct ppp_option *ipcp_option;

	data = g_try_malloc0(sizeof(*data));
	if (!data)
		return NULL;

	pppcp = pppcp_new(ppp, IPCP_PROTO);
	if (!pppcp) {
		g_printerr("Failed to allocate PPPCP struct\n");
		g_free(data);
		return NULL;
	}
	pppcp_set_valid_codes(pppcp, IPCP_SUPPORTED_CODES);
	pppcp->option_strings = ipcp_option_strings;
	pppcp->prefix = "ipcp";
	pppcp->priv = data;

	/* set the actions */
	pppcp->action = &ipcp_action;

	/* add the default config options */
	ipcp_option = g_try_malloc0(6);
	if (!ipcp_option) {
		pppcp_free(pppcp);
		g_free(data);
		return NULL;
	}
	ipcp_option->type = IP_ADDRESS;
	ipcp_option->length= 6;
	pppcp_add_config_option(pppcp, ipcp_option);

	return pppcp;
}

void ipcp_free(struct pppcp_data *data)
{
	struct ipcp_data *ipcp = data->priv;

	/* TBD unregister IPCP packet handler */

	/* free ipcp */
	g_free(ipcp);

	/* free pppcp */
	pppcp_free(data);
}
