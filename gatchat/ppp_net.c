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
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <glib.h>

#include "gatutil.h"
#include "gatppp.h"
#include "ppp.h"

static void ipcp_free(struct pppcp_data *data);

/* XXX should be maximum IP Packet size */
#define MAX_PACKET 1500
#define PPP_IP_PROTO	0x0021

struct ipcp_data {
	guint8 ip_address[4];
	guint8 primary_dns[4];
	guint8 secondary_dns[4];
	struct pppcp_data *pppcp;
};

static struct pppcp_data *ipcp_new(GAtPPP *ppp);
static void ipcp_option_process(gpointer data, gpointer user);
static guint ipcp_option_scan(struct ppp_option *option, gpointer user);

static void ip_process_packet(gpointer priv, guint8 *packet)
{
	struct ppp_net_data *data = priv;
	GError *error = NULL;
	GIOStatus status;
	gsize bytes_written;
	guint16 len;

	/*
	 * since ppp_net_open can fail, we need to make sure
	 * channel is valid
	 */
	if (data->channel == NULL)
		return;

	/* find the length of the packet to transmit */
	len = get_host_short(&packet[2]);
	status = g_io_channel_write_chars(data->channel, (gchar *) packet,
                                          len, &bytes_written, &error);
}

/*
 * packets received by the tun interface need to be written to
 * the modem.  So, just read a packet, write out to the modem
 *
 * TBD - how do we know we have a full packet?  Do we care?
 */
static gboolean ppp_net_callback(GIOChannel *channel, GIOCondition cond,
				gpointer userdata)
{
	GIOStatus status;
	gchar buf[MAX_PACKET + 2];
	gsize bytes_read;
	GError *error = NULL;
	struct ppp_header *ppp = (struct ppp_header *) buf;
	struct ppp_net_data *data = (struct ppp_net_data *) userdata;

	if (cond & G_IO_IN) {
		/* leave space to add PPP protocol field */
		status = g_io_channel_read_chars(channel, buf + 2, MAX_PACKET,
				&bytes_read, &error);
		if (bytes_read > 0) {
			ppp->proto = htons(PPP_IP_PROTO);
			ppp_transmit(data->ppp, (guint8 *) buf, bytes_read);
		}
		if (status != G_IO_STATUS_NORMAL && status != G_IO_STATUS_AGAIN)
			return FALSE;
	}
	return TRUE;
}

void ppp_net_close(struct ppp_net_data *data)
{
	/* Not Implemented Yet */
}

void ppp_net_open(struct ppp_net_data *data)
{
	int fd;
	struct ifreq ifr;
	GIOChannel *channel;
	int signal_source;
	int err;

	if (data == NULL)
		return;

	/* open a tun interface */
	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		g_printerr("error opening tun\n");
		return;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	err = ioctl(fd, TUNSETIFF, (void *)&ifr);
	if (err < 0) {
		g_printerr("error %d setting ifr\n", err);
		close(fd);
		return;
	}
	data->if_name = strdup(ifr.ifr_name);

	/* create a channel for reading and writing to this interface */
	channel = g_io_channel_unix_new(fd);
	if (!channel) {
		g_printerr("Error creating I/O Channel to TUN device\n");
		close(fd);
		return;
	}
	if (!g_at_util_setup_io(channel, G_IO_FLAG_NONBLOCK)) {
		g_io_channel_unref(channel);
		return;
	}
	data->channel = channel;
	g_io_channel_set_buffered(channel, FALSE);
	signal_source = g_io_add_watch(channel,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			ppp_net_callback, (gpointer) data);

	pppcp_generate_event(data->ipcp, OPEN, NULL, 0);

}

struct ppp_packet_handler ip_packet_handler = {
	.proto = PPP_IP_PROTO,
	.handler = ip_process_packet,
};

void ppp_net_free(struct ppp_net_data *data)
{
	/* TBD unregister packet handler */

	/* cleanup tun interface */
	ppp_net_close(data);

	/* free ipcp data */
	ipcp_free(data->ipcp);

	/* free self */
	g_free(data);
}

struct ppp_net_data *ppp_net_new(GAtPPP *ppp)
{
	struct ppp_net_data *data;

	data = g_try_malloc0(sizeof(*data));
	if (!data)
		return NULL;

	data->ppp = ppp;
	data->ipcp = ipcp_new(ppp);

	/* register packet handler for IP protocol */
	ip_packet_handler.priv = data;
	ppp_register_packet_handler(&ip_packet_handler);
	return data;
}

/****** IPCP support ****************/
#define IPCP_SUPPORTED_CODES	  ((1 << CONFIGURE_REQUEST) | \
				  (1 << CONFIGURE_ACK) | \
				  (1 << CONFIGURE_NAK) | \
				  (1 << CONFIGURE_REJECT) | \
				  (1 << TERMINATE_REQUEST) | \
				  (1 << TERMINATE_ACK) | \
				  (1 << CODE_REJECT))

#define IPCP_PROTO		0x8021

enum ipcp_option_types {
	IP_ADDRESSES		= 1,
	IP_COMPRESSION_PROTO	= 2,
	IP_ADDRESS		= 3,
	PRIMARY_DNS_SERVER	= 129,
	SECONDARY_DNS_SERVER	= 131,
};

static void ipcp_up(struct pppcp_data *pppcp)
{
	struct ipcp_data *data = pppcp->priv;
	GAtPPP *ppp = pppcp->ppp;

	/* call the connect function */
	if (ppp->connect_cb)
		ppp->connect_cb(ppp, G_AT_PPP_CONNECT_SUCCESS,
				__get_unaligned_long(data->ip_address),
				__get_unaligned_long(data->primary_dns),
				__get_unaligned_long(data->secondary_dns),
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

struct pppcp_action ipcp_action = {
	.this_layer_up =	ipcp_up,
	.this_layer_down = 	ipcp_down,
	.this_layer_started = 	ipcp_started,
	.this_layer_finished =	ipcp_finished,
	.option_scan = 		ipcp_option_scan,
	.option_process = 	ipcp_option_process,
};

struct ppp_packet_handler ipcp_packet_handler = {
	.proto = IPCP_PROTO,
	.handler = pppcp_process_packet,
};

/*
 * Scan the option to see if it is acceptable, unacceptable, or rejected
 */
static guint ipcp_option_scan(struct ppp_option *option, gpointer user)
{
	switch (option->type) {
	case IP_ADDRESS:
	case PRIMARY_DNS_SERVER:
	case SECONDARY_DNS_SERVER:
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
	case SECONDARY_DNS_SERVER:
		memcpy(ipcp->secondary_dns, option->data, 4);
		break;
	default:
		g_printerr("Unable to process unknown option %d\n", option->type);
		break;
	}
}

static void ipcp_free(struct pppcp_data *data)
{
	struct ipcp_data *ipcp = data->priv;

	/* TBD unregister IPCP packet handler */

	/* free ipcp */
	g_free(ipcp);

	/* free pppcp */
	pppcp_free(data);
}

static struct pppcp_data * ipcp_new(GAtPPP *ppp)
{
	struct ipcp_data *data;
	struct pppcp_data *pppcp;
	struct ppp_option *ipcp_option;

	data = g_try_malloc0(sizeof(*data));
	if (!data)
		return NULL;

	pppcp = pppcp_new(ppp, IPCP_PROTO, data);
	if (!pppcp) {
		g_printerr("Failed to allocate PPPCP struct\n");
		g_free(data);
		return NULL;
	}
	pppcp_set_valid_codes(pppcp, IPCP_SUPPORTED_CODES);
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

	/* register packet handler for IPCP protocol */
	ipcp_packet_handler.priv = pppcp;
	ppp_register_packet_handler(&ipcp_packet_handler);
	return pppcp;
}
