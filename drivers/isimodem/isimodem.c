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
#include <ofono/phonebook.h>

#include "isi.h"

struct isi_data {
	struct ofono_modem *modem;
	GIsiModem *idx;
};

static GPhonetNetlink *link = NULL;
static GSList *g_modems = NULL;


void dump_msg(const unsigned char *msg, size_t len)
{
	char dumpstr[len * 5 + len / 10 + 1];
	size_t i;

	for (i = 0; i < len; i++)
		sprintf(dumpstr + i * 5, "0x%02x%s",
			msg[i], (i + 1) % 10 == 0 ? "\n" : " ");

	DBG("%zd bytes:\n%s", len, dumpstr);
}

static struct isi_data *find_modem_by_idx(GSList *modems, GIsiModem *idx)
{
	GSList *m = NULL;

	for (m = g_modems; m; m = m->next) {
		struct isi_data *isi = m->data;

		if (isi->idx == idx)
			return isi;
	}
	return NULL;
}

static void netlink_status_cb(bool up, uint8_t addr, GIsiModem *idx,
				void *data)
{
	struct isi_data *isi = find_modem_by_idx(g_modems, idx);

	DBG("PhoNet is %s, addr=0x%02x, idx=%p",
		up ? "up" : "down", addr, idx);
	

	if (up) {
		char node[128];

		if (isi) {
			DBG("Modem already registered: (0x%02x)",
				g_isi_modem_index(idx));
			return;
		}

		isi = g_new0(struct isi_data, 1);
		if (!isi)
			return;

		sprintf(node, "isi%p", idx);

		isi->idx = idx;
		isi->modem = ofono_modem_create(node, "isi");

		if (!isi->modem) {
			g_free(isi);
			return;
		}

		g_modems = g_slist_prepend(g_modems, isi);

		ofono_modem_set_data(isi->modem, isi);
		ofono_modem_set_powered(isi->modem, TRUE);
		ofono_modem_register(isi->modem);
 	} else {

		if (!isi) {
			DBG("Unknown modem: (0x%02x)",
				g_isi_modem_index(idx));
			return;
		}

		ofono_modem_remove(isi->modem);

		g_modems = g_slist_remove(g_modems, isi);
	}
}

static int isi_modem_probe(struct ofono_modem *modem)
{
	return 0;
}

static int isi_modem_remove(struct ofono_modem *modem)
{
	return 0;
}

static int isi_modem_enable(struct ofono_modem *modem)
{
	return 0;
}

static int isi_modem_disable(struct ofono_modem *modem)
{
	return 0;
}

static int isi_modem_populate(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	ofono_devinfo_create(isi->modem, "isi", isi->idx);
	ofono_phonebook_create(isi->modem, "isi", isi->idx);

	return 0;
}

static struct ofono_modem_driver driver = {
	.name = "isi",
	.probe = isi_modem_probe,
	.remove = isi_modem_remove,
	.enable = isi_modem_enable,
	.disable = isi_modem_disable,
	.populate = isi_modem_populate,
};

static int isimodem_init(void)
{
	link = g_pn_netlink_start(netlink_status_cb, NULL);

	isi_devinfo_init();
	isi_phonebook_init();

	ofono_modem_driver_register(&driver);

	return 0;
}

static void isimodem_exit(void)
{
	GSList *m;

	for (m = g_modems; m; m = m->next) {
		struct isi_data *isi = m->data;

		ofono_modem_remove(isi->modem);

		g_free(isi);
	}

	g_slist_free(g_modems);

	if (link) {
		g_pn_netlink_stop(link);
		link = NULL;
	}

	ofono_modem_driver_unregister(&driver);

	isi_devinfo_exit();
	isi_phonebook_exit();
}

OFONO_PLUGIN_DEFINE(isimodem, "PhoNet / ISI modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, isimodem_init, isimodem_exit)
