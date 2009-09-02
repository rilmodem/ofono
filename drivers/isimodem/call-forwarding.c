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
#include <ofono/call-forwarding.h>

#include "isi.h"

#define PN_SS			0x06

struct call_forwarding_data {
	GIsiClient *client;
	struct isi_version version;
};

static void isi_activation(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
}

static void isi_registration(struct ofono_call_forwarding *cf,
				int type, int cls,
				const struct ofono_phone_number *number,
				int time,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
}

static void isi_deactivation(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
}

static void isi_erasure(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
}

static void isi_query(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_query_cb_t cb,
				void *data)
{
}

static int isi_call_forwarding_probe(struct ofono_call_forwarding *cf,
					unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct call_forwarding_data *data;

	data = g_try_new0(struct call_forwarding_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SS);
	if (!data->client)
		return -ENOMEM;

	ofono_call_forwarding_set_data(cf, data);

	return 0;
}

static void isi_call_forwarding_remove(struct ofono_call_forwarding *cf)
{
	struct call_forwarding_data *data = ofono_call_forwarding_get_data(cf);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_call_forwarding_driver driver = {
	.name			= "isimodem",
	.probe			= isi_call_forwarding_probe,
	.remove			= isi_call_forwarding_remove,
	.activation		= isi_activation,
	.registration		= isi_registration,
	.deactivation		= isi_deactivation,
	.erasure		= isi_erasure,
	.query			= isi_query
};

void isi_call_forwarding_init()
{
	ofono_call_forwarding_driver_register(&driver);
}

void isi_call_forwarding_exit()
{
	ofono_call_forwarding_driver_unregister(&driver);
}
