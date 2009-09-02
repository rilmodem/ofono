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
#include <ofono/call-settings.h>

#include "isi.h"

#define PN_SS			0x06

struct call_settings_data {
	GIsiClient *client;
	struct isi_version version;
};

static void isi_clip_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
}

static void isi_colp_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
}

static void isi_clir_query(struct ofono_call_settings *cs,
				ofono_call_settings_clir_cb_t cb, void *data)
{
}

static void isi_colr_query(struct ofono_call_settings *cs,
				ofono_call_settings_status_cb_t cb, void *data)
{
}

static void isi_clir_set(struct ofono_call_settings *cs, int mode,
				ofono_call_settings_set_cb_t cb, void *data)
{
}

static void isi_cw_query(struct ofono_call_settings *cs, int cls,
			ofono_call_settings_status_cb_t cb, void *data)
{
}

static void isi_cw_set(struct ofono_call_settings *cs, int mode, int cls,
			ofono_call_settings_set_cb_t cb, void *data)
{
}

static int isi_call_settings_probe(struct ofono_call_settings *cs, unsigned int vendor,
					void *user)
{
	GIsiModem *idx = user;
	struct call_settings_data *data;

	data = g_try_new0(struct call_settings_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SS);

	if (!data->client)
		return -ENOMEM;

	ofono_call_settings_set_data(cs, data);

	return 0;
}

static void isi_call_settings_remove(struct ofono_call_settings *cs)
{
	struct call_settings_data *data = ofono_call_settings_get_data(cs);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_call_settings_driver driver = {
	.name			= "isimodem",
	.probe			= isi_call_settings_probe,
	.remove			= isi_call_settings_remove,
	.clip_query		= isi_clip_query,
	.colp_query		= isi_colp_query,
	.clir_query		= isi_clir_query,
	.colr_query		= isi_colr_query,
	.clir_set		= isi_clir_set,
	.cw_query		= isi_cw_query,
	.cw_set			= isi_cw_set
};

void isi_call_settings_init()
{
	ofono_call_settings_driver_register(&driver);
}

void isi_call_settings_exit()
{
	ofono_call_settings_driver_unregister(&driver);
}
