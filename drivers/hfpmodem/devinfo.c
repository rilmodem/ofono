/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  BMW Car IT GmbH. All rights reserved.
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

#include <string.h>

#include <glib.h>
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "hfpmodem.h"

struct devinfo_data {
	char *device_address;
	guint register_source;
};

static void hfp_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	CALLBACK_WITH_SUCCESS(cb, dev->device_address, data);
}

static gboolean hfp_devinfo_register(gpointer user_data)
{
	struct ofono_devinfo *info = user_data;
	struct devinfo_data *dd = ofono_devinfo_get_data(info);

	dd->register_source = 0;

	ofono_devinfo_register(info);

	return FALSE;
}

static int hfp_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *user)
{
	const char *device_address = user;
	struct devinfo_data *dd;

	dd = g_new0(struct devinfo_data, 1);
	dd->device_address = g_strdup(device_address);

	ofono_devinfo_set_data(info, dd);

	dd->register_source = g_idle_add(hfp_devinfo_register, info);
	return 0;
}

static void hfp_devinfo_remove(struct ofono_devinfo *info)
{
	struct devinfo_data *dd = ofono_devinfo_get_data(info);

	ofono_devinfo_set_data(info, NULL);
	if (dd == NULL)
		return;

	if (dd->register_source != 0)
		g_source_remove(dd->register_source);

	g_free(dd->device_address);
	g_free(dd);
}

static struct ofono_devinfo_driver driver = {
	.name			= "hfpmodem",
	.probe			= hfp_devinfo_probe,
	.remove			= hfp_devinfo_remove,
	.query_serial		= hfp_query_serial
};

void hfp_devinfo_init(void)
{
	ofono_devinfo_driver_register(&driver);
}

void hfp_devinfo_exit(void)
{
	ofono_devinfo_driver_unregister(&driver);
}
