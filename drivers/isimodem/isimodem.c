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
#include <glib.h>

#include <gisi/netlink.h>

#include <ofono/plugin.h>
#include <ofono/log.h>
#include "driver.h"

#include "isi.h"

static GPhonetNetlink *pn_link;

void dump_msg(const unsigned char *msg, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		printf("0x%02x, ", msg[i]);
	printf("\n");
}

static void isi_query_manufacturer(struct ofono_modem *modem,
					ofono_modem_attribute_query_cb_t cb,
					void *data)
{
	struct ofono_error err;
	cb(&err, "Nokia", data);
}

static void isi_query_model(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb,
				void *data)
{
	struct ofono_error err;
	cb(&err, "", data);
}

static void isi_query_revision(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb,
				void *data)
{
	struct ofono_error err;
	cb(&err, "", data);
}

static void isi_query_serial(struct ofono_modem *modem,
				ofono_modem_attribute_query_cb_t cb,
				void *data)
{
	struct ofono_error err;
	cb(&err, "", data);
}

static struct ofono_modem_attribute_ops ops = {
	.query_manufacturer = isi_query_manufacturer,
	.query_model = isi_query_model,
	.query_revision = isi_query_revision,
	.query_serial = isi_query_serial
};

static void netlink_status_cb(bool up, uint8_t addr, unsigned idx,
				void *data)
{
	struct isi_data *isi = data;

	DBG("PhoNet is %s, addr=0x%02x, idx=%d",
		up ? "up" : "down", addr, idx);

	if (up) {
		isi->modem = ofono_modem_register(&ops);
		if (!isi->modem)
			return;
		ofono_modem_set_userdata(isi->modem, isi);
	} else {
		ofono_modem_unregister(isi->modem);
	}
}

static int isimodem_init(void)
{
	struct isi_data *isi;

	DBG("");

	isi = g_new0(struct isi_data, 1);
	pn_link = g_pn_netlink_start(netlink_status_cb, (void *)isi);
	
	return 0;
}

static void isimodem_exit(void)
{
	DBG("");

	g_pn_netlink_stop(pn_link);
}

OFONO_PLUGIN_DEFINE(isimodem, "PhoNet / ISI modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, isimodem_init, isimodem_exit)
