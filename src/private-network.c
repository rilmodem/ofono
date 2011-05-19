/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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
#include "ofono.h"

static GSList *g_drivers = NULL;

void __ofono_private_network_release(int id)
{
	GSList *d;

	DBG("");

	for (d = g_drivers; d; d = d->next) {
		const struct ofono_private_network_driver *driver = d->data;

		if (!driver->release)
			continue;

		driver->release(id);

		break;
	}
}

ofono_bool_t __ofono_private_network_request(ofono_private_network_cb_t cb,
						int *id, void *data)
{
	GSList *d;
	int uid;

	DBG("");

	for (d = g_drivers; d; d = d->next) {
		const struct ofono_private_network_driver *driver = d->data;

		if (!driver->request)
			continue;

		uid = driver->request(cb, data);
		if (uid <= 0)
			continue;

		*id = uid;
		return TRUE;
	}

	return FALSE;
}

int ofono_private_network_driver_register(
			const struct ofono_private_network_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_private_network_driver_unregister(
			const struct ofono_private_network_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}
