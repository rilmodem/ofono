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
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include <glib.h>

#include "gatutil.h"
#include "gatppp.h"
#include "ppp.h"

/* XXX should be maximum IP Packet size */
#define MAX_PACKET 1500

void ppp_net_process_packet(struct ppp_net_data *data, guint8 *packet)
{
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
	strcpy(ifr.ifr_name, "ppp%d");
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
}

struct ppp_net_data *ppp_net_new(GAtPPP *ppp)
{
	struct ppp_net_data *data;

	data = g_try_malloc0(sizeof(*data));
	if (!data)
		return NULL;

	data->ppp = ppp;

	return data;
}

void ppp_net_free(struct ppp_net_data *data)
{
	/* cleanup tun interface */
	ppp_net_close(data);

	/* free self */
	g_free(data);
}
