/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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
#include <stdio.h>

#include <glib.h>

#include "ofono.h"

static GSList *nettime_drivers = NULL;

static struct ofono_nettime_context *nettime_context_create(
					struct ofono_modem *modem,
					struct ofono_nettime_driver *driver)
{
	struct ofono_nettime_context *context;

	if (driver->probe == NULL)
		return NULL;

	context = g_try_new0(struct ofono_nettime_context, 1);

	if (context == NULL)
		return NULL;

	context->driver = driver;
	context->modem = modem;

	if (driver->probe(context) < 0) {
		g_free(context);
		return NULL;
	}

	return context;
}

static void context_remove(struct ofono_atom *atom)
{
	struct ofono_nettime_context *context = __ofono_atom_get_data(atom);

	if (context->driver->remove)
		context->driver->remove(context);

	g_free(context);
}

void __ofono_nettime_probe_drivers(struct ofono_modem *modem)
{
	struct ofono_nettime_driver *driver;
	struct ofono_nettime_context *context;
	GSList *l;

	for (l = nettime_drivers; l; l = l->next) {
		driver = l->data;

		context = nettime_context_create(modem, driver);
		if (context == NULL)
			continue;

		__ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_NETTIME,
						context_remove, context);
	}
}

static void nettime_info_received(struct ofono_atom *atom, void *data)
{
	struct ofono_nettime_context *context = __ofono_atom_get_data(atom);
	struct ofono_network_time *info = data;

	if (context->driver->info_received == NULL)
		return;

	context->driver->info_received(context, info);
}

void __ofono_nettime_info_received(struct ofono_modem *modem,
					struct ofono_network_time *info)
{
	__ofono_modem_foreach_atom(modem, OFONO_ATOM_TYPE_NETTIME,
					nettime_info_received, info);
}

int ofono_nettime_driver_register(const struct ofono_nettime_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	nettime_drivers = g_slist_prepend(nettime_drivers, (void *) driver);

	return 0;
}

void ofono_nettime_driver_unregister(const struct ofono_nettime_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	nettime_drivers = g_slist_remove(nettime_drivers, driver);
}
