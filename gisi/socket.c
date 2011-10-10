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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <net/if.h>
#include <fcntl.h>
#include <glib.h>

#include "phonet.h"
#include "socket.h"

GIOChannel *g_isi_phonet_new(unsigned ifindex)
{
	GIOChannel *channel;
	struct sockaddr_pn addr = {
		.spn_family = AF_PHONET,
	};
	char buf[IF_NAMESIZE];

	int fd = socket(PF_PHONET, SOCK_DGRAM, 0);
	if (fd == -1)
		return NULL;

	fcntl(fd, F_SETFD, FD_CLOEXEC);
	/* Use blocking mode on purpose. */

	if (ifindex == 0)
		g_warning("Unspecified modem interface index");
	else if (if_indextoname(ifindex, buf) == NULL ||
		setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, buf, IF_NAMESIZE))
		goto error;

	if (bind(fd, (void *)&addr, sizeof(addr)))
		goto error;

	channel = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);
	return channel;

error:
	close(fd);
	return NULL;
}

size_t g_isi_phonet_peek_length(GIOChannel *channel)
{
	int len;
	int fd = g_io_channel_unix_get_fd(channel);

	return ioctl(fd, FIONREAD, &len) ? 0 : len;
}

ssize_t g_isi_phonet_read(GIOChannel *channel, void *restrict buf, size_t len,
				struct sockaddr_pn *addr)
{
	socklen_t addrlen = sizeof(struct sockaddr_pn);
	ssize_t ret;

	ret = recvfrom(g_io_channel_unix_get_fd(channel), buf, len,
			MSG_DONTWAIT, (void *)addr, &addrlen);
	if (ret == -1)
		return -1;

	return ret;
}
