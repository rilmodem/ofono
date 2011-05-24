/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib.h>

#include <fcntl.h>
#include <errno.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>

#define OFONO_API_SUBJECT_TO_CHANGE

#include <ofono/modem.h>
#include <ofono/types.h>
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/private-network.h>

#define SERVER_ADDRESS		"192.168.1.1"
#define DNS_SERVER_1		"10.10.10.10"
#define DNS_SERVER_2		"10.10.10.11"
#define PEER_ADDRESS_PREFIX	"192.168.1."

static int next_peer = 2;

struct req_data {
	ofono_private_network_cb_t cb;
	void *userdata;
};

static gboolean request_cb(gpointer data)
{
	struct req_data *rd = data;
	struct ofono_private_network_settings pns;
	struct ifreq ifr;
	int fd, err;
	char ip[16];

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0)
		goto error;

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strcpy(ifr.ifr_name, "ppp%d");

	err = ioctl(fd, TUNSETIFF, (void *) &ifr);
	if (err < 0)
		goto error;

	sprintf(ip, "%s%d", PEER_ADDRESS_PREFIX, next_peer++);

	pns.fd = fd;
	pns.server_ip = SERVER_ADDRESS;
	pns.peer_ip = ip;
	pns.primary_dns = DNS_SERVER_1;
	pns.secondary_dns = DNS_SERVER_2;

	rd->cb(&pns, rd->userdata);

	return FALSE;

error:
	if (fd >= 0)
		close(fd);

	rd->cb(NULL, rd->userdata);

	return FALSE;
}

static int example_request(ofono_private_network_cb_t cb, void *data)
{
	struct req_data *rd = g_new0(struct req_data, 1);

	DBG("");

	rd->cb = cb;
	rd->userdata = data;

	return g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 2, request_cb,
						rd, (GDestroyNotify) g_free);
}

static void example_release(int id)
{
	DBG("");

	g_source_remove(id);
}

static struct ofono_private_network_driver example_driver = {
	.name		= "Example Private Network Driver",
	.request	= example_request,
	.release	= example_release,
};

static int example_private_network_init(void)
{
	return ofono_private_network_driver_register(&example_driver);
}

static void example_private_network_exit(void)
{
	ofono_private_network_driver_unregister(&example_driver);
}

OFONO_PLUGIN_DEFINE(example_private_network, "Example Private Network Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_LOW,
			example_private_network_init,
			example_private_network_exit)
