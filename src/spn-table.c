/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) 2014  Canonical Ltd.
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
#include "spn-table.h"

static GSList *g_drivers = NULL;

const char *__ofono_spn_table_get_spn(const char *numeric)
{
	GSList *d;
	const char *spn = NULL;

	for (d = g_drivers; d != NULL; d = d->next) {
		const struct ofono_spn_table_driver *driver = d->data;

		if (driver->get_spn == NULL)
			continue;

		DBG("Calling spntable plugin '%s'", driver->name);

		if ((spn = driver->get_spn(numeric)) == NULL)
			continue;

		return spn;
	}

	return spn;
}

int ofono_spn_table_driver_register(struct ofono_spn_table_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_prepend(g_drivers, driver);
	return 0;
}

void ofono_spn_table_driver_unregister(
			const struct ofono_spn_table_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_remove(g_drivers, driver);
}
