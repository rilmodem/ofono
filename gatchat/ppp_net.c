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
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include <glib.h>

#include "gatutil.h"
#include "gatppp.h"
#include "ppp.h"

#define MAX_PACKET 1500

struct ppp_net {
	GAtPPP *ppp;
	char *if_name;
	GIOChannel *channel;
	guint watch;
	gint mtu;
	struct ppp_header *ppp_packet;
};

gboolean ppp_net_set_mtu(struct ppp_net *net, guint16 mtu)
{
	struct ifreq ifr;
	int sk, err;

	if (net == NULL || mtu > MAX_PACKET)
		return FALSE;

	net->mtu = mtu;

	sk = socket(AF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return FALSE;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, net->if_name, sizeof(ifr.ifr_name));
	ifr.ifr_mtu = mtu;

	err = ioctl(sk, SIOCSIFMTU, (caddr_t) &ifr);

	close(sk);

	if (err < 0)
		return FALSE;

	return TRUE;
}

void ppp_net_process_packet(struct ppp_net *net, const guint8 *packet,
				gsize plen)
{
	GIOStatus status;
	gsize bytes_written;
	guint16 len;

	if (plen < 4)
		return;

	/* find the length of the packet to transmit */
	len = get_host_short(&packet[2]);
	status = g_io_channel_write_chars(net->channel, (gchar *) packet,
						MIN(len, plen),
						&bytes_written, NULL);

	if (status != G_IO_STATUS_NORMAL)
		return;
}

/*
 * packets received by the tun interface need to be written to
 * the modem.  So, just read a packet, write out to the modem
 */
static gboolean ppp_net_callback(GIOChannel *channel, GIOCondition cond,
				gpointer userdata)
{
	struct ppp_net *net = (struct ppp_net *) userdata;
	GIOStatus status;
	gsize bytes_read;
	gchar *buf = (gchar *) net->ppp_packet->info;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	if (cond & G_IO_IN) {
		/* leave space to add PPP protocol field */
		status = g_io_channel_read_chars(channel, buf, net->mtu,
							&bytes_read, NULL);
		if (bytes_read > 0)
			ppp_transmit(net->ppp, (guint8 *) net->ppp_packet,
					bytes_read);

		if (status != G_IO_STATUS_NORMAL && status != G_IO_STATUS_AGAIN)
			return FALSE;
	}
	return TRUE;
}

const char *ppp_net_get_interface(struct ppp_net *net)
{
	return net->if_name;
}

struct ppp_net *ppp_net_new(GAtPPP *ppp, int fd)
{
	struct ppp_net *net;
	GIOChannel *channel = NULL;
	struct ifreq ifr;
	int err;

	net = g_try_new0(struct ppp_net, 1);
	if (net == NULL)
		goto badalloc;

	net->ppp_packet = ppp_packet_new(MAX_PACKET, PPP_IP_PROTO);
	if (net->ppp_packet == NULL)
		goto error;

	/*
	 * If the fd value is still the default one,
	 * open the tun interface and configure it.
	 */
	memset(&ifr, 0, sizeof(ifr));

	if (fd < 0) {
		/* open a tun interface */
		fd = open("/dev/net/tun", O_RDWR);
		if (fd < 0)
			goto error;

		ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
		strcpy(ifr.ifr_name, "ppp%d");

		err = ioctl(fd, TUNSETIFF, (void *) &ifr);
		if (err < 0)
			goto error;
	} else {
		err = ioctl(fd, TUNGETIFF, (void *) &ifr);
		if (err < 0)
			goto error;
	}

	net->if_name = strdup(ifr.ifr_name);

	/* create a channel for reading and writing to this interface */
	channel = g_io_channel_unix_new(fd);
	if (channel == NULL)
		goto error;

	if (!g_at_util_setup_io(channel, 0))
		goto error;

	g_io_channel_set_buffered(channel, FALSE);

	net->channel = channel;
	net->watch = g_io_add_watch(channel,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			ppp_net_callback, net);
	net->ppp = ppp;

	net->mtu = MAX_PACKET;
	return net;

error:
	if (channel)
		g_io_channel_unref(channel);

	g_free(net->if_name);
	g_free(net->ppp_packet);
	g_free(net);

badalloc:
	if (fd >= 0)
		close(fd);

	return NULL;
}

void ppp_net_free(struct ppp_net *net)
{
	if (net->watch) {
		g_source_remove(net->watch);
		net->watch = 0;
	}

	g_io_channel_unref(net->channel);

	g_free(net->ppp_packet);
	g_free(net->if_name);
	g_free(net);
}

void ppp_net_suspend_interface(struct ppp_net *net)
{
	if (net == NULL || net->channel == NULL)
		return;

	if (net->watch == 0)
		return;

	g_source_remove(net->watch);
	net->watch = 0;
}

void ppp_net_resume_interface(struct ppp_net *net)
{
	if (net == NULL || net->channel == NULL)
		return;

	net->watch = g_io_add_watch(net->channel,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			ppp_net_callback, net);
}
