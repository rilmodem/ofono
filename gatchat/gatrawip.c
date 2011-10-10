/*
 *
 *  AT chat library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>

#include <glib.h>

#include "ringbuffer.h"
#include "gatrawip.h"

struct _GAtRawIP {
	gint ref_count;
	GAtIO *io;
	GAtIO *tun_io;
	char *ifname;
	struct ring_buffer *write_buffer;
	struct ring_buffer *tun_write_buffer;
	GAtDebugFunc debugf;
	gpointer debug_data;
};

GAtRawIP *g_at_rawip_new(GIOChannel *channel)
{
	GAtRawIP *rawip;
	GAtIO *io;

	io = g_at_io_new(channel);
	if (io == NULL)
		return NULL;

	rawip = g_at_rawip_new_from_io(io);

	g_at_io_unref(io);

	return rawip;
}

GAtRawIP *g_at_rawip_new_from_io(GAtIO *io)
{
	GAtRawIP *rawip;

	rawip = g_try_new0(GAtRawIP, 1);
	if (rawip == NULL)
		return NULL;

	rawip->ref_count = 1;

	rawip->write_buffer = NULL;
	rawip->tun_write_buffer = NULL;

	rawip->io = g_at_io_ref(io);

	return rawip;
}

GAtRawIP *g_at_rawip_ref(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return NULL;

	g_atomic_int_inc(&rawip->ref_count);

	return rawip;
}

void g_at_rawip_unref(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return;

	if (g_atomic_int_dec_and_test(&rawip->ref_count) == FALSE)
		return;

	g_at_rawip_shutdown(rawip);

	g_at_io_unref(rawip->io);
	rawip->io = NULL;

	g_free(rawip->ifname);
	rawip->ifname = NULL;

	g_free(rawip);
}

static gboolean can_write_data(gpointer data)
{
	GAtRawIP *rawip = data;
	unsigned int len;
	unsigned char *buf;
	gsize bytes_written;

	if (rawip->write_buffer == NULL)
		return FALSE;

	len = ring_buffer_len_no_wrap(rawip->write_buffer);
	buf = ring_buffer_read_ptr(rawip->write_buffer, 0);

	bytes_written = g_at_io_write(rawip->io, (gchar *) buf, len);
	ring_buffer_drain(rawip->write_buffer, bytes_written);

	if (ring_buffer_len(rawip->write_buffer) > 0)
		return TRUE;

	rawip->write_buffer = NULL;

	return FALSE;
}

static gboolean tun_write_data(gpointer data)
{
	GAtRawIP *rawip = data;
	unsigned int len;
	unsigned char *buf;
	gsize bytes_written;

	if (rawip->tun_write_buffer == NULL)
		return FALSE;

	len = ring_buffer_len_no_wrap(rawip->tun_write_buffer);
	buf = ring_buffer_read_ptr(rawip->tun_write_buffer, 0);

	bytes_written = g_at_io_write(rawip->tun_io, (gchar *) buf, len);
	ring_buffer_drain(rawip->tun_write_buffer, bytes_written);

	if (ring_buffer_len(rawip->tun_write_buffer) > 0)
		return TRUE;

	rawip->tun_write_buffer = NULL;

	return FALSE;
}

static void new_bytes(struct ring_buffer *rbuf, gpointer user_data)
{
	GAtRawIP *rawip = user_data;

	rawip->tun_write_buffer = rbuf;

	g_at_io_set_write_handler(rawip->tun_io, tun_write_data, rawip);
}

static void tun_bytes(struct ring_buffer *rbuf, gpointer user_data)
{
	GAtRawIP *rawip = user_data;

	rawip->write_buffer = rbuf;

	g_at_io_set_write_handler(rawip->io, can_write_data, rawip);
}

static void create_tun(GAtRawIP *rawip)
{
	GIOChannel *channel;
	struct ifreq ifr;
	int fd, err;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strcpy(ifr.ifr_name, "gprs%d");

	err = ioctl(fd, TUNSETIFF, (void *) &ifr);
	if (err < 0) {
		close(fd);
		return;
	}

	rawip->ifname = g_strdup(ifr.ifr_name);

	channel = g_io_channel_unix_new(fd);
	if (channel == NULL) {
		close(fd);
		return;
	}

	rawip->tun_io = g_at_io_new(channel);

	g_io_channel_unref(channel);
}

void g_at_rawip_open(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return;

	create_tun(rawip);

	if (rawip->tun_io == NULL)
		return;

	g_at_io_set_read_handler(rawip->io, new_bytes, rawip);
	g_at_io_set_read_handler(rawip->tun_io, tun_bytes, rawip);
}

void g_at_rawip_shutdown(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return;

	if (rawip->tun_io == NULL)
		return;

	g_at_io_set_read_handler(rawip->io, NULL, NULL);
	g_at_io_set_read_handler(rawip->tun_io, NULL, NULL);

	rawip->write_buffer = NULL;
	rawip->tun_write_buffer = NULL;

	g_at_io_unref(rawip->tun_io);
	rawip->tun_io = NULL;
}

const char *g_at_rawip_get_interface(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return NULL;

	return rawip->ifname;
}

void g_at_rawip_set_debug(GAtRawIP *rawip, GAtDebugFunc func,
						gpointer user_data)
{
	if (rawip == NULL)
		return;

	rawip->debugf = func;
	rawip->debug_data = user_data;
}
