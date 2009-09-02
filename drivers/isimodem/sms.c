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
#include <ofono/sms.h>

#include "isi.h"

#define PN_SMS			0x02

struct sms_data {
	GIsiClient *client;
	struct isi_version version;
};

static void isi_sca_query(struct ofono_sms *sms, ofono_sms_sca_query_cb_t cb,
				void *data)
{
}

static void isi_sca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *data)
{
}

static void isi_submit(struct ofono_sms *sms, unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *data)
{
}

static int isi_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct sms_data *data = g_try_new0(struct sms_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SMS);
	if (!data->client)
		return -ENOMEM;

	ofono_sms_set_data(sms, data);

	return 0;
}

static void isi_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_sms_driver driver = {
	.name			= "isimodem",
	.probe			= isi_sms_probe,
	.remove			= isi_sms_remove,
	.sca_query		= isi_sca_query,
	.sca_set		= isi_sca_set,
	.submit			= isi_submit
};

void isi_sms_init()
{
	ofono_sms_driver_register(&driver);
}

void isi_sms_exit()
{
	ofono_sms_driver_unregister(&driver);
}
