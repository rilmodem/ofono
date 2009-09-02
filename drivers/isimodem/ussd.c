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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ussd.h>

#include "isi.h"

#define PN_SS			0x06

struct ussd_data {
	GIsiClient *client;
	struct isi_version version;
};

static void isi_request(struct ofono_ussd *ussd, const char *str,
				ofono_ussd_cb_t cb, void *data)
{
}

static void isi_cancel(struct ofono_ussd *ussd,
				ofono_ussd_cb_t cb, void *data)
{
}

static int isi_ussd_probe(struct ofono_ussd *ussd, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct ussd_data *data = g_try_new0(struct ussd_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SS);
	if (!data->client)
		return -ENOMEM;

	ofono_ussd_set_data(ussd, data);

	return 0;
}

static void isi_ussd_remove(struct ofono_ussd *ussd)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_ussd_driver driver = {
	.name			= "isimodem",
	.probe			= isi_ussd_probe,
	.remove			= isi_ussd_remove,
	.request		= isi_request,
	.cancel			= isi_cancel
};

void isi_ussd_init()
{
	ofono_ussd_driver_register(&driver);
}

void isi_ussd_exit()
{
	ofono_ussd_driver_unregister(&driver);
}
