/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

static GSList *g_devinfo_drivers = NULL;

static GSList *g_modem_list = NULL;
static int g_next_modem_id = 1;

struct ofono_modem {
	int		id;
	char		*path;
	GSList		*atoms;
	GSList		*atom_watches;
	int		next_atom_watch_id;
	GSList         	*interface_list;
	int		flags;
	unsigned int	call_ids;
	DBusMessage	*pending;
	guint		interface_update;
	void		*driver_data;
};

struct ofono_devinfo {
	char *manufacturer;
	char *model;
	char *revision;
	char *serial;
	const struct ofono_devinfo_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct ofono_atom {
	enum ofono_atom_type type;
	void (*destruct)(struct ofono_atom *atom);
	void (*unregister)(struct ofono_atom *atom);
	void *data;
	struct ofono_modem *modem;
};

struct ofono_atom_watch {
	enum ofono_atom_type type;
	int id;
	ofono_atom_watch_func notify;
	ofono_destroy_func destroy;
	void *notify_data;
};

unsigned int __ofono_modem_alloc_callid(struct ofono_modem *modem)
{
	unsigned int i;

	for (i = 1; i < sizeof(modem->call_ids) * 8; i++) {
		if (modem->call_ids & (0x1 << i))
			continue;

		modem->call_ids |= (0x1 << i);
		return i;
	}

	return 0;
}

void __ofono_modem_release_callid(struct ofono_modem *modem, int id)
{
	modem->call_ids &= ~(0x1 << id);
}

void ofono_modem_set_userdata(struct ofono_modem *modem, void *userdata)
{
	if (modem == NULL)
		return;

	modem->driver_data = userdata;
}

void *ofono_modem_get_userdata(struct ofono_modem *modem)
{
	if (modem == NULL)
		return NULL;

	return modem->driver_data;
}

const char *ofono_modem_get_path(struct ofono_modem *modem)
{
	if (modem)
		return modem->path;

	return NULL;
}

struct ofono_atom *__ofono_modem_add_atom(struct ofono_modem *modem,
					enum ofono_atom_type type,
					void (*destruct)(struct ofono_atom *),
					void *data)
{
	struct ofono_atom *atom;

	if (modem == NULL)
		return NULL;

	atom = g_new0(struct ofono_atom, 1);

	atom->type = type;
	atom->destruct = destruct;
	atom->data = data;
	atom->modem = modem;

	modem->atoms = g_slist_prepend(modem->atoms, atom);

	return atom;
}

void *__ofono_atom_get_data(struct ofono_atom *atom)
{
	return atom->data;
}

const char *__ofono_atom_get_path(struct ofono_atom *atom)
{
	return atom->modem->path;
}

struct ofono_modem *__ofono_atom_get_modem(struct ofono_atom *atom)
{
	return atom->modem;
}

static void call_watches(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond)
{
	struct ofono_modem *modem = atom->modem;
	GSList *l;
	struct ofono_atom_watch *watch;

	for (l = modem->atom_watches; l; l = l->next) {
		watch = l->data;

		if (watch->type != atom->type)
			continue;

		watch->notify(atom, cond, watch->notify_data);
	}
}

void __ofono_atom_register(struct ofono_atom *atom,
			void (*unregister)(struct ofono_atom *))
{
	if (unregister == NULL)
		return;

	atom->unregister = unregister;

	call_watches(atom, OFONO_ATOM_WATCH_CONDITION_REGISTERED);
}

void __ofono_atom_unregister(struct ofono_atom *atom)
{
	if (atom->unregister == NULL)
		return;

	call_watches(atom, OFONO_ATOM_WATCH_CONDITION_UNREGISTERED);

	atom->unregister(atom);
}

gboolean __ofono_atom_get_registered(struct ofono_atom *atom)
{
	return atom->unregister ? TRUE : FALSE;
}

int __ofono_modem_add_atom_watch(struct ofono_modem *modem,
					enum ofono_atom_type type,
					ofono_atom_watch_func notify,
					void *data, ofono_destroy_func destroy)
{
	struct ofono_atom_watch *watch;

	if (notify == NULL)
		return 0;

	watch = g_new0(struct ofono_atom_watch, 1);

	watch->type = type;
	watch->id = ++modem->next_atom_watch_id;
	watch->notify = notify;
	watch->destroy = destroy;
	watch->notify_data = data;

	modem->atom_watches = g_slist_prepend(modem->atom_watches, watch);

	return watch->id;
}

gboolean __ofono_modem_remove_atom_watch(struct ofono_modem *modem, int id)
{
	struct ofono_atom_watch *watch;
	GSList *p;
	GSList *c;

	p = NULL;
	c = modem->atom_watches;

	while (c) {
		watch = c->data;

		if (watch->id != id) {
			p = c;
			c = c->next;
			continue;
		}

		if (p)
			p->next = c->next;
		else
			modem->atom_watches = c->next;

		if (watch->destroy)
			watch->destroy(watch->notify_data);

		g_free(watch);
		g_slist_free_1(c);

		return TRUE;
	}

	return FALSE;
}

static void remove_all_watches(struct ofono_modem *modem)
{
	struct ofono_atom_watch *watch;
	GSList *l;

	for (l = modem->atom_watches; l; l = l->next) {
		watch = l->data;

		if (watch->destroy)
			watch->destroy(watch->notify_data);

		g_free(watch);
	}

	g_slist_free(modem->atom_watches);
	modem->atom_watches = NULL;
}

struct ofono_atom *__ofono_modem_find_atom(struct ofono_modem *modem,
						enum ofono_atom_type type)
{
	GSList *l;
	struct ofono_atom *atom;

	if (modem == NULL)
		return NULL;

	for (l = modem->atoms; l; l = l->next) {
		atom = l->data;

		if (atom->type == type)
			return atom;
	}

	return NULL;
}

void __ofono_modem_foreach_atom(struct ofono_modem *modem,
				enum ofono_atom_type type,
				ofono_atom_func callback, void *data)
{
	GSList *l;
	struct ofono_atom *atom;

	if (modem == NULL)
		return;

	for (l = modem->atoms; l; l = l->next) {
		atom = l->data;

		if (atom->type != type)
			continue;

		callback(atom, data);
	}
}

void __ofono_atom_free(struct ofono_atom *atom)
{
	struct ofono_modem *modem = atom->modem;

	modem->atoms = g_slist_remove(modem->atoms, atom);

	__ofono_atom_unregister(atom);

	if (atom->destruct)
		atom->destruct(atom);

	g_free(atom);
}

static void remove_all_atoms(struct ofono_modem *modem)
{
	GSList *l;
	struct ofono_atom *atom;

	if (modem == NULL)
		return;

	for (l = modem->atoms; l; l = l->next) {
		atom = l->data;

		__ofono_atom_unregister(atom);

		if (atom->destruct)
			atom->destruct(atom);

		g_free(atom);
	}

	g_slist_free(modem->atoms);
	modem->atoms = NULL;
}

static void modem_free(gpointer data)
{
	struct ofono_modem *modem = data;
	GSList *l;

	if (modem == NULL)
		return;

	g_slist_foreach(modem->interface_list, (GFunc)g_free, NULL);
	g_slist_free(modem->interface_list);

	if (modem->pending)
		dbus_message_unref(modem->pending);

	if (modem->interface_update)
		g_source_remove(modem->interface_update);

	g_free(modem->path);
	g_free(modem);
}

static DBusMessage *modem_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char **interfaces;
	int i;
	GSList *l;
	struct ofono_atom *devinfo_atom;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	devinfo_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_DEVINFO);

	/* We cheat a little here and don't check the registered status */
	if (devinfo_atom) {
		struct ofono_devinfo *info;

		info = __ofono_atom_get_data(devinfo_atom);

		if (info->manufacturer)
			ofono_dbus_dict_append(&dict, "Manufacturer",
						DBUS_TYPE_STRING,
						&info->manufacturer);

		if (info->model)
			ofono_dbus_dict_append(&dict, "Model", DBUS_TYPE_STRING,
						&info->model);

		if (info->revision)
			ofono_dbus_dict_append(&dict, "Revision",
						DBUS_TYPE_STRING,
						&info->revision);

		if (info->serial)
			ofono_dbus_dict_append(&dict, "Serial",
						DBUS_TYPE_STRING,
						&info->serial);
	}

	interfaces = g_new0(char *, g_slist_length(modem->interface_list) + 1);

	for (i = 0, l = modem->interface_list; l; l = l->next, i++)
		interfaces[i] = l->data;

	ofono_dbus_dict_append_array(&dict, "Interfaces", DBUS_TYPE_STRING,
					&interfaces);

	g_free(interfaces);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable modem_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	modem_get_properties },
	{ }
};

static GDBusSignalTable modem_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static gboolean trigger_interface_update(void *data)
{
	struct ofono_modem *modem = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	char **interfaces;
	GSList *l;
	int i;

	interfaces = g_new0(char *, g_slist_length(modem->interface_list) + 1);

	for (i = 0, l = modem->interface_list; l; l = l->next, i++)
		interfaces[i] = l->data;

	ofono_dbus_signal_array_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Interfaces", DBUS_TYPE_STRING,
						&interfaces);

	g_free(interfaces);

	modem->interface_update = 0;

	return FALSE;
}

void ofono_modem_add_interface(struct ofono_modem *modem,
				const char *interface)
{
	modem->interface_list =
		g_slist_prepend(modem->interface_list, g_strdup(interface));

	if (modem->interface_update != 0)
		return;

	modem->interface_update = g_idle_add(trigger_interface_update, modem);
}

void ofono_modem_remove_interface(struct ofono_modem *modem,
				const char *interface)
{
	GSList *found = g_slist_find_custom(modem->interface_list, interface,
						(GCompareFunc) strcmp);

	if (!found) {
		ofono_error("Interface %s not found on the interface_list",
				interface);
		return;
	}

	g_free(found->data);

	modem->interface_list = g_slist_remove(modem->interface_list,
						found->data);

	if (modem->interface_update != 0)
		return;

	modem->interface_update = g_idle_add(trigger_interface_update, modem);
}

static void query_serial_cb(const struct ofono_error *error,
				const char *serial, void *user)
{
	struct ofono_devinfo *info = user;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(info->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		return;

	info->serial = g_strdup(serial);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_MODEM_INTERFACE,
						"Serial", DBUS_TYPE_STRING,
						&info->serial);
}

static void query_serial(struct ofono_devinfo *info)
{
	if (!info->driver->query_serial)
		return;

	info->driver->query_serial(info, query_serial_cb, info);
}

static void query_revision_cb(const struct ofono_error *error,
				const char *revision, void *user)
{
	struct ofono_devinfo *info = user;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(info->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto out;

	info->revision = g_strdup(revision);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_MODEM_INTERFACE,
						"Revision", DBUS_TYPE_STRING,
						&info->revision);

out:
	query_serial(info);
}

static void query_revision(struct ofono_devinfo *info)
{
	if (!info->driver->query_revision) {
		query_serial(info);
		return;
	}

	info->driver->query_revision(info, query_revision_cb, info);
}

static void query_model_cb(const struct ofono_error *error,
				const char *model, void *user)
{
	struct ofono_devinfo *info = user;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(info->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto out;

	info->model = g_strdup(model);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_MODEM_INTERFACE,
						"Model", DBUS_TYPE_STRING,
						&info->model);

out:
	query_revision(info);
}

static void query_model(struct ofono_devinfo *info)
{
	if (!info->driver->query_model) {
		/* If model is not supported, don't bother querying revision */
		query_serial(info);
	}

	info->driver->query_model(info, query_model_cb, info);
}

static void query_manufacturer_cb(const struct ofono_error *error,
					const char *manufacturer, void *user)
{
	struct ofono_devinfo *info = user;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(info->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto out;

	info->manufacturer = g_strdup(manufacturer);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_MODEM_INTERFACE,
						"Serial", DBUS_TYPE_STRING,
						&info->manufacturer);

out:
	query_model(info);
}

static gboolean query_manufacturer(gpointer user)
{
	struct ofono_devinfo *info = user;

	if (!info->driver->query_manufacturer) {
		query_model(info);
		return FALSE;
	}

	info->driver->query_manufacturer(info, query_manufacturer_cb, info);

	return FALSE;
}

int ofono_devinfo_driver_register(const struct ofono_devinfo_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_devinfo_drivers = g_slist_prepend(g_devinfo_drivers, (void *)d);

	return 0;
}

void ofono_devinfo_driver_unregister(const struct ofono_devinfo_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_devinfo_drivers = g_slist_remove(g_devinfo_drivers, (void *)d);
}

static void devinfo_remove(struct ofono_atom *atom)
{
	struct ofono_devinfo *info = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (info == NULL)
		return;

	if (info->driver == NULL)
		return;

	if (info->driver->remove)
		info->driver->remove(info);

	g_free(info->manufacturer);
	g_free(info->model);
	g_free(info->revision);
	g_free(info->serial);

	g_free(info);
}

struct ofono_devinfo *ofono_devinfo_create(struct ofono_modem *modem,
							const char *driver,
							void *data)
{
	struct ofono_devinfo *info;
	GSList *l;

	info = g_new0(struct ofono_devinfo, 1);

	info->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_DEVINFO,
						devinfo_remove, info);
	info->driver_data = data;

	for (l = g_devinfo_drivers; l; l = l->next) {
		const struct ofono_devinfo_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(info) < 0)
			continue;

		info->driver = drv;
		break;
	}

	return info;
}

void ofono_devinfo_register(struct ofono_devinfo *info)
{
	query_manufacturer(info);
}

void ofono_devinfo_remove(struct ofono_devinfo *info)
{
	__ofono_atom_free(info->atom);
}

void ofono_devinfo_set_data(struct ofono_devinfo *info, void *data)
{
	info->driver_data = data;
}

void *ofono_devinfo_get_data(struct ofono_devinfo *info)
{
	return info->driver_data;
}

static struct ofono_modem *modem_create(int id)
{
	char path[128];
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem;

	modem = g_try_new0(struct ofono_modem, 1);
	if (modem == NULL)
		return modem;

	modem->id = id;

	snprintf(path, sizeof(path), "/modem%d", modem->id);
	modem->path = g_strdup(path);

	if (!g_dbus_register_interface(conn, path, OFONO_MODEM_INTERFACE,
			modem_methods, modem_signals, NULL,
			modem, modem_free)) {
		ofono_error("Modem interface init failed on path %s", path);
		modem_free(modem);
		return NULL;
	}

	return modem;
}

static void modem_remove(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	/* Need to make a copy to keep gdbus happy */
	char *path = g_strdup(modem->path);

	ofono_debug("Removing modem: %s", modem->path);

	remove_all_atoms(modem);
	remove_all_watches(modem);

	g_dbus_unregister_interface(conn, path, OFONO_MODEM_INTERFACE);

	g_free(path);
}

/* Clients only need to free *modems */
const char **__ofono_modem_get_list()
{
	GSList *l;
	int i;
	struct ofono_modem *modem;
	const char **modems;

	modems = g_new0(const char *, g_slist_length(g_modem_list) + 1);

	for (l = g_modem_list, i = 0; l; l = l->next, i++) {
		modem = l->data;

		modems[i] = modem->path;
	}

	return modems;
}

struct ofono_modem *ofono_modem_register()
{
	struct ofono_modem *modem;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char **modems;

	modem = modem_create(g_next_modem_id);

	if (modem == NULL)
		return 0;

	++g_next_modem_id;

	__ofono_history_probe_drivers(modem);
	g_modem_list = g_slist_prepend(g_modem_list, modem);

	modems = __ofono_modem_get_list();

	if (modems) {
		ofono_dbus_signal_array_property_changed(conn,
				OFONO_MANAGER_PATH,
				OFONO_MANAGER_INTERFACE, "Modems",
				DBUS_TYPE_OBJECT_PATH, &modems);

		g_free(modems);
	}

	return modem;
}

int ofono_modem_unregister(struct ofono_modem *m)
{
	struct ofono_modem *modem = m;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char **modems;

	if (modem == NULL)
		return -1;

	modem_remove(modem);

	g_modem_list = g_slist_remove(g_modem_list, modem);

	modems = __ofono_modem_get_list();

	if (modems) {
		ofono_dbus_signal_array_property_changed(conn,
				OFONO_MANAGER_PATH,
				OFONO_MANAGER_INTERFACE, "Modems",
				DBUS_TYPE_OBJECT_PATH, &modems);

		g_free(modems);
	}

	return 0;
}
