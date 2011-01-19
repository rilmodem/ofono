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

#define _GNU_SOURCE

#include <glib.h>
#include <errno.h>
#include <unistd.h>

#include "ofono.h"

#include "simutil.h"

static GSList *g_drivers = NULL;

struct ofono_sim_auth {
	const struct ofono_sim_auth_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

int ofono_sim_auth_driver_register(const struct ofono_sim_auth_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_sim_auth_driver_unregister(const struct ofono_sim_auth_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void sim_auth_unregister(struct ofono_atom *atom)
{
}

static void sim_auth_remove(struct ofono_atom *atom)
{
	struct ofono_sim_auth *sa = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (sa == NULL)
		return;

	if (sa->driver && sa->driver->remove)
		sa->driver->remove(sa);

	g_free(sa);
}

struct ofono_sim_auth *ofono_sim_auth_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data)
{
	struct ofono_sim_auth *sa;
	GSList *l;

	if (driver == NULL)
		return NULL;

	sa = g_try_new0(struct ofono_sim_auth, 1);

	if (sa == NULL)
		return NULL;

	sa->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_SIM_AUTH,
						sim_auth_remove, sa);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_sim_auth_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(sa, vendor, data) < 0)
			continue;

		sa->driver = drv;
		break;
	}

	return sa;
}

void ofono_sim_auth_register(struct ofono_sim_auth *sa)
{
	__ofono_atom_register(sa->atom, sim_auth_unregister);
}

void ofono_sim_auth_remove(struct ofono_sim_auth *sa)
{
	__ofono_atom_free(sa->atom);
}

void ofono_sim_auth_set_data(struct ofono_sim_auth *sa, void *data)
{
	sa->driver_data = data;
}

void *ofono_sim_auth_get_data(struct ofono_sim_auth *sa)
{
	return sa->driver_data;
}
