/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Rémi Denis-Courmont <remi.denis-courmont@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#ifndef SOL_NETLINK
#define SOL_NETLINK 270 /* libc!? */
#endif
#include "phonet.h"
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>
#include <glib.h>

#include "netlink.h"

struct _GPhonetNetlink {
	GPhonetNetlinkFunc callback;
	void *opaque;
	guint watch;
};

static inline GIsiModem *make_modem(unsigned idx)
{
	return (void *)(uintptr_t)idx;
}

static void bring_up(unsigned ifindex)
{
	struct ifreq req = { .ifr_ifindex = ifindex, };
	int fd = socket(PF_LOCAL, SOCK_DGRAM, 0);

	if (ioctl(fd, SIOCGIFNAME, &req) ||
	    ioctl(fd, SIOCGIFFLAGS, &req))
		goto error;
	req.ifr_flags |= IFF_UP | IFF_RUNNING;
	ioctl(fd, SIOCSIFFLAGS, &req);
error:
	close(fd);
}

/* Parser Netlink messages */
static gboolean g_pn_nl_process(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	struct {
		struct nlmsghdr nlh;
		struct rtmsg rtm;
		char buf[1024];
	} req;
	struct iovec iov = { &req, sizeof(req), };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, };
	ssize_t ret;
	struct nlmsghdr *nlh;
	int fd = g_io_channel_unix_get_fd(channel);
	GPhonetNetlink *self = data;

	if (cond & (G_IO_NVAL|G_IO_HUP))
		return FALSE;

	ret = recvmsg(fd, &msg, 0);
	if (ret == -1 || (msg.msg_flags & MSG_TRUNC))
		return TRUE;

	for (nlh = (struct nlmsghdr *)&req; NLMSG_OK(nlh, (size_t)ret);
						nlh = NLMSG_NEXT(nlh, ret)) {
		const struct ifaddrmsg *ifa;
		const struct rtattr *rta;
		int len;
		bool up;
		uint8_t addr = 0;

		if (nlh->nlmsg_type == NLMSG_DONE)
			break;
		switch (nlh->nlmsg_type) {
		case NLMSG_ERROR: {
			const struct nlmsgerr *err;
			err = (struct nlmsgerr *)NLMSG_DATA(nlh);
			g_critical("Netlink error: %s", strerror(-err->error));
			return FALSE;
		}
		case RTM_NEWADDR:
			up = true;
			break;
		case RTM_DELADDR:
			up = false;
			break;
		default:
			continue;
		}
		/* We have a route message */
		ifa = NLMSG_DATA(nlh);
		len = IFA_PAYLOAD(nlh);

		/* If Phonet is absent, kernel transmits other families... */
		if (ifa->ifa_family != AF_PHONET)
			continue;
		for (rta = IFA_RTA(ifa); RTA_OK(rta, len);
						rta = RTA_NEXT(rta, len))
			if (rta->rta_type == IFA_LOCAL)
				memcpy(&addr, RTA_DATA(rta), 1);
		if (up)
			bring_up(ifa->ifa_index);
		self->callback(up, addr,
				make_modem(ifa->ifa_index), self->opaque);
	}
	return TRUE;
}

/* Dump current Phonet address table */
static int g_pn_netlink_query(int fd)
{
	struct {
		struct nlmsghdr nlh;
		struct rtmsg rtm;
	} req;
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK, };

	req.nlh.nlmsg_type = RTM_GETADDR;
	req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.rtm));
	req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
	req.nlh.nlmsg_seq = 0;
	req.nlh.nlmsg_pid = getpid();

	req.rtm.rtm_family = AF_PHONET;
	req.rtm.rtm_dst_len = 6;
	req.rtm.rtm_src_len = 0;
	req.rtm.rtm_tos = 0;

	req.rtm.rtm_table = RT_TABLE_MAIN;
	req.rtm.rtm_protocol = RTPROT_STATIC;
	req.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
	req.rtm.rtm_type = RTN_UNICAST;
	req.rtm.rtm_flags = 0;

	if (sendto(fd, &req, req.nlh.nlmsg_len, 0,
			(struct sockaddr *)&addr, sizeof(addr)) == -1)
		return -1;
	return 0;
}

GPhonetNetlink *g_pn_netlink_start(GPhonetNetlinkFunc cb, void *opaque)
{
	GIOChannel *chan;
	GPhonetNetlink *self;
	unsigned group = RTNLGRP_PHONET_IFADDR;
	int fd;

	self = malloc(sizeof(*self));
	if (self == NULL)
		return NULL;

	fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (fd == -1)
		goto error;

	fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL));
	if (setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
			&group, sizeof(group)))
		goto error;
	g_pn_netlink_query(fd);

	chan = g_io_channel_unix_new(fd);
	if (chan == NULL)
		goto error;
	g_io_channel_set_close_on_unref(chan, TRUE);
	g_io_channel_set_encoding(chan, NULL, NULL);
	g_io_channel_set_buffered(chan, FALSE);

	self->callback = cb;
	self->opaque = opaque;
	self->watch = g_io_add_watch(chan, G_IO_IN|G_IO_ERR|G_IO_HUP,
					g_pn_nl_process, self);
	g_io_channel_unref(chan);
	return self;

error:
	if (fd != -1)
		close(fd);
	free(self);
	return NULL;
}

void g_pn_netlink_stop(GPhonetNetlink *self)
{
	g_source_remove(self->watch);
	free(self);
}
