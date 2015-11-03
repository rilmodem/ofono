/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  Canonical Ltd.
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
#include "system-settings.h"

static GSList *g_drivers = NULL;

char *__ofono_system_settings_get_string_value(const char *name)
{
	GSList *d;
	char *value = NULL;

	for (d = g_drivers; d != NULL; d = d->next) {
		const struct ofono_system_settings_driver *driver = d->data;

		if (driver->get_string_value == NULL)
			continue;

		DBG("Calling system settings plugin '%s'", driver->name);

		value = driver->get_string_value(name);
		if (value == NULL)
			continue;

		DBG("property %s value %s", name, value);

		return value;
	}

	return value;
}

int ofono_system_settings_driver_register(
			struct ofono_system_settings_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_prepend(g_drivers, driver);
	return 0;
}

void ofono_system_settings_driver_unregister(
			const struct ofono_system_settings_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_remove(g_drivers, driver);
}
