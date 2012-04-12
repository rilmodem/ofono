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
#include <stdio.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"

static GSList *g_devinfo_drivers = NULL;
static GSList *g_driver_list = NULL;
static GSList *g_modem_list = NULL;

static int next_modem_id = 0;
static gboolean powering_down = FALSE;
static int modems_remaining = 0;

static struct ofono_watchlist *g_modemwatches = NULL;

enum property_type {
	PROPERTY_TYPE_INVALID = 0,
	PROPERTY_TYPE_STRING,
	PROPERTY_TYPE_INTEGER,
	PROPERTY_TYPE_BOOLEAN,
};

enum modem_state {
	MODEM_STATE_POWER_OFF,
	MODEM_STATE_PRE_SIM,
	MODEM_STATE_OFFLINE,
	MODEM_STATE_ONLINE,
};

struct ofono_modem {
	char			*path;
	enum modem_state	modem_state;
	GSList			*atoms;
	struct ofono_watchlist	*atom_watches;
	GSList			*interface_list;
	GSList			*feature_list;
	unsigned int		call_ids;
	DBusMessage		*pending;
	guint			interface_update;
	ofono_bool_t		powered;
	ofono_bool_t		powered_pending;
	ofono_bool_t		get_online;
	ofono_bool_t		lockdown;
	char			*lock_owner;
	guint			lock_watch;
	guint			timeout;
	ofono_bool_t		online;
	struct ofono_watchlist	*online_watches;
	struct ofono_watchlist	*powered_watches;
	guint			emergency;
	GHashTable		*properties;
	struct ofono_sim	*sim;
	unsigned int		sim_watch;
	unsigned int		sim_ready_watch;
	const struct ofono_modem_driver *driver;
	void			*driver_data;
	char			*driver_type;
	char			*name;
};

struct ofono_devinfo {
	char *manufacturer;
	char *model;
	char *revision;
	char *serial;
	unsigned int dun_watch;
	const struct ofono_devinfo_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct ofono_atom {
	enum ofono_atom_type type;
	enum modem_state modem_state;
	void (*destruct)(struct ofono_atom *atom);
	void (*unregister)(struct ofono_atom *atom);
	void *data;
	struct ofono_modem *modem;
};

struct atom_watch {
	struct ofono_watchlist_item item;
	enum ofono_atom_type type;
};

struct modem_property {
	enum property_type type;
	void *value;
};

static const char *modem_type_to_string(enum ofono_modem_type type)
{
	switch (type) {
	case OFONO_MODEM_TYPE_HARDWARE:
		return "hardware";
	case OFONO_MODEM_TYPE_HFP:
		return "hfp";
	case OFONO_MODEM_TYPE_SAP:
		return "sap";
	}

	return "unknown";
}

unsigned int __ofono_modem_callid_next(struct ofono_modem *modem)
{
	unsigned int i;

	for (i = 1; i < sizeof(modem->call_ids) * 8; i++) {
		if (modem->call_ids & (1 << i))
			continue;

		return i;
	}

	return 0;
}

void __ofono_modem_callid_hold(struct ofono_modem *modem, int id)
{
	modem->call_ids |= (1 << id);
}

void __ofono_modem_callid_release(struct ofono_modem *modem, int id)
{
	modem->call_ids &= ~(1 << id);
}

void ofono_modem_set_data(struct ofono_modem *modem, void *data)
{
	if (modem == NULL)
		return;

	modem->driver_data = data;
}

void *ofono_modem_get_data(struct ofono_modem *modem)
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
	atom->modem_state = modem->modem_state;
	atom->destruct = destruct;
	atom->data = data;
	atom->modem = modem;

	modem->atoms = g_slist_prepend(modem->atoms, atom);

	return atom;
}

struct ofono_atom *__ofono_modem_add_atom_offline(struct ofono_modem *modem,
					enum ofono_atom_type type,
					void (*destruct)(struct ofono_atom *),
					void *data)
{
	struct ofono_atom *atom;

	atom = __ofono_modem_add_atom(modem, type, destruct, data);

	atom->modem_state = MODEM_STATE_OFFLINE;

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
	GSList *atom_watches = modem->atom_watches->items;
	GSList *l;
	struct atom_watch *watch;
	ofono_atom_watch_func notify;

	for (l = atom_watches; l; l = l->next) {
		watch = l->data;

		if (watch->type != atom->type)
			continue;

		notify = watch->item.notify;
		notify(atom, cond, watch->item.notify_data);
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
	atom->unregister = NULL;
}

gboolean __ofono_atom_get_registered(struct ofono_atom *atom)
{
	return atom->unregister ? TRUE : FALSE;
}

unsigned int __ofono_modem_add_atom_watch(struct ofono_modem *modem,
					enum ofono_atom_type type,
					ofono_atom_watch_func notify,
					void *data, ofono_destroy_func destroy)
{
	struct atom_watch *watch;
	unsigned int id;
	GSList *l;
	struct ofono_atom *atom;

	if (notify == NULL)
		return 0;

	watch = g_new0(struct atom_watch, 1);

	watch->type = type;
	watch->item.notify = notify;
	watch->item.destroy = destroy;
	watch->item.notify_data = data;

	id = __ofono_watchlist_add_item(modem->atom_watches,
					(struct ofono_watchlist_item *)watch);

	for (l = modem->atoms; l; l = l->next) {
		atom = l->data;

		if (atom->type != type || atom->unregister == NULL)
			continue;

		notify(atom, OFONO_ATOM_WATCH_CONDITION_REGISTERED, data);
	}

	return id;
}

gboolean __ofono_modem_remove_atom_watch(struct ofono_modem *modem,
						unsigned int id)
{
	return __ofono_watchlist_remove_item(modem->atom_watches, id);
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

		if (atom->type == type && atom->unregister != NULL)
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

void __ofono_modem_foreach_registered_atom(struct ofono_modem *modem,
						enum ofono_atom_type type,
						ofono_atom_func callback,
						void *data)
{
	GSList *l;
	struct ofono_atom *atom;

	if (modem == NULL)
		return;

	for (l = modem->atoms; l; l = l->next) {
		atom = l->data;

		if (atom->type != type)
			continue;

		if (atom->unregister == NULL)
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

static void flush_atoms(struct ofono_modem *modem, enum modem_state new_state)
{
	GSList *cur;
	GSList *prev;
	GSList *tmp;

	DBG("");

	prev = NULL;
	cur = modem->atoms;

	while (cur) {
		struct ofono_atom *atom = cur->data;

		if (atom->modem_state <= new_state) {
			prev = cur;
			cur = cur->next;
			continue;
		}

		__ofono_atom_unregister(atom);

		if (atom->destruct)
			atom->destruct(atom);

		g_free(atom);

		if (prev)
			prev->next = cur->next;
		else
			modem->atoms = cur->next;

		tmp = cur;
		cur = cur->next;
		g_slist_free_1(tmp);
	}
}

static void notify_online_watches(struct ofono_modem *modem)
{
	struct ofono_watchlist_item *item;
	GSList *l;
	ofono_modem_online_notify_func notify;

	if (modem->online_watches == NULL)
		return;

	for (l = modem->online_watches->items; l; l = l->next) {
		item = l->data;
		notify = item->notify;
		notify(modem, modem->online, item->notify_data);
	}
}

static void notify_powered_watches(struct ofono_modem *modem)
{
	struct ofono_watchlist_item *item;
	GSList *l;
	ofono_modem_powered_notify_func notify;

	if (modem->powered_watches == NULL)
		return;

	for (l = modem->powered_watches->items; l; l = l->next) {
		item = l->data;
		notify = item->notify;
		notify(modem, modem->powered, item->notify_data);
	}
}

static void set_online(struct ofono_modem *modem, ofono_bool_t new_online)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (new_online == modem->online)
		return;

	modem->online = new_online;

	ofono_dbus_signal_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Online", DBUS_TYPE_BOOLEAN,
						&modem->online);

	notify_online_watches(modem);
}

static void modem_change_state(struct ofono_modem *modem,
				enum modem_state new_state)
{
	struct ofono_modem_driver const *driver = modem->driver;
	enum modem_state old_state = modem->modem_state;

	DBG("old state: %d, new state: %d", old_state, new_state);

	if (old_state == new_state)
		return;

	modem->modem_state = new_state;

	if (old_state > new_state)
		flush_atoms(modem, new_state);

	switch (new_state) {
	case MODEM_STATE_POWER_OFF:
		modem->call_ids = 0;
		break;

	case MODEM_STATE_PRE_SIM:
		if (old_state < MODEM_STATE_PRE_SIM && driver->pre_sim)
			driver->pre_sim(modem);
		break;

	case MODEM_STATE_OFFLINE:
		if (old_state < MODEM_STATE_OFFLINE) {
			if (driver->post_sim)
				driver->post_sim(modem);

			__ofono_history_probe_drivers(modem);
			__ofono_nettime_probe_drivers(modem);
		}

		break;

	case MODEM_STATE_ONLINE:
		if (driver->post_online)
			driver->post_online(modem);

		break;
	}
}

unsigned int __ofono_modem_add_online_watch(struct ofono_modem *modem,
					ofono_modem_online_notify_func notify,
					void *data, ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *item;

	if (modem == NULL || notify == NULL)
		return 0;

	item = g_new0(struct ofono_watchlist_item, 1);

	item->notify = notify;
	item->destroy = destroy;
	item->notify_data = data;

	return __ofono_watchlist_add_item(modem->online_watches, item);
}

void __ofono_modem_remove_online_watch(struct ofono_modem *modem,
					unsigned int id)
{
	__ofono_watchlist_remove_item(modem->online_watches, id);
}

unsigned int __ofono_modem_add_powered_watch(struct ofono_modem *modem,
					ofono_modem_powered_notify_func notify,
					void *data, ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *item;

	if (modem == NULL || notify == NULL)
		return 0;

	item = g_new0(struct ofono_watchlist_item, 1);

	item->notify = notify;
	item->destroy = destroy;
	item->notify_data = data;

	return __ofono_watchlist_add_item(modem->powered_watches, item);
}

void __ofono_modem_remove_powered_watch(struct ofono_modem *modem,
					unsigned int id)
{
	__ofono_watchlist_remove_item(modem->powered_watches, id);
}

static gboolean modem_has_sim(struct ofono_modem *modem)
{
	GSList *l;
	struct ofono_atom *atom;

	for (l = modem->atoms; l; l = l->next) {
		atom = l->data;

		if (atom->type == OFONO_ATOM_TYPE_SIM)
			return TRUE;
	}

	return FALSE;
}

static void common_online_cb(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		return;

	/*
	 * If we need to get online after a silent reset this callback
	 * is called.  The callback should not consider the pending dbus
	 * message.
	 *
	 * Additionally, this process can be interrupted by the following
	 * events:
	 *	- Sim being removed or reset
	 *	- SetProperty(Powered, False) being called
	 *	- SetProperty(Lockdown, True) being called
	 *
	 * We should not set the modem to the online state in these cases.
	 */
	switch (modem->modem_state) {
	case MODEM_STATE_OFFLINE:
		set_online(modem, TRUE);

		/* Will this increase emergency call setup time??? */
		modem_change_state(modem, MODEM_STATE_ONLINE);
		break;
	case MODEM_STATE_POWER_OFF:
		/* The powered operation is pending */
		break;
	case MODEM_STATE_PRE_SIM:
		/*
		 * Its valid to be in online even without a SIM/SIM being
		 * PIN locked. e.g.: Emergency mode
		 */
		DBG("Online in PRE SIM state");

		set_online(modem, TRUE);
		break;
	case MODEM_STATE_ONLINE:
		ofono_error("Online called when the modem is already online!");
		break;
	};
}

static void online_cb(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	DBusMessage *reply;

	if (!modem->pending)
		goto out;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(modem->pending);
	else
		reply = __ofono_error_failed(modem->pending);

	__ofono_dbus_pending_reply(&modem->pending, reply);

out:
	common_online_cb(error, data);
}

static void offline_cb(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(modem->pending);
	else
		reply = __ofono_error_failed(modem->pending);

	__ofono_dbus_pending_reply(&modem->pending, reply);

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		switch (modem->modem_state) {
		case MODEM_STATE_PRE_SIM:
			set_online(modem, FALSE);
			break;
		case MODEM_STATE_ONLINE:
			set_online(modem, FALSE);
			modem_change_state(modem, MODEM_STATE_OFFLINE);
			break;
		default:
			break;
		}
	}
}

static void sim_state_watch(enum ofono_sim_state new_state, void *user)
{
	struct ofono_modem *modem = user;

	switch (new_state) {
	case OFONO_SIM_STATE_NOT_PRESENT:
		modem_change_state(modem, MODEM_STATE_PRE_SIM);
	case OFONO_SIM_STATE_INSERTED:
		break;
	case OFONO_SIM_STATE_LOCKED_OUT:
		modem_change_state(modem, MODEM_STATE_PRE_SIM);
		break;
	case OFONO_SIM_STATE_READY:
		modem_change_state(modem, MODEM_STATE_OFFLINE);

		/*
		 * If we don't have the set_online method, also proceed
		 * straight to the online state
		 */
		if (modem->driver->set_online == NULL)
			set_online(modem, TRUE);

		if (modem->online == TRUE)
			modem_change_state(modem, MODEM_STATE_ONLINE);
		else if (modem->get_online)
			modem->driver->set_online(modem, 1, common_online_cb,
							modem);

		modem->get_online = FALSE;

		break;
	}
}

static DBusMessage *set_property_online(struct ofono_modem *modem,
					DBusMessage *msg,
					DBusMessageIter *var)
{
	ofono_bool_t online;
	const struct ofono_modem_driver *driver = modem->driver;

	if (modem->powered == FALSE)
		return __ofono_error_not_available(msg);

	if (dbus_message_iter_get_arg_type(var) != DBUS_TYPE_BOOLEAN)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(var, &online);

	if (modem->pending != NULL)
		return __ofono_error_busy(msg);

	if (modem->online == online)
		return dbus_message_new_method_return(msg);

	if (ofono_modem_get_emergency_mode(modem) == TRUE)
		return __ofono_error_emergency_active(msg);

	if (driver->set_online == NULL)
		return __ofono_error_not_implemented(msg);

	modem->pending = dbus_message_ref(msg);

	driver->set_online(modem, online,
				online ? online_cb : offline_cb, modem);

	return NULL;
}

ofono_bool_t ofono_modem_get_online(struct ofono_modem *modem)
{
	if (modem == NULL)
		return FALSE;

	return modem->online;
}

void __ofono_modem_append_properties(struct ofono_modem *modem,
						DBusMessageIter *dict)
{
	char **interfaces;
	char **features;
	int i;
	GSList *l;
	struct ofono_devinfo *info;
	dbus_bool_t emergency = ofono_modem_get_emergency_mode(modem);
	const char *strtype;

	ofono_dbus_dict_append(dict, "Online", DBUS_TYPE_BOOLEAN,
				&modem->online);

	ofono_dbus_dict_append(dict, "Powered", DBUS_TYPE_BOOLEAN,
				&modem->powered);

	ofono_dbus_dict_append(dict, "Lockdown", DBUS_TYPE_BOOLEAN,
				&modem->lockdown);

	ofono_dbus_dict_append(dict, "Emergency", DBUS_TYPE_BOOLEAN,
				&emergency);

	info = __ofono_atom_find(OFONO_ATOM_TYPE_DEVINFO, modem);
	if (info) {
		if (info->manufacturer)
			ofono_dbus_dict_append(dict, "Manufacturer",
						DBUS_TYPE_STRING,
						&info->manufacturer);

		if (info->model)
			ofono_dbus_dict_append(dict, "Model", DBUS_TYPE_STRING,
						&info->model);

		if (info->revision)
			ofono_dbus_dict_append(dict, "Revision",
						DBUS_TYPE_STRING,
						&info->revision);

		if (info->serial)
			ofono_dbus_dict_append(dict, "Serial",
						DBUS_TYPE_STRING,
						&info->serial);
	}

	interfaces = g_new0(char *, g_slist_length(modem->interface_list) + 1);
	for (i = 0, l = modem->interface_list; l; l = l->next, i++)
		interfaces[i] = l->data;
	ofono_dbus_dict_append_array(dict, "Interfaces", DBUS_TYPE_STRING,
					&interfaces);
	g_free(interfaces);

	features = g_new0(char *, g_slist_length(modem->feature_list) + 1);
	for (i = 0, l = modem->feature_list; l; l = l->next, i++)
		features[i] = l->data;
	ofono_dbus_dict_append_array(dict, "Features", DBUS_TYPE_STRING,
					&features);
	g_free(features);

	if (modem->name)
		ofono_dbus_dict_append(dict, "Name", DBUS_TYPE_STRING,
					&modem->name);

	strtype = modem_type_to_string(modem->driver->modem_type);
	ofono_dbus_dict_append(dict, "Type", DBUS_TYPE_STRING, &strtype);
}

static DBusMessage *modem_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
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
	__ofono_modem_append_properties(modem, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static int set_powered(struct ofono_modem *modem, ofono_bool_t powered)
{
	const struct ofono_modem_driver *driver = modem->driver;
	int err = -EINVAL;

	if (modem->powered_pending == powered)
		return -EALREADY;

	/* Remove the atoms even if the driver is no longer available */
	if (powered == FALSE)
		modem_change_state(modem, MODEM_STATE_POWER_OFF);

	modem->powered_pending = powered;

	if (driver == NULL)
		return -EINVAL;

	if (powered == TRUE) {
		if (driver->enable)
			err = driver->enable(modem);
	} else {
		if (driver->disable)
			err = driver->disable(modem);
	}

	if (err == 0) {
		modem->powered = powered;
		notify_powered_watches(modem);
	} else if (err != -EINPROGRESS)
		modem->powered_pending = modem->powered;

	return err;
}

static void lockdown_remove(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (modem->lock_watch) {
		g_dbus_remove_watch(conn, modem->lock_watch);
		modem->lock_watch = 0;
	}

	g_free(modem->lock_owner);
	modem->lock_owner = NULL;

	modem->lockdown = FALSE;
}

static gboolean set_powered_timeout(gpointer user)
{
	struct ofono_modem *modem = user;

	DBG("modem: %p", modem);

	modem->timeout = 0;

	if (modem->powered_pending == FALSE) {
		DBusConnection *conn = ofono_dbus_get_connection();
		dbus_bool_t powered = FALSE;

		set_online(modem, FALSE);

		modem->powered = FALSE;
		notify_powered_watches(modem);

		ofono_dbus_signal_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Powered", DBUS_TYPE_BOOLEAN,
						&powered);
	} else {
		modem->powered_pending = modem->powered;
	}

	if (modem->pending != NULL) {
		DBusMessage *reply;

		reply = __ofono_error_timed_out(modem->pending);
		__ofono_dbus_pending_reply(&modem->pending, reply);

		if (modem->lockdown)
			lockdown_remove(modem);
	}

	return FALSE;
}

static void lockdown_disconnect(DBusConnection *conn, void *user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	ofono_dbus_signal_property_changed(conn, modem->path,
					OFONO_MODEM_INTERFACE,
					"Lockdown", DBUS_TYPE_BOOLEAN,
					&modem->lockdown);

	modem->lock_watch = 0;
	lockdown_remove(modem);
}

static DBusMessage *set_property_lockdown(struct ofono_modem *modem,
					DBusMessage *msg,
					DBusMessageIter *var)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	ofono_bool_t lockdown;
	dbus_bool_t powered;
	const char *caller;
	int err;

	if (dbus_message_iter_get_arg_type(var) != DBUS_TYPE_BOOLEAN)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(var, &lockdown);

	if (modem->pending != NULL)
		return __ofono_error_busy(msg);

	caller = dbus_message_get_sender(msg);

	if (modem->lockdown && g_strcmp0(caller, modem->lock_owner))
		return __ofono_error_access_denied(msg);

	if (modem->lockdown == lockdown)
		return dbus_message_new_method_return(msg);

	if (lockdown == FALSE) {
		lockdown_remove(modem);
		goto done;
	}

	if (ofono_modem_get_emergency_mode(modem) == TRUE)
		return __ofono_error_emergency_active(msg);

	modem->lock_owner = g_strdup(caller);

	modem->lock_watch = g_dbus_add_disconnect_watch(conn,
				modem->lock_owner, lockdown_disconnect,
				modem, NULL);

	if (modem->lock_watch == 0) {
		g_free(modem->lock_owner);
		modem->lock_owner = NULL;

		return __ofono_error_failed(msg);
	}

	modem->lockdown = lockdown;

	if (modem->powered == FALSE)
		goto done;

	err = set_powered(modem, FALSE);
	if (err < 0) {
		if (err != -EINPROGRESS) {
			lockdown_remove(modem);
			return __ofono_error_failed(msg);
		}

		modem->pending = dbus_message_ref(msg);
		modem->timeout = g_timeout_add_seconds(20,
						set_powered_timeout, modem);
		return NULL;
	}

	set_online(modem, FALSE);

	powered = FALSE;
	ofono_dbus_signal_property_changed(conn, modem->path,
					OFONO_MODEM_INTERFACE,
					"Powered", DBUS_TYPE_BOOLEAN,
					&powered);

done:
	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, modem->path,
					OFONO_MODEM_INTERFACE,
					"Lockdown", DBUS_TYPE_BOOLEAN,
					&lockdown);

	return NULL;
}

static DBusMessage *modem_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	DBusMessageIter iter, var;
	const char *name;

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	if (powering_down == TRUE)
		return __ofono_error_failed(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_str_equal(name, "Online"))
		return set_property_online(modem, msg, &var);

	if (g_str_equal(name, "Powered") == TRUE) {
		ofono_bool_t powered;
		int err;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &powered);

		if (modem->pending != NULL)
			return __ofono_error_busy(msg);

		if (modem->powered == powered)
			return dbus_message_new_method_return(msg);

		if (ofono_modem_get_emergency_mode(modem) == TRUE)
			return __ofono_error_emergency_active(msg);

		if (modem->lockdown)
			return __ofono_error_access_denied(msg);

		err = set_powered(modem, powered);
		if (err < 0) {
			if (err != -EINPROGRESS)
				return __ofono_error_failed(msg);

			modem->pending = dbus_message_ref(msg);
			modem->timeout = g_timeout_add_seconds(20,
						set_powered_timeout, modem);
			return NULL;
		}

		g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

		ofono_dbus_signal_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Powered", DBUS_TYPE_BOOLEAN,
						&powered);

		if (powered) {
			modem_change_state(modem, MODEM_STATE_PRE_SIM);

			/* Force SIM Ready for devies with no sim atom */
			if (modem_has_sim(modem) == FALSE)
				sim_state_watch(OFONO_SIM_STATE_READY, modem);
		} else {
			set_online(modem, FALSE);
			modem_change_state(modem, MODEM_STATE_POWER_OFF);
		}

		return NULL;
	}

	if (g_str_equal(name, "Lockdown"))
		return set_property_lockdown(modem, msg, &var);

	return __ofono_error_invalid_args(msg);
}

static GDBusMethodTable modem_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	modem_get_properties },
	{ "SetProperty",	"sv",	"",		modem_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable modem_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

void ofono_modem_set_powered(struct ofono_modem *modem, ofono_bool_t powered)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t dbus_powered = powered;

	if (modem->timeout > 0) {
		g_source_remove(modem->timeout);
		modem->timeout = 0;
	}

	if (modem->powered_pending != modem->powered &&
						modem->pending != NULL) {
		DBusMessage *reply;

		if (powered == modem->powered_pending)
			reply = dbus_message_new_method_return(modem->pending);
		else
			reply = __ofono_error_failed(modem->pending);

		__ofono_dbus_pending_reply(&modem->pending, reply);
	}

	modem->powered_pending = powered;

	if (modem->powered == powered)
		goto out;

	modem->powered = powered;
	notify_powered_watches(modem);

	if (modem->lockdown)
		ofono_dbus_signal_property_changed(conn, modem->path,
					OFONO_MODEM_INTERFACE,
					"Lockdown", DBUS_TYPE_BOOLEAN,
					&modem->lockdown);

	if (modem->driver == NULL) {
		ofono_error("Calling ofono_modem_set_powered on a"
				"modem with no driver is not valid, "
				"please fix the modem driver.");
		return;
	}

	ofono_dbus_signal_property_changed(conn, modem->path,
					OFONO_MODEM_INTERFACE,
					"Powered", DBUS_TYPE_BOOLEAN,
					&dbus_powered);

	if (powered) {
		modem_change_state(modem, MODEM_STATE_PRE_SIM);

		/* Force SIM Ready for devices with no sim atom */
		if (modem_has_sim(modem) == FALSE)
			sim_state_watch(OFONO_SIM_STATE_READY, modem);
	} else {
		set_online(modem, FALSE);

		modem_change_state(modem, MODEM_STATE_POWER_OFF);
	}

out:
	if (powering_down && powered == FALSE) {
		modems_remaining -= 1;

		if (modems_remaining == 0)
			__ofono_exit();
	}
}

ofono_bool_t ofono_modem_get_powered(struct ofono_modem *modem)
{
	if (modem == NULL)
		return FALSE;

	return modem->powered;
}

static gboolean trigger_interface_update(void *data)
{
	struct ofono_modem *modem = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	char **interfaces;
	char **features;
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

	features = g_new0(char *, g_slist_length(modem->feature_list) + 1);
	for (i = 0, l = modem->feature_list; l; l = l->next, i++)
		features[i] = l->data;
	ofono_dbus_signal_array_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Features", DBUS_TYPE_STRING,
						&features);
	g_free(features);

	modem->interface_update = 0;

	return FALSE;
}

static const struct {
	const char *interface;
	const char *feature;
} feature_map[] = {
	{ OFONO_NETWORK_REGISTRATION_INTERFACE,		"net"	},
	{ OFONO_RADIO_SETTINGS_INTERFACE,		"rat"	},
	{ OFONO_CELL_BROADCAST_INTERFACE,		"cbs"	},
	{ OFONO_MESSAGE_MANAGER_INTERFACE,		"sms"	},
	{ OFONO_SIM_MANAGER_INTERFACE,			"sim"	},
	{ OFONO_STK_INTERFACE,				"stk"	},
	{ OFONO_SUPPLEMENTARY_SERVICES_INTERFACE,	"ussd"	},
	{ OFONO_CONNECTION_MANAGER_INTERFACE,		"gprs"	},
	{ OFONO_TEXT_TELEPHONY_INTERFACE,		"tty"	},
	{ OFONO_LOCATION_REPORTING_INTERFACE,		"gps"	},
	{ },
};

static const char *get_feature(const char *interface)
{
	int i;

	for (i = 0; feature_map[i].interface; i++) {
		if (strcmp(feature_map[i].interface, interface) == 0)
			return feature_map[i].feature;
	}

	return NULL;
}

void ofono_modem_add_interface(struct ofono_modem *modem,
				const char *interface)
{
	const char *feature;

	modem->interface_list = g_slist_prepend(modem->interface_list,
						g_strdup(interface));

	feature = get_feature(interface);
	if (feature)
		modem->feature_list = g_slist_prepend(modem->feature_list,
							g_strdup(feature));

	if (modem->interface_update != 0)
		return;

	modem->interface_update = g_idle_add(trigger_interface_update, modem);
}

void ofono_modem_remove_interface(struct ofono_modem *modem,
				const char *interface)
{
	GSList *found;
	const char *feature;

	found = g_slist_find_custom(modem->interface_list, interface,
						(GCompareFunc) strcmp);
	if (found == NULL) {
		ofono_error("Interface %s not found on the interface_list",
				interface);
		return;
	}

	g_free(found->data);
	modem->interface_list = g_slist_remove(modem->interface_list,
						found->data);

	feature = get_feature(interface);
	if (feature) {
		found = g_slist_find_custom(modem->feature_list, feature,
						(GCompareFunc) strcmp);
		if (found) {
			g_free(found->data);
			modem->feature_list =
				g_slist_remove(modem->feature_list,
						found->data);
		}
	}

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
	if (info->driver->query_serial == NULL)
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
	if (info->driver->query_revision == NULL) {
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
	if (info->driver->query_model == NULL) {
		/* If model is not supported, don't bother querying revision */
		query_serial(info);
		return;
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
						"Manufacturer",
						DBUS_TYPE_STRING,
						&info->manufacturer);

out:
	query_model(info);
}

static gboolean query_manufacturer(gpointer user)
{
	struct ofono_devinfo *info = user;

	if (info->driver->query_manufacturer == NULL) {
		query_model(info);
		return FALSE;
	}

	info->driver->query_manufacturer(info, query_manufacturer_cb, info);

	return FALSE;
}

static void attr_template(struct ofono_emulator *em,
				struct ofono_emulator_request *req,
				const char *attr)
{
	struct ofono_error result;

	if (attr == NULL)
		attr = "Unknown";

	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY:
		ofono_emulator_send_info(em, attr, TRUE);
		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		ofono_emulator_send_final(em, &result);
		break;
	case OFONO_EMULATOR_REQUEST_TYPE_SUPPORT:
		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		ofono_emulator_send_final(em, &result);
		break;
	default:
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
	};
}

static void gmi_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_devinfo *info = userdata;

	attr_template(em, req, info->manufacturer);
}

static void gmm_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_devinfo *info = userdata;

	attr_template(em, req, info->model);
}

static void gmr_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_devinfo *info = userdata;

	attr_template(em, req, info->revision);
}

static void gcap_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	attr_template(em, req, "+GCAP: +CGSM");
}

static void dun_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED)
		return;

	ofono_emulator_add_handler(em, "+GMI", gmi_cb, data, NULL);
	ofono_emulator_add_handler(em, "+GMM", gmm_cb, data, NULL);
	ofono_emulator_add_handler(em, "+GMR", gmr_cb, data, NULL);
	ofono_emulator_add_handler(em, "+GCAP", gcap_cb, data, NULL);
}

int ofono_devinfo_driver_register(const struct ofono_devinfo_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_devinfo_drivers = g_slist_prepend(g_devinfo_drivers, (void *) d);

	return 0;
}

void ofono_devinfo_driver_unregister(const struct ofono_devinfo_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_devinfo_drivers = g_slist_remove(g_devinfo_drivers, (void *) d);
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

	g_free(info);
}

struct ofono_devinfo *ofono_devinfo_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data)
{
	struct ofono_devinfo *info;
	GSList *l;

	info = g_new0(struct ofono_devinfo, 1);

	info->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_DEVINFO,
						devinfo_remove, info);

	for (l = g_devinfo_drivers; l; l = l->next) {
		const struct ofono_devinfo_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(info, vendor, data) < 0)
			continue;

		info->driver = drv;
		break;
	}

	return info;
}

static void devinfo_unregister(struct ofono_atom *atom)
{
	struct ofono_devinfo *info = __ofono_atom_get_data(atom);

	g_free(info->manufacturer);
	info->manufacturer = NULL;

	g_free(info->model);
	info->model = NULL;

	g_free(info->revision);
	info->revision = NULL;

	g_free(info->serial);
	info->serial = NULL;
}

void ofono_devinfo_register(struct ofono_devinfo *info)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(info->atom);

	__ofono_atom_register(info->atom, devinfo_unregister);

	info->dun_watch = __ofono_modem_add_atom_watch(modem,
						OFONO_ATOM_TYPE_EMULATOR_DUN,
						dun_watch, info, NULL);

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

static void unregister_property(gpointer data)
{
	struct modem_property *property = data;

	DBG("property %p", property);

	g_free(property->value);
	g_free(property);
}

static int set_modem_property(struct ofono_modem *modem, const char *name,
				enum property_type type, const void *value)
{
	struct modem_property *property;

	DBG("modem %p property %s", modem, name);

	if (type != PROPERTY_TYPE_STRING &&
			type != PROPERTY_TYPE_INTEGER)
		return -EINVAL;

	property = g_try_new0(struct modem_property, 1);
	if (property == NULL)
		return -ENOMEM;

	property->type = type;

	switch (type) {
	case PROPERTY_TYPE_STRING:
		property->value = g_strdup((const char *) value);
		break;
	case PROPERTY_TYPE_INTEGER:
		property->value = g_memdup(value, sizeof(int));
		break;
	case PROPERTY_TYPE_BOOLEAN:
		property->value = g_memdup(value, sizeof(ofono_bool_t));
		break;
	default:
		break;
	}

	g_hash_table_replace(modem->properties, g_strdup(name), property);

	return 0;
}

static gboolean get_modem_property(struct ofono_modem *modem, const char *name,
					enum property_type type,
					void *value)
{
	struct modem_property *property;

	DBG("modem %p property %s", modem, name);

	property = g_hash_table_lookup(modem->properties, name);

	if (property == NULL)
		return FALSE;

	if (property->type != type)
		return FALSE;

	switch (property->type) {
	case PROPERTY_TYPE_STRING:
		*((const char **) value) = property->value;
		return TRUE;
	case PROPERTY_TYPE_INTEGER:
		memcpy(value, property->value, sizeof(int));
		return TRUE;
	case PROPERTY_TYPE_BOOLEAN:
		memcpy(value, property->value, sizeof(ofono_bool_t));
		return TRUE;
	default:
		return FALSE;
	}
}

int ofono_modem_set_string(struct ofono_modem *modem,
				const char *key, const char *value)
{
	return set_modem_property(modem, key, PROPERTY_TYPE_STRING, value);
}

int ofono_modem_set_integer(struct ofono_modem *modem,
				const char *key, int value)
{
	return set_modem_property(modem, key, PROPERTY_TYPE_INTEGER, &value);
}

int ofono_modem_set_boolean(struct ofono_modem *modem,
				const char *key, ofono_bool_t value)
{
	return set_modem_property(modem, key, PROPERTY_TYPE_BOOLEAN, &value);
}

const char *ofono_modem_get_string(struct ofono_modem *modem, const char *key)
{
	const char *value;

	if (get_modem_property(modem, key,
				PROPERTY_TYPE_STRING, &value) == FALSE)
		return NULL;

	return value;
}

int ofono_modem_get_integer(struct ofono_modem *modem, const char *key)
{
	int value;

	if (get_modem_property(modem, key,
				PROPERTY_TYPE_INTEGER, &value) == FALSE)
		return 0;

	return value;
}

ofono_bool_t ofono_modem_get_boolean(struct ofono_modem *modem, const char *key)
{
	ofono_bool_t value;

	if (get_modem_property(modem, key,
				PROPERTY_TYPE_BOOLEAN, &value) == FALSE)
		return FALSE;

	return value;
}

void ofono_modem_set_name(struct ofono_modem *modem, const char *name)
{
	if (modem->name)
		g_free(modem->name);

	modem->name = g_strdup(name);

	if (modem->driver) {
		DBusConnection *conn = ofono_dbus_get_connection();

		ofono_dbus_signal_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Name", DBUS_TYPE_STRING,
						&modem->name);
	}
}

struct ofono_modem *ofono_modem_create(const char *name, const char *type)
{
	struct ofono_modem *modem;
	char path[128];

	DBG("name: %s, type: %s", name, type);

	if (strlen(type) > 16)
		return NULL;

	if (name && strlen(name) > 64)
		return NULL;

	if (name == NULL)
		snprintf(path, sizeof(path), "/%s_%d", type, next_modem_id);
	else
		snprintf(path, sizeof(path), "/%s", name);

	if (__ofono_dbus_valid_object_path(path) == FALSE)
		return NULL;

	modem = g_try_new0(struct ofono_modem, 1);

	if (modem == NULL)
		return modem;

	modem->path = g_strdup(path);
	modem->driver_type = g_strdup(type);
	modem->properties = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, unregister_property);

	g_modem_list = g_slist_prepend(g_modem_list, modem);

	if (name == NULL)
		next_modem_id += 1;

	return modem;
}

static void sim_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *data)
{
	struct ofono_modem *modem = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		modem->sim_ready_watch = 0;
		return;
	}

	modem->sim = __ofono_atom_get_data(atom);
	modem->sim_ready_watch = ofono_sim_add_state_watch(modem->sim,
							sim_state_watch,
							modem, NULL);
}

void __ofono_modemwatch_init(void)
{
	g_modemwatches = __ofono_watchlist_new(g_free);
}

void __ofono_modemwatch_cleanup(void)
{
	__ofono_watchlist_free(g_modemwatches);
}

unsigned int __ofono_modemwatch_add(ofono_modemwatch_cb_t cb, void *user,
					ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *watch;

	if (cb == NULL)
		return 0;

	watch = g_new0(struct ofono_watchlist_item, 1);

	watch->notify = cb;
	watch->destroy = destroy;
	watch->notify_data = user;

	return __ofono_watchlist_add_item(g_modemwatches, watch);
}

gboolean __ofono_modemwatch_remove(unsigned int id)
{
	return __ofono_watchlist_remove_item(g_modemwatches, id);
}

static void call_modemwatches(struct ofono_modem *modem, gboolean added)
{
	GSList *l;
	struct ofono_watchlist_item *watch;
	ofono_modemwatch_cb_t notify;

	DBG("%p added:%d", modem, added);

	for (l = g_modemwatches->items; l; l = l->next) {
		watch = l->data;

		notify = watch->notify;
		notify(modem, added, watch->notify_data);
	}
}

static void emit_modem_added(struct ofono_modem *modem)
{
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *path;

	DBG("%p", modem);

	signal = dbus_message_new_signal(OFONO_MANAGER_PATH,
						OFONO_MANAGER_INTERFACE,
						"ModemAdded");

	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);

	path = modem->path;
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	__ofono_modem_append_properties(modem, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(ofono_dbus_get_connection(), signal);
}

ofono_bool_t ofono_modem_is_registered(struct ofono_modem *modem)
{
	if (modem == NULL)
		return FALSE;

	if (modem->driver == NULL)
		return FALSE;

	return TRUE;
}

int ofono_modem_register(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	GSList *l;

	DBG("%p", modem);

	if (modem == NULL)
		return -EINVAL;

	if (powering_down == TRUE)
		return -EBUSY;

	if (modem->driver != NULL)
		return -EALREADY;

	for (l = g_driver_list; l; l = l->next) {
		const struct ofono_modem_driver *drv = l->data;

		if (g_strcmp0(drv->name, modem->driver_type))
			continue;

		if (drv->probe(modem) < 0)
			continue;

		modem->driver = drv;
		break;
	}

	if (modem->driver == NULL)
		return -ENODEV;

	if (!g_dbus_register_interface(conn, modem->path,
					OFONO_MODEM_INTERFACE,
					modem_methods, modem_signals, NULL,
					modem, NULL)) {
		ofono_error("Modem register failed on path %s", modem->path);

		if (modem->driver->remove)
			modem->driver->remove(modem);

		modem->driver = NULL;

		return -EIO;
	}

	g_free(modem->driver_type);
	modem->driver_type = NULL;

	modem->atom_watches = __ofono_watchlist_new(g_free);
	modem->online_watches = __ofono_watchlist_new(g_free);
	modem->powered_watches = __ofono_watchlist_new(g_free);

	emit_modem_added(modem);
	call_modemwatches(modem, TRUE);

	modem->sim_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_SIM,
					sim_watch, modem, NULL);

	return 0;
}

static void emit_modem_removed(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = modem->path;

	DBG("%p", modem);

	g_dbus_emit_signal(conn, OFONO_MANAGER_PATH, OFONO_MANAGER_INTERFACE,
				"ModemRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
}

static void modem_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("%p", modem);

	if (modem->powered == TRUE)
		set_powered(modem, FALSE);

	__ofono_watchlist_free(modem->atom_watches);
	modem->atom_watches = NULL;

	__ofono_watchlist_free(modem->online_watches);
	modem->online_watches = NULL;

	__ofono_watchlist_free(modem->powered_watches);
	modem->powered_watches = NULL;

	modem->sim_watch = 0;
	modem->sim_ready_watch = 0;

	g_slist_foreach(modem->interface_list, (GFunc) g_free, NULL);
	g_slist_free(modem->interface_list);
	modem->interface_list = NULL;

	g_slist_foreach(modem->feature_list, (GFunc) g_free, NULL);
	g_slist_free(modem->feature_list);
	modem->feature_list = NULL;

	if (modem->timeout) {
		g_source_remove(modem->timeout);
		modem->timeout = 0;
	}

	if (modem->pending) {
		dbus_message_unref(modem->pending);
		modem->pending = NULL;
	}

	if (modem->interface_update) {
		g_source_remove(modem->interface_update);
		modem->interface_update = 0;
	}

	if (modem->lock_watch) {
		lockdown_remove(modem);

		ofono_dbus_signal_property_changed(conn, modem->path,
					OFONO_MODEM_INTERFACE,
					"Lockdown", DBUS_TYPE_BOOLEAN,
					&modem->lockdown);
	}

	g_dbus_unregister_interface(conn, modem->path, OFONO_MODEM_INTERFACE);

	if (modem->driver && modem->driver->remove)
		modem->driver->remove(modem);

	g_hash_table_destroy(modem->properties);
	modem->properties = NULL;

	modem->driver = NULL;

	emit_modem_removed(modem);
	call_modemwatches(modem, FALSE);
}

void ofono_modem_remove(struct ofono_modem *modem)
{
	DBG("%p", modem);

	if (modem == NULL)
		return;

	if (modem->driver)
		modem_unregister(modem);

	g_modem_list = g_slist_remove(g_modem_list, modem);

	g_free(modem->driver_type);
	g_free(modem->name);
	g_free(modem->path);
	g_free(modem);
}

void ofono_modem_reset(struct ofono_modem *modem)
{
	int err;

	DBG("%p", modem);

	if (modem->pending) {
		DBusMessage *reply = __ofono_error_failed(modem->pending);
		__ofono_dbus_pending_reply(&modem->pending, reply);
	}

	if (modem->modem_state == MODEM_STATE_ONLINE)
		modem->get_online = TRUE;

	ofono_modem_set_powered(modem, FALSE);

	err = set_powered(modem, TRUE);
	if (err == -EINPROGRESS)
		return;

	modem_change_state(modem, MODEM_STATE_PRE_SIM);
}

void __ofono_modem_sim_reset(struct ofono_modem *modem)
{
	DBG("%p", modem);

	modem_change_state(modem, MODEM_STATE_PRE_SIM);
}

int ofono_modem_driver_register(const struct ofono_modem_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_driver_list = g_slist_prepend(g_driver_list, (void *) d);

	return 0;
}

void ofono_modem_driver_unregister(const struct ofono_modem_driver *d)
{
	GSList *l;
	struct ofono_modem *modem;

	DBG("driver: %p, name: %s", d, d->name);

	g_driver_list = g_slist_remove(g_driver_list, (void *) d);

	for (l = g_modem_list; l; l = l->next) {
		modem = l->data;

		if (modem->driver != d)
			continue;

		modem_unregister(modem);
	}
}

void __ofono_modem_shutdown(void)
{
	struct ofono_modem *modem;
	GSList *l;

	powering_down = TRUE;

	for (l = g_modem_list; l; l = l->next) {
		modem = l->data;

		if (modem->driver == NULL)
			continue;

		if (modem->powered == FALSE && modem->powered_pending == FALSE)
			continue;

		if (set_powered(modem, FALSE) == -EINPROGRESS)
			modems_remaining += 1;
	}

	if (modems_remaining == 0)
		__ofono_exit();
}

void __ofono_modem_foreach(ofono_modem_foreach_func func, void *userdata)
{
	struct ofono_modem *modem;
	GSList *l;

	for (l = g_modem_list; l; l = l->next) {
		modem = l->data;
		func(modem, userdata);
	}
}

ofono_bool_t ofono_modem_get_emergency_mode(struct ofono_modem *modem)
{
	return modem->emergency != 0;
}

void __ofono_modem_inc_emergency_mode(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t emergency = TRUE;

	if (++modem->emergency > 1)
		return;

	ofono_dbus_signal_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Emergency", DBUS_TYPE_BOOLEAN,
						&emergency);
}

void __ofono_modem_dec_emergency_mode(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t emergency = FALSE;

	if (modem->emergency == 0) {
		ofono_error("emergency mode is already deactivated!!!");
		return;
	}

	if (modem->emergency > 1)
		goto out;

	ofono_dbus_signal_property_changed(conn, modem->path,
						OFONO_MODEM_INTERFACE,
						"Emergency", DBUS_TYPE_BOOLEAN,
						&emergency);

out:
	modem->emergency--;
}
