/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <gisi/netlink.h>
#include <gisi/client.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

static GPhonetNetlink *link = NULL;

/*
 * Add or remove isimodems
 * when usbpn* phonet interfaces are added/removed
 */
static void usbpn_status_cb(GIsiModem *idx,
				GPhonetLinkState st,
				char const ifname[],
				void *data)
{
	struct ofono_modem *modem;
	int error;

	DBG("Phonet link %s (%u) is %s",
		ifname, g_isi_modem_index(idx),
		st == PN_LINK_REMOVED ? "removed" :
		st == PN_LINK_DOWN ? "down" : "up");

	/* Expect phonet interface name usbpn<idx> */
	if (strncmp(ifname, "usbpn", 5) ||
		ifname[5 + strspn(ifname + 5, "0123456789")])
		return;

	if (st == PN_LINK_REMOVED)
		return;

	if (g_pn_netlink_by_modem(idx)) {
		DBG("Modem for interface %s already exists", ifname);
		return;
	}

	error = g_pn_netlink_set_address(idx, PN_DEV_PC);
	if (error && error != -EEXIST) {
		DBG("g_pn_netlink_set_address: %s\n", strerror(-error));
		return;
	}

	modem = ofono_modem_create(NULL, "isimodem");
	if (!modem)
		return;

	ofono_modem_set_string(modem, "Interface", ifname);

	if (ofono_modem_register(modem) == 0)
		DBG("Done regging modem %s", ofono_modem_get_path(modem));
	else
		ofono_modem_remove(modem);
}

static int usbpn_init(void)
{
	link = g_pn_netlink_start(NULL, usbpn_status_cb, NULL);
	return 0;
}

static void usbpn_exit(void)
{
	g_pn_netlink_stop(link);
	link = NULL;
}

OFONO_PLUGIN_DEFINE(usbpnmodem, "Hotplug driver for USB Phonet modems", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			usbpn_init, usbpn_exit)
