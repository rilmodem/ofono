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
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#include <gdbus.h>

#include "ofono.h"

static GSList *g_drivers;

struct ofono_cdma_netreg {
	const struct ofono_cdma_netreg_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static DBusMessage *network_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable cdma_netreg_manager_methods[] = {
	{ "GetProperties",  "",  "a{sv}",	network_get_properties },
	{ }
};

static GDBusSignalTable cdma_netreg_manager_signals[] = {
	{ }
};

int ofono_cdma_netreg_driver_register(const struct ofono_cdma_netreg_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_cdma_netreg_driver_unregister(
				const struct ofono_cdma_netreg_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void cdma_netreg_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	g_dbus_unregister_interface(conn, path,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE);

	ofono_modem_remove_interface(modem,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE);
}

static void cdma_netreg_remove(struct ofono_atom *atom)
{
	struct ofono_cdma_netreg *cdma_netreg = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cdma_netreg == NULL)
		return;

	if (cdma_netreg->driver && cdma_netreg->driver->remove)
		cdma_netreg->driver->remove(cdma_netreg);

	g_free(cdma_netreg);
}

struct ofono_cdma_netreg *ofono_cdma_netreg_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data)
{
	struct ofono_cdma_netreg *cdma_netreg;
	GSList *l;

	if (driver == NULL)
		return NULL;

	cdma_netreg = g_try_new0(struct ofono_cdma_netreg, 1);
	if (cdma_netreg == NULL)
		return NULL;

	cdma_netreg->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPE_CDMA_NETREG,
					cdma_netreg_remove, cdma_netreg);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_cdma_netreg_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cdma_netreg, vendor, data) < 0)
			continue;

		cdma_netreg->driver = drv;
		break;
	}

	return cdma_netreg;
}

void ofono_cdma_netreg_register(struct ofono_cdma_netreg *cdma_netreg)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cdma_netreg->atom);
	const char *path = __ofono_atom_get_path(cdma_netreg->atom);

	if (!g_dbus_register_interface(conn, path,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE,
				cdma_netreg_manager_methods,
				cdma_netreg_manager_signals,
				NULL, cdma_netreg, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE);

	__ofono_atom_register(cdma_netreg->atom, cdma_netreg_unregister);
}

void ofono_cdma_netreg_remove(struct ofono_cdma_netreg *cdma_netreg)
{
	__ofono_atom_free(cdma_netreg->atom);
}

void ofono_cdma_netreg_set_data(struct ofono_cdma_netreg *cdma_netreg,
					void *data)
{
	cdma_netreg->driver_data = data;
}

void *ofono_cdma_netreg_get_data(struct ofono_cdma_netreg *cdma_netreg)
{
	return cdma_netreg->driver_data;
}
