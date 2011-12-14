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
#include <string.h>

#include <gdbus.h>

#include "ofono.h"

static GSList *g_drivers;

struct ofono_cdma_netreg {
	enum cdma_netreg_status status;
	int strength;
	int hdr_strength;
	const struct ofono_cdma_netreg_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	char *provider_name;
	char *sid;
};

static const char *cdma_netreg_status_to_string(enum cdma_netreg_status status)
{
	switch (status) {
	case CDMA_NETWORK_REGISTRATION_STATUS_NOT_REGISTERED:
		return "unregistered";
	case CDMA_NETWORK_REGISTRATION_STATUS_REGISTERED:
		return "registered";
	case CDMA_NETWORK_REGISTRATION_STATUS_ROAMING:
		return "roaming";
	}

	return "";
}

static DBusMessage *network_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_cdma_netreg *cdma_netreg = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	const char *status = cdma_netreg_status_to_string(cdma_netreg->status);

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "Status", DBUS_TYPE_STRING, &status);

	if (cdma_netreg->strength != -1) {
		unsigned char strength = cdma_netreg->strength;

		ofono_dbus_dict_append(&dict, "Strength", DBUS_TYPE_BYTE,
					&strength);
	}

	if (cdma_netreg->hdr_strength != -1) {
		unsigned char strength = cdma_netreg->hdr_strength;

		ofono_dbus_dict_append(&dict, "DataStrength", DBUS_TYPE_BYTE,
					&strength);
	}

	if (cdma_netreg->sid)
		ofono_dbus_dict_append(&dict, "SystemIdentifier",
						DBUS_TYPE_STRING,
						&cdma_netreg->sid);

	if (cdma_netreg->provider_name)
		ofono_dbus_dict_append(&dict, "Name", DBUS_TYPE_STRING,
						&cdma_netreg->provider_name);

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

static void serving_system_callback(const struct ofono_error *error,
					const char *sid, void *data)
{
	struct ofono_cdma_netreg *cdma_netreg = data;
	const char *path = __ofono_atom_get_path(cdma_netreg->atom);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (cdma_netreg->status != CDMA_NETWORK_REGISTRATION_STATUS_REGISTERED
			&& cdma_netreg->status !=
				CDMA_NETWORK_REGISTRATION_STATUS_ROAMING)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during serving system query");
		return;
	}

	DBG("Serving system Identifier: %s", sid);

	if (cdma_netreg->sid != NULL && !strcmp(cdma_netreg->sid, sid))
		return;

	g_free(cdma_netreg->provider_name);
	g_free(cdma_netreg->sid);
	cdma_netreg->provider_name = NULL;
	cdma_netreg->sid = g_strdup(sid);

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE,
				"SystemIdentifier", DBUS_TYPE_STRING,
				&cdma_netreg->sid);

	if (__ofono_cdma_provision_get_name(sid,
				&cdma_netreg->provider_name) == FALSE) {
		ofono_warn("Provider name not found");
		return;
	}

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE,
				"Name", DBUS_TYPE_STRING,
				&cdma_netreg->provider_name);
}

static void set_registration_status(struct ofono_cdma_netreg *cdma_netreg,
						enum cdma_netreg_status status)
{
	const char *str_status = cdma_netreg_status_to_string(status);
	const char *path = __ofono_atom_get_path(cdma_netreg->atom);
	DBusConnection *conn = ofono_dbus_get_connection();

	cdma_netreg->status = status;

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE,
				"Status", DBUS_TYPE_STRING,
				&str_status);

	if (cdma_netreg->status == CDMA_NETWORK_REGISTRATION_STATUS_REGISTERED
			|| cdma_netreg->status ==
				CDMA_NETWORK_REGISTRATION_STATUS_ROAMING)
		if (cdma_netreg->driver->serving_system != NULL)
			cdma_netreg->driver->serving_system(cdma_netreg,
				serving_system_callback, cdma_netreg);
}

void ofono_cdma_netreg_status_notify(struct ofono_cdma_netreg *cdma_netreg,
					enum cdma_netreg_status status)
{
	if (cdma_netreg == NULL)
		return;

	if (cdma_netreg->status != status)
		set_registration_status(cdma_netreg, status);
}

static void strength_notify_common(struct ofono_cdma_netreg *netreg,
					int strength, const char *property,
					int *dest)
{
	if (netreg == NULL)
		return;

	if (*dest == strength)
		return;

	/*
	 * Theoretically we can get signal strength even when not registered
	 * to any network.  However, what do we do with it in that case?
	 */
	if (netreg->status == CDMA_NETWORK_REGISTRATION_STATUS_NOT_REGISTERED)
		return;

	*dest = strength;

	if (strength != -1) {
		DBusConnection *conn = ofono_dbus_get_connection();
		const char *path = __ofono_atom_get_path(netreg->atom);
		unsigned char val = strength;

		ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_NETWORK_REGISTRATION_INTERFACE,
				property, DBUS_TYPE_BYTE, &val);
	}
}

void ofono_cdma_netreg_strength_notify(struct ofono_cdma_netreg *netreg,
					int strength)
{
	return strength_notify_common(netreg, strength,
					"Strength", &netreg->strength);
}

void ofono_cdma_netreg_data_strength_notify(struct ofono_cdma_netreg *netreg,
						int data_strength)
{
	return strength_notify_common(netreg, data_strength,
					"DataStrength", &netreg->hdr_strength);
}

int ofono_cdma_netreg_get_status(struct ofono_cdma_netreg *netreg)
{
	if (netreg == NULL)
		return -1;

	return netreg->status;
}

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

	g_free(cdma_netreg->sid);
	g_free(cdma_netreg->provider_name);
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

	cdma_netreg->status = CDMA_NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
	cdma_netreg->strength = -1;
	cdma_netreg->hdr_strength = -1;

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
