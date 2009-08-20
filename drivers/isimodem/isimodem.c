/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <gisi/netlink.h>
#include <gisi/client.h>

#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "isi.h"

static GPhonetNetlink *pn_link = NULL;
static struct isi_data *isi = NULL;

void dump_msg(const unsigned char *msg, size_t len)
{
	char dumpstr[len * 5 + len / 10 + 1];
	size_t i;

	for (i = 0; i < len; i++)
		sprintf(dumpstr + i * 5, "0x%02x%s",
			msg[i], (i + 1) % 10 == 0 ? "\n" : " ");

	DBG("%zd bytes:\n%s", len, dumpstr);
}

static void netlink_status_cb(bool up, uint8_t addr, GIsiModem *idx,
				void *data)
{
	struct isi_data *isi = data;

	DBG("PhoNet is %s, addr=0x%02x, idx=%p",
		up ? "up" : "down", addr, idx);

	if (up) {
		if (!isi->modem) {
			isi->modem = ofono_modem_register();
			if (!isi->modem)
				return;

			ofono_modem_set_userdata(isi->modem, isi);
			ofono_devinfo_create(isi->modem, "isi", idx);
			ofono_phonebook_create(isi->modem, "isi", NULL);
		}
	} else {
		if (isi->modem) {
			ofono_modem_unregister(isi->modem);
			isi->modem = NULL;
		}
	}
}

static int isimodem_init(void)
{
	isi = g_new0(struct isi_data, 1);

	pn_link = g_pn_netlink_start(netlink_status_cb, isi);

	isi_phonebook_init();

	return 0;
}

static void isimodem_exit(void)
{
	if (pn_link) {
		g_pn_netlink_stop(pn_link);
		pn_link = NULL;
	}

	isi_phonebook_exit();

	g_free(isi);
}

OFONO_PLUGIN_DEFINE(isimodem, "PhoNet / ISI modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, isimodem_init, isimodem_exit)
