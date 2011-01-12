/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ssn.h>

#include "isimodem.h"
#include "ss.h"

struct ssn_data {
	GIsiClient *client;
};

static int isi_ssn_probe(struct ofono_ssn *ssn, unsigned int vendor,
				void *user)
{
	GIsiModem *modem = user;
	struct ssn_data *sd;

	sd = g_try_new0(struct ssn_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->client = g_isi_client_create(modem, PN_SS);
	if (sd->client == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	ofono_ssn_set_data(ssn, sd);

	return 0;
}

static void isi_ssn_remove(struct ofono_ssn *ssn)
{
	struct ssn_data *data = ofono_ssn_get_data(ssn);

	ofono_ssn_set_data(ssn, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_ssn_driver driver = {
	.name			= "isimodem",
	.probe			= isi_ssn_probe,
	.remove			= isi_ssn_remove
};

void isi_ssn_init(void)
{
	ofono_ssn_driver_register(&driver);
}

void isi_ssn_exit(void)
{
	ofono_ssn_driver_unregister(&driver);
}
