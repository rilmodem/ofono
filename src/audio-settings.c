/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License veasion 2 as
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

static GSList *g_drivers = NULL;

struct ofono_audio_settings {
	ofono_bool_t active;
	char *mode;
	const struct ofono_audio_settings_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

void ofono_audio_settings_active_notify(struct ofono_audio_settings *as,
					ofono_bool_t active)
{
	const char *path = __ofono_atom_get_path(as->atom);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (as->active == active)
		return;

	DBG("active %d", active);

	as->active = active;

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_AUDIO_SETTINGS_INTERFACE,
				"Active", DBUS_TYPE_BOOLEAN, &as->active);

}

void ofono_audio_settings_mode_notify(struct ofono_audio_settings *as,
						const char *mode)
{
	const char *path = __ofono_atom_get_path(as->atom);
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("mode %s", mode);

	g_free(as->mode);
	as->mode = g_strdup(mode);

	if (as->mode == NULL)
		return;

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_AUDIO_SETTINGS_INTERFACE,
				"Mode", DBUS_TYPE_STRING, &as->mode);
}

static DBusMessage *audio_get_properties_reply(DBusMessage *msg,
					struct ofono_audio_settings *as)
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

	ofono_dbus_dict_append(&dict, "Active", DBUS_TYPE_BOOLEAN, &as->active);

	if (as->mode)
		ofono_dbus_dict_append(&dict, "Mode",
					DBUS_TYPE_STRING, &as->mode);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *audio_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_audio_settings *as = data;

	return audio_get_properties_reply(msg, as);
}

static const GDBusMethodTable audio_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetProperties",
				NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
				audio_get_properties) },
	{ }
};

static const GDBusSignalTable audio_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

int ofono_audio_settings_driver_register(const struct ofono_audio_settings_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_audio_settings_driver_unregister(const struct ofono_audio_settings_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void audio_settings_unregister(struct ofono_atom *atom)
{
	struct ofono_audio_settings *as = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(as->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(as->atom);

	ofono_modem_remove_interface(modem, OFONO_AUDIO_SETTINGS_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_AUDIO_SETTINGS_INTERFACE);
}

static void audio_settings_remove(struct ofono_atom *atom)
{
	struct ofono_audio_settings *as = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (as == NULL)
		return;

	if (as->driver && as->driver->remove)
		as->driver->remove(as);

	g_free(as->mode);
	g_free(as);
}

struct ofono_audio_settings *ofono_audio_settings_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data)
{
	struct ofono_audio_settings *as;
	GSList *l;

	if (driver == NULL)
		return NULL;

	as = g_try_new0(struct ofono_audio_settings, 1);
	if (as == NULL)
		return NULL;

	as->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_AUDIO_SETTINGS,
						audio_settings_remove, as);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_audio_settings_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver) != 0)
			continue;

		if (drv->probe(as, vendor, data) < 0)
			continue;

		as->driver = drv;
		break;
	}

	return as;
}

void ofono_audio_settings_register(struct ofono_audio_settings *as)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(as->atom);
	const char *path = __ofono_atom_get_path(as->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_AUDIO_SETTINGS_INTERFACE,
					audio_methods, audio_signals,
					NULL, as, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_AUDIO_SETTINGS_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_AUDIO_SETTINGS_INTERFACE);
	__ofono_atom_register(as->atom, audio_settings_unregister);
}

void ofono_audio_settings_remove(struct ofono_audio_settings *as)
{
	__ofono_atom_free(as->atom);
}

void ofono_audio_settings_set_data(struct ofono_audio_settings *as, void *data)
{
	as->driver_data = data;
}

void *ofono_audio_settings_get_data(struct ofono_audio_settings *as)
{
	return as->driver_data;
}

struct ofono_modem *ofono_audio_settings_get_modem(struct ofono_audio_settings *as)
{
	return __ofono_atom_get_modem(as->atom);
}
