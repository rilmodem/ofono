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
#include <ofono/sim.h>

#include "isi.h"

#define PN_SIM			0x09

struct sim_data {
	GIsiClient *client;
	struct isi_version version;
};

static void isi_read_file_info(struct ofono_sim *sim, int fileid,
				ofono_sim_file_info_cb_t cb, void *data)
{
}

static void isi_read_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					ofono_sim_read_cb_t cb, void *data)
{
}

static void isi_read_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
}

static void isi_read_file_cyclic(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
}

static void isi_write_file_transparent(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
}

static void isi_write_file_linear(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
}

static void isi_write_file_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
}

static void isi_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *data)
{
}

static int isi_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct sim_data *data = g_try_new0(struct sim_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SIM);
	if (!data->client)
		return -ENOMEM;

	ofono_sim_set_data(sim, data);

	return 0;
}

static void isi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_sim_driver driver = {
	.name			= "isimodem",
	.probe			= isi_sim_probe,
	.remove			= isi_sim_remove,
	.read_file_info		= isi_read_file_info,
	.read_file_transparent	= isi_read_file_transparent,
	.read_file_linear	= isi_read_file_linear,
	.read_file_cyclic	= isi_read_file_cyclic,
	.write_file_transparent	= isi_write_file_transparent,
	.write_file_linear	= isi_write_file_linear,
	.write_file_cyclic	= isi_write_file_cyclic,
	.read_imsi		= isi_read_imsi
};

void isi_sim_init()
{
	ofono_sim_driver_register(&driver);
}

void isi_sim_exit()
{
	ofono_sim_driver_unregister(&driver);
}
