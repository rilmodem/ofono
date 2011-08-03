/*
 *
 *  oFono - Open Source Telephony
 *
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

#include <glib.h>
#include <gatchat.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>

#include "dunmodem.h"

static void dun_gprs_set_attached(struct ofono_gprs *gprs, int attached,
						ofono_gprs_cb_t cb, void *data)
{
	DBG("");

	CALLBACK_WITH_SUCCESS(cb, data);
}

static gboolean dun_gprs_finish_registration(gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;

	ofono_gprs_register(gprs);

	return FALSE;
}

static int dun_gprs_probe(struct ofono_gprs *gprs,
					unsigned int vendor, void *data)
{
	DBG("");

	g_idle_add(dun_gprs_finish_registration, gprs);

	return 0;
}

static void dun_gprs_remove(struct ofono_gprs *gprs)
{
	DBG("");
}

static void dun_gprs_attached_status(struct ofono_gprs *gprs,
						ofono_gprs_status_cb_t cb,
						void *data)
{
	DBG("");

	CALLBACK_WITH_SUCCESS(cb, 1, data);
}

static struct ofono_gprs_driver driver = {
	.name			= "dunmodem",
	.probe			= dun_gprs_probe,
	.remove			= dun_gprs_remove,
	.set_attached		= dun_gprs_set_attached,
	.attached_status	= dun_gprs_attached_status,
};

void dun_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void dun_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
