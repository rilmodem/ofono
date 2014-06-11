/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Canonical Ltd. All rights reserved.
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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#include <glib.h>
#include <gdbus.h>
#include <errno.h>

#include "ofono.h"

#include "common.h"
#include "powerd.h"

static GSList *g_drivers = NULL;

struct ofono_powerd {
	const struct ofono_powerd_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	gboolean low_power;
	gboolean low_power_pending;
	DBusMessage *pending;
};

static void set_low_power(struct ofono_powerd *power, gboolean low_power)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(power->atom);
	dbus_bool_t value = low_power;

	if (power->low_power == low_power)
		return;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_RADIO_SETTINGS_INTERFACE,
						"LowPowerMode",
						DBUS_TYPE_BOOLEAN, &value);
	power->low_power = low_power;
}

static void set_display_state_cb(const struct ofono_error *error,
						void *data)
{
	struct ofono_powerd *power = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Error setting low power mode");

		power->low_power_pending = power->low_power;

		reply = __ofono_error_failed(power->pending);
		__ofono_dbus_pending_reply(&power->pending, reply);

		return;
	}

	reply = dbus_message_new_method_return(power->pending);
	__ofono_dbus_pending_reply(&power->pending, reply);

	set_low_power(power, power->low_power_pending);
}

static DBusMessage *power_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_powerd *power = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (power->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_strcmp0(property, "LowPowerMode") == 0) {
		dbus_bool_t value;

		if (power->driver->set_display_state == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (power->low_power_pending == (gboolean) value)
			return dbus_message_new_method_return(msg);

		power->pending = dbus_message_ref(msg);
		power->low_power_pending = value;

		power->driver->set_display_state(power, value ? FALSE : TRUE,
						set_display_state_cb, power);
		return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

static DBusMessage *power_get_properties_reply(DBusMessage *msg,
						struct ofono_powerd *power)
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

	ofono_dbus_dict_append(&dict, "LowPowerMode",
				DBUS_TYPE_BOOLEAN, &power->low_power);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *power_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_powerd *power = data;

	return power_get_properties_reply(msg, power);
}

static void powerd_unregister(struct ofono_atom *atom)
{
	struct ofono_powerd *power = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(power->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(power->atom);

	ofono_modem_remove_interface(modem, OFONO_POWER_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_POWER_INTERFACE);
}

static void powerd_remove(struct ofono_atom *atom)
{
	struct ofono_powerd *powerd = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (powerd == NULL)
		return;

	if (powerd->driver && powerd->driver->remove)
		powerd->driver->remove(powerd);

	g_free(powerd);
}

static const GDBusMethodTable power_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			power_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, power_set_property) },
	{ }
};

static const GDBusSignalTable power_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void register_interface(struct ofono_powerd *power)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(power->atom);
	const char *path = __ofono_atom_get_path(power->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_POWER_INTERFACE,
					power_methods, power_signals,
					NULL, power, NULL)) {
		ofono_error("%s: Could not create %s interface",
				__func__, OFONO_POWER_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_POWER_INTERFACE);
	__ofono_atom_register(power->atom, powerd_unregister);
}

static void register_cb(const struct ofono_error *error, void *data)
{
	struct ofono_powerd *power = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("%s: Error setting low power mode", __func__);
		return;
	}

	register_interface(power);
}

void ofono_powerd_register(struct ofono_powerd *power)
{
	if (power->driver->set_display_state == NULL) {
		register_interface(power);
		return;
	}

	/* Set to known state */
	power->driver->set_display_state(power, power->low_power ? FALSE : TRUE,
						register_cb, power);
}

int ofono_powerd_driver_register(const struct ofono_powerd_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_powerd_driver_unregister(const struct ofono_powerd_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

struct ofono_powerd *ofono_powerd_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data)
{
	struct ofono_powerd *powerd;
	GSList *l;

	if (driver == NULL)
		return NULL;

	powerd = g_try_new0(struct ofono_powerd, 1);
	if (powerd == NULL)
		return NULL;

	powerd->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_POWERD,
						powerd_remove, powerd);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_powerd_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(powerd, vendor, data) < 0)
			continue;

		powerd->driver = drv;
		break;
	}

	return powerd;
}

void ofono_powerd_remove(struct ofono_powerd *powerd)
{
	__ofono_atom_free(powerd->atom);
}

void ofono_powerd_set_data(struct ofono_powerd *powerd, void *data)
{
	powerd->driver_data = data;
}

void *ofono_powerd_get_data(struct ofono_powerd *powerd)
{
	return powerd->driver_data;
}
