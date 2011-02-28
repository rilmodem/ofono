/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) 2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  ProFUSION embedded systems.
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
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"
#include "common.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

static GSList *g_drivers = NULL;

struct ofono_location_reporting {
	DBusMessage *pending;
	const struct ofono_location_reporting_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	ofono_bool_t enabled;
	char *client_owner;
	guint disconnect_watch;
};

static const char *location_reporting_type_to_string(
					enum ofono_location_reporting_type type)
{
	switch (type) {
	case OFONO_LOCATION_REPORTING_TYPE_NMEA:
		return "nmea";
	};

	return NULL;
}

static DBusMessage *location_reporting_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)

{
	struct ofono_location_reporting *lr = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *type;
	int value;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	value = lr->enabled;
	ofono_dbus_dict_append(&dict, "Enabled", DBUS_TYPE_BOOLEAN, &value);

	type = location_reporting_type_to_string(lr->driver->type);
	ofono_dbus_dict_append(&dict, "Type", DBUS_TYPE_STRING, &type);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void client_remove(struct ofono_location_reporting *lr)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (lr->disconnect_watch) {
		g_dbus_remove_watch(conn, lr->disconnect_watch);
		lr->disconnect_watch = 0;
	}

	g_free(lr->client_owner);
}

static void signal_enabled(const struct ofono_location_reporting *lr)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(lr->atom);
	int value = lr->enabled;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_LOCATION_REPORTING_INTERFACE,
					"Enabled", DBUS_TYPE_BOOLEAN, &value);
}

static void client_exited_disable_cb(const struct ofono_error *error,
								void *data)
{
	struct ofono_location_reporting *lr = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Disabling location-reporting failed");

		return;
	}

	client_remove(lr);
	lr->enabled = FALSE;

	signal_enabled(lr);
}

static void client_exited(DBusConnection *conn, void *data)
{
	struct ofono_location_reporting *lr = data;

	lr->disconnect_watch = 0;

	lr->driver->disable(lr, client_exited_disable_cb , lr);
}

static void location_reporting_disable_cb(const struct ofono_error *error,
								void *data)
{
	struct ofono_location_reporting *lr = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Disabling location-reporting failed");

		reply = __ofono_error_failed(lr->pending);
		__ofono_dbus_pending_reply(&lr->pending, reply);

		return;
	}

	client_remove(lr);
	lr->enabled = FALSE;

	reply = dbus_message_new_method_return(lr->pending);
	__ofono_dbus_pending_reply(&lr->pending, reply);

	signal_enabled(lr);
}

static void location_reporting_enable_cb(const struct ofono_error *error,
							int fd,	void *data)
{
	struct ofono_location_reporting *lr = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Enabling location-reporting failed");

		reply = __ofono_error_failed(lr->pending);
		__ofono_dbus_pending_reply(&lr->pending, reply);

		return;
	}

	lr->enabled = TRUE;
	lr->client_owner = g_strdup(dbus_message_get_sender(lr->pending));
	lr->disconnect_watch = g_dbus_add_disconnect_watch(conn,
				lr->client_owner, client_exited, lr, NULL);

	reply = dbus_message_new_method_return(lr->pending);
	dbus_message_append_args(reply, DBUS_TYPE_UNIX_FD, &fd,
							DBUS_TYPE_INVALID);

	__ofono_dbus_pending_reply(&lr->pending, reply);

	signal_enabled(lr);
}

static DBusMessage *location_reporting_request(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_location_reporting *lr = data;

	if (lr->pending != NULL)
		return __ofono_error_busy(msg);

	if (lr->enabled)
		return __ofono_error_in_use(msg);

	lr->pending = dbus_message_ref(msg);

	lr->driver->enable(lr, location_reporting_enable_cb, lr);

	return NULL;
}

static DBusMessage *location_reporting_release(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_location_reporting *lr = data;
	const char *caller = dbus_message_get_sender(msg);


	/*
	 * Avoid a race by not trying to release the device if there is a
	 * pending message or client already signaled it's exiting. In the
	 * later case, the device will eventually be released in
	 * client_exited_disable_cb().
	 */
	if (lr->pending != NULL || (lr->enabled && !lr->disconnect_watch))
		return __ofono_error_busy(msg);

	if (lr->enabled == FALSE)
		return __ofono_error_not_available(msg);

	if (g_strcmp0(caller, lr->client_owner))
		return __ofono_error_access_denied(msg);

	lr->pending = dbus_message_ref(msg);

	lr->driver->disable(lr, location_reporting_disable_cb, lr);

	return NULL;
}

static GDBusMethodTable location_reporting_methods[] = {
	{ "GetProperties",  "",    "a{sv}", location_reporting_get_properties },
	{ "Request",        "",    "h",     location_reporting_request,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "Release",        "",    "",      location_reporting_release,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable location_reporting_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

int ofono_location_reporting_driver_register(
				const struct ofono_location_reporting_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d == NULL || d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_location_reporting_driver_unregister(
				const struct ofono_location_reporting_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d == NULL)
		return;

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

struct ofono_modem *ofono_location_reporting_get_modem(
					struct ofono_location_reporting *lr)
{
	return __ofono_atom_get_modem(lr->atom);
}

static void location_reporting_unregister(struct ofono_atom *atom)
{
	struct ofono_location_reporting *lr = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(lr->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(lr->atom);

	ofono_modem_remove_interface(modem, OFONO_LOCATION_REPORTING_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					OFONO_LOCATION_REPORTING_INTERFACE);
}

static void location_reporting_remove(struct ofono_atom *atom)
{
	struct ofono_location_reporting *lr = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (lr == NULL)
		return;

	if (lr->driver && lr->driver->remove)
		lr->driver->remove(lr);

	g_free(lr);
}

struct ofono_location_reporting *ofono_location_reporting_create(
						struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data)
{
	struct ofono_location_reporting *lr;
	GSList *l;

	if (driver == NULL)
		return NULL;

	/* Only D-Bus >= 1.3 supports fd-passing */
	if (DBUS_TYPE_UNIX_FD == -1)
		return NULL;

	lr = g_try_new0(struct ofono_location_reporting, 1);
	if (lr == NULL)
		return NULL;

	lr->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPE_LOCATION_REPORTING,
					location_reporting_remove, lr);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_location_reporting_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver) != 0)
			continue;

		if (drv->probe(lr, vendor, data) < 0)
			continue;

		lr->driver = drv;
		break;
	}

	return lr;
}

void ofono_location_reporting_register(struct ofono_location_reporting *lr)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(lr->atom);
	const char *path = __ofono_atom_get_path(lr->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_LOCATION_REPORTING_INTERFACE,
					location_reporting_methods,
					location_reporting_signals,
					NULL, lr, NULL)) {
		ofono_error("Could not create %s interface",
					OFONO_LOCATION_REPORTING_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_LOCATION_REPORTING_INTERFACE);
	__ofono_atom_register(lr->atom, location_reporting_unregister);
}

void ofono_location_reporting_remove(struct ofono_location_reporting *lr)
{
	__ofono_atom_free(lr->atom);
}

void ofono_location_reporting_set_data(struct ofono_location_reporting *lr,
								void *data)
{
	lr->driver_data = data;
}

void *ofono_location_reporting_get_data(struct ofono_location_reporting *lr)
{
	return lr->driver_data;
}
