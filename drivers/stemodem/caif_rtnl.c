/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  ST-Ericsson AB.
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

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <fcntl.h>
#include <linux/rtnetlink.h>

#include <glib.h>

#include <ofono/log.h>

#include "if_caif.h"
#include "caif_rtnl.h"

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

#define RTNL_MSG_SIZE 1024

struct rtnl_msg {
	struct nlmsghdr n;
	struct ifinfomsg i;
	char data[RTNL_MSG_SIZE];
};

struct iplink_req {
	__u32 rtnlmsg_seqnr;
	void *user_data;
	caif_rtnl_create_cb_t callback;
};

static GSList *pending_requests;
static __u32 rtnl_seqnr;
static guint rtnl_watch;
static GIOChannel *rtnl_channel;

static struct iplink_req *find_request(__u32 seq)
{
	GSList *list;

	for (list = pending_requests; list; list = list->next) {
		struct iplink_req *req = list->data;

		if (req->rtnlmsg_seqnr == seq)
			return req;
	}

	return NULL;
}

static void parse_newlink_param(struct ifinfomsg *msg, int size,
						int *index, char *ifname)
{
	struct rtattr *attr;

	for (attr = IFLA_RTA(msg); RTA_OK(attr, size);
		attr = RTA_NEXT(attr, size)) {

		if (attr->rta_type == IFLA_IFNAME &&
				ifname != NULL) {

			strncpy(ifname, RTA_DATA(attr), IF_NAMESIZE);
			ifname[IF_NAMESIZE-1] = '\0';
			break;
		}
	}

	*index = msg->ifi_index;
}

static void parse_rtnl_message(const void *buf, size_t len)
{
	struct ifinfomsg *msg;
	struct iplink_req *req;
	char ifname[IF_NAMESIZE];
	int index;

	while (len > 0) {
		const struct nlmsghdr *hdr = buf;

		if (!NLMSG_OK(hdr, len))
			break;

		switch (hdr->nlmsg_type) {
		case RTM_NEWLINK:
			req = g_slist_nth_data(pending_requests, 0);
			if (req == NULL)
				break;

			msg = (struct ifinfomsg *) NLMSG_DATA(hdr);
			parse_newlink_param(msg, IFA_PAYLOAD(hdr),
							&index, ifname);

			if (req->callback)
				req->callback(index, ifname, req->user_data);
			break;

		case NLMSG_ERROR:
			req = find_request(hdr->nlmsg_seq);
			if (req == NULL)
				break;

			DBG("nlmsg error req");

			if (req->callback)
				req->callback(-1, ifname, req->user_data);
			break;

		default:
			req = NULL;
			break;
		}

		len -= hdr->nlmsg_len;
		buf += hdr->nlmsg_len;

		if (req) {
			pending_requests = g_slist_remove(pending_requests,
								req);
			g_free(req);
		}
	}
}

static int add_attribute(struct nlmsghdr *n, unsigned int maxlen, int type,
				const void *data, int datalen)
{
	int len = RTA_LENGTH(datalen);
	struct rtattr *rta;

	if ((NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len)) > maxlen) {
		DBG("attribute to large for message %d %d %d",
				n->nlmsg_len, len, maxlen);
		return -1;
	}

	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, datalen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);

	return 0;
}

static inline void prep_rtnl_req(struct rtnl_msg *msg, int reqtype, __u32 seqnr)
{
	msg->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	msg->n.nlmsg_flags = NLM_F_REQUEST|NLM_F_CREATE|NLM_F_EXCL;
	msg->n.nlmsg_type = reqtype;
	msg->n.nlmsg_seq = seqnr;
	msg->i.ifi_family = AF_UNSPEC;
}

static gboolean netlink_event(GIOChannel *chan,
				GIOCondition cond, void *data)
{
	unsigned char buf[RTNL_MSG_SIZE];
	int len, sk;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR)) {
		rtnl_watch = 0;
		return FALSE;
	}

	sk = g_io_channel_unix_get_fd(rtnl_channel);

	len = recv(sk, buf, sizeof(buf), MSG_DONTWAIT);
	if (len < 0) {
		if (len == -EAGAIN)
			return TRUE;

		rtnl_watch = 0;
		return FALSE;
	}

	parse_rtnl_message(buf, len);

	return TRUE;
}

int caif_rtnl_init(void)
{
	struct sockaddr_nl addr;
	int sk, err;

	sk = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (sk < 0)
		return sk;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = RTMGRP_LINK;

	err = bind(sk, (struct sockaddr *) &addr, sizeof(addr));
	if (err < 0) {
		close(sk);
		return err;
	}

	rtnl_channel = g_io_channel_unix_new(sk);
	g_io_channel_set_flags(rtnl_channel, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_close_on_unref(rtnl_channel, TRUE);

	rtnl_watch = g_io_add_watch(rtnl_channel,
				G_IO_IN | G_IO_NVAL | G_IO_HUP | G_IO_ERR,
				netlink_event, NULL);

	return 0;
}

void caif_rtnl_exit(void)
{
	GSList *list;

	if (rtnl_watch > 0)
		g_source_remove(rtnl_watch);

	g_io_channel_unref(rtnl_channel);

	for (list = pending_requests; list; list = list->next) {
		struct iplink_req *req = list->data;
		g_free(req);
	}

	g_slist_free(pending_requests);
}

int caif_rtnl_create_interface(int type, int connid, int loop,
				caif_rtnl_create_cb_t cb, void *user_data)
{
	struct iplink_req *req;
	struct sockaddr_nl addr;
	struct rtnl_msg msg;
	struct rtattr *linkinfo;
	struct rtattr *data_start;
	int err, sk;

	req = g_try_new0(struct iplink_req, 1);
	if (req == NULL)
		return -ENOMEM;

	req->user_data = user_data;
	req->callback = cb;
	memset(&msg, 0, RTNL_MSG_SIZE);

	req->rtnlmsg_seqnr = ++rtnl_seqnr;
	prep_rtnl_req(&msg, RTM_NEWLINK, req->rtnlmsg_seqnr);

	linkinfo = NLMSG_TAIL(&msg.n);
	add_attribute(&msg.n, sizeof(msg), IFLA_LINKINFO,
			NULL, 0);
	add_attribute(&msg.n, sizeof(msg), IFLA_INFO_KIND,
			"caif", 4);
	data_start = NLMSG_TAIL(&msg.n);
	add_attribute(&msg.n, sizeof(msg), IFLA_INFO_DATA,
			NULL, 0);

	switch (type) {
	case IFLA_CAIF_IPV4_CONNID:
	case IFLA_CAIF_IPV6_CONNID:
		add_attribute(&msg.n, sizeof(msg),
				type, &connid,
				sizeof(connid));
		break;
	default:
		DBG("unsupported linktype");
		g_free(req);
		return -EINVAL;
	}

	if (loop)
		add_attribute(&msg.n, sizeof(msg),
				IFLA_CAIF_LOOPBACK, &loop, sizeof(loop));

	data_start->rta_len = (void *)NLMSG_TAIL(&msg.n) - (void *)data_start;
	linkinfo->rta_len = (void *)NLMSG_TAIL(&msg.n) - (void *)linkinfo;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;

	sk = g_io_channel_unix_get_fd(rtnl_channel);

	err = sendto(sk, &msg, msg.n.nlmsg_len, 0,
			(struct sockaddr *) &addr, sizeof(addr));
	if (err < 0) {
		g_free(req);
		return err;
	}

	pending_requests = g_slist_append(pending_requests, req);

	return 0;
}

int caif_rtnl_delete_interface(int index)
{
	struct sockaddr_nl addr;
	struct rtnl_msg msg;
	int err, sk;

	if (index < 0)
		return -EINVAL;

	sk = g_io_channel_unix_get_fd(rtnl_channel);

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;

	memset(&msg, 0, sizeof(msg));
	prep_rtnl_req(&msg, RTM_DELLINK, ++rtnl_seqnr);
	msg.i.ifi_index = index;

	err = sendto(sk, &msg, msg.n.nlmsg_len, 0,
			(struct sockaddr *) &addr, sizeof(addr));
	if (err < 0)
		return err;

	return 0;
}
