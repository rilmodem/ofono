/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) 2013  Canonical Ltd.
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

#include <errno.h>
#include <string.h>
#include <glib.h>
#include "ofono.h"
#include "sim-mnclength.h"

static GSList *g_drivers = NULL;

int __ofono_sim_mnclength_get_mnclength(const char *imsi)
{
	GSList *d;
	int mnclen = -ENOTSUP;

	for (d = g_drivers; d != NULL; d = d->next) {
		const struct ofono_sim_mnclength_driver *driver = d->data;

		if (driver->get_mnclength == NULL)
			continue;

		DBG("Calling mnclength plugin '%s'", driver->name);

		mnclen = driver->get_mnclength(imsi);
		if (mnclen <= 0)
			continue;

		return mnclen;
	}

	return mnclen;
}

int ofono_sim_mnclength_driver_register(
			struct ofono_sim_mnclength_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_prepend(g_drivers, driver);
	return 0;
}

void ofono_sim_mnclength_driver_unregister(
			const struct ofono_sim_mnclength_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_remove(g_drivers, driver);
}
