/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011 ST-Ericsson AB.
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

#include <errno.h>
#include <string.h>
#include <net/if.h>

#include <glib.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/dbus.h>

/*
 * ST-Ericsson's Modem Init Daemon is used for controlling the modem power
 * cycles and provides a dbus API for modem state and properties.
 */
#define MGR_SERVICE		"com.stericsson.modeminit"
#define MGR_INTERFACE		MGR_SERVICE ".Manager"
#define MGR_GET_MODEMS		"GetModems"
#define GET_MODEMS_TIMEOUT	5000

#define MGR_MODEM_INTERFACE	MGR_SERVICE ".Modem"
#define PROPERTY_CHANGED	"PropertyChanged"

enum ste_state {
	STE_STATE_OFF,
	STE_STATE_READY,
	STE_STATE_RESET
};

enum ste_operation {
	STE_OP_STARTING,
	STE_OP_READY,
	STE_OP_RESTART,
	STE_OP_OFF
};

struct ste_modem {
	char *path;
	struct ofono_modem *modem;
	enum ste_state state;
	char *serial;
	char *interface;
};

static GHashTable *modem_list;
static guint modem_daemon_watch;
static guint property_changed_watch;
static DBusConnection *connection;

static void state_change(struct ste_modem *stemodem, enum ste_operation op)
{
	switch (stemodem->state) {
	case STE_STATE_OFF:
		/*
		 * The STE Modem is in state OFF and we're waiting for
		 * the Modem Init Daemon to signal that modem is ready
		 * in order to create and register the modem.
		 */
		switch (op) {
		case STE_OP_READY:
			stemodem->modem = ofono_modem_create(stemodem->serial,
								"ste");
			if (stemodem->modem == NULL) {
				ofono_error("Could not create modem %s, %s",
						stemodem->path,
						stemodem->serial);
				return;
			}

			DBG("register modem %s, %s", stemodem->path,
				stemodem->serial);

			if (stemodem->interface != NULL)
				ofono_modem_set_string(stemodem->modem,
							"Interface",
							stemodem->interface);

			ofono_modem_register(stemodem->modem);
			stemodem->state = STE_STATE_READY;
			break;
		case STE_OP_STARTING:
		case STE_OP_RESTART:
		case STE_OP_OFF:
			break;
		}
		break;
	case STE_STATE_READY:
		/*
		 * The STE Modem is ready and the modem has been created
		 * and registered in oFono. In this state two things can
		 * happen: Modem restarts or is turned off. Turning off
		 * the modem is an exceptional situation e.g. high-temperature,
		 * low battery or upgrade. In this scenario we remove the
		 * STE modem from oFono.
		 */
		switch (op) {
		case STE_OP_READY:
			break;
		case STE_OP_STARTING:
		case STE_OP_RESTART:
			DBG("reset ongoing %s", stemodem->path);
			/* Note: Consider to power off modem here? */
			stemodem->state = STE_STATE_RESET;
			break;
		case STE_OP_OFF:
			DBG("STE modem unregistering %s", stemodem->path);
			ofono_modem_remove(stemodem->modem);
			stemodem->modem = NULL;
			stemodem->state = STE_STATE_OFF;
			break;
		}
		break;
	case STE_STATE_RESET:
		/*
		 * The STE Modem is resetting.In this state two things can
		 * happen: Modem restarts succeeds, or modem is turned off.
		 */
		switch (op) {
		case STE_OP_STARTING:
		case STE_OP_RESTART:
			break;
		case STE_OP_READY:
			DBG("STE modem reset complete %s", stemodem->path);
			if (ofono_modem_get_powered(stemodem->modem))
				ofono_modem_reset(stemodem->modem);
			stemodem->state = STE_STATE_READY;
			break;
		case STE_OP_OFF:
			DBG("STE modem unregistering %s", stemodem->path);
			ofono_modem_remove(stemodem->modem);
			stemodem->modem = NULL;
			stemodem->state = STE_STATE_OFF;
			break;
		}
		break;
	}
}

static void update_property(struct ste_modem *stemodem, const char *prop,
				DBusMessageIter *iter, enum ste_operation *op,
				gboolean *op_valid)
{
	const char *value;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRING)
		return;

	dbus_message_iter_get_basic(iter, &value);

	if (g_strcmp0(prop, "State") == 0) {
		*op_valid = TRUE;
		if (g_strcmp0(value, "booting") == 0)
			*op = STE_OP_STARTING;
		else if (g_strcmp0(value, "upgrading") == 0)
			*op = STE_OP_OFF;
		else if (g_strcmp0(value, "ready") == 0)
			*op = STE_OP_READY;
		else if (g_strcmp0(value, "off") == 0)
			*op = STE_OP_OFF;
		else if (g_strcmp0(value, "dumping") == 0)
			*op = STE_OP_RESTART;
		else
			*op_valid = FALSE;
	} else if (g_strcmp0(prop, "Interface") == 0) {
		g_free(stemodem->interface);
		stemodem->interface = g_strdup(value);
	} else if (g_strcmp0(prop, "Serial") == 0) {
		g_free(stemodem->serial);
		stemodem->serial = g_strdup(value);
	}
}

static void update_modem_properties(const char *path, DBusMessageIter *iter)
{
	enum ste_operation operation;
	gboolean operation_valid;
	struct ste_modem *stemodem = g_hash_table_lookup(modem_list, path);

	if (stemodem == NULL) {
		stemodem = g_try_new0(struct ste_modem, 1);
		if (stemodem == NULL)
			return;

		stemodem->path = g_strdup(path);
		stemodem->state = STE_STATE_OFF;
		g_hash_table_insert(modem_list, stemodem->path, stemodem);
	}

	while (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;

		dbus_message_iter_recurse(iter, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		update_property(stemodem, key, &value, &operation,
					&operation_valid);

		dbus_message_iter_next(iter);
	}

	if (operation_valid)
		state_change(stemodem, operation);
}

static void get_modems_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessageIter iter, list;
	DBusError err;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&err);

	if (dbus_set_error_from_message(&err, reply)) {
		ofono_error("%s: %s\n", err.name, err.message);
		dbus_error_free(&err);
		goto done;
	}

	if (!dbus_message_has_signature(reply, "a(oa{sv})"))
		goto done;

	if (!dbus_message_iter_init(reply, &iter))
		goto done;

	dbus_message_iter_recurse(&iter, &list);

	while (dbus_message_iter_get_arg_type(&list) == DBUS_TYPE_STRUCT) {
		DBusMessageIter entry, dict;
		const char *path;

		dbus_message_iter_recurse(&list, &entry);
		dbus_message_iter_get_basic(&entry, &path);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &dict);

		update_modem_properties(path, &dict);

		dbus_message_iter_next(&list);
	}

done:
	dbus_message_unref(reply);
}

static void get_modems(void)
{
	DBusMessage *message;
	DBusPendingCall *call;

	message = dbus_message_new_method_call(MGR_SERVICE, "/",
					MGR_INTERFACE, MGR_GET_MODEMS);
	if (message == NULL) {
		ofono_error("Unable to allocate new D-Bus message");
		goto error;
	}

	dbus_message_set_auto_start(message, FALSE);

	if (!dbus_connection_send_with_reply(connection, message, &call,
						GET_MODEMS_TIMEOUT)) {
		ofono_error("Sending D-Bus message failed");
		goto error;
	}

	if (call == NULL) {
		DBG("D-Bus connection not available");
		goto error;
	}

	dbus_pending_call_set_notify(call, get_modems_reply, NULL, NULL);
	dbus_pending_call_unref(call);

error:
	dbus_message_unref(message);
}

static gboolean property_changed(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	DBusMessageIter iter;
	struct ste_modem *stemodem;
	const char *key;
	enum ste_operation operation;
	gboolean operation_valid;

	stemodem = g_hash_table_lookup(modem_list,
					dbus_message_get_path(message));

	if (stemodem == NULL)
		return TRUE;


	if (!dbus_message_iter_init(message, &iter))
		return TRUE;

	dbus_message_iter_get_basic(&iter, &key);
	dbus_message_iter_next(&iter);

	update_property(stemodem, key, &iter, &operation, &operation_valid);

	if (operation_valid)
		state_change(stemodem, operation);

	return TRUE;
}

static void mgr_connect(DBusConnection *connection, void *user_data)
{
	property_changed_watch = g_dbus_add_signal_watch(connection, NULL,
						NULL,
						MGR_MODEM_INTERFACE,
						PROPERTY_CHANGED,
						property_changed,
						NULL, NULL);
	get_modems();
}

static void mgr_disconnect(DBusConnection *connection, void *user_data)
{
	g_hash_table_remove_all(modem_list);
	g_dbus_remove_watch(connection, property_changed_watch);
	property_changed_watch = 0;
}

static void destroy_stemodem(gpointer data)
{
	struct ste_modem *stemodem = data;

	ofono_modem_remove(stemodem->modem);

	g_free(stemodem->interface);
	g_free(stemodem->path);
	g_free(stemodem->serial);
	g_free(stemodem);
}

static int stemgr_init(void)
{
	connection = ofono_dbus_get_connection();

	modem_list = g_hash_table_new_full(g_str_hash, g_str_equal,
						NULL, destroy_stemodem);
	modem_daemon_watch = g_dbus_add_service_watch(connection, MGR_SERVICE,
				mgr_connect, mgr_disconnect, NULL, NULL);
	return 0;
}

static void stemgr_exit(void)
{
	g_hash_table_destroy(modem_list);
	g_dbus_remove_watch(connection, modem_daemon_watch);

	if (property_changed_watch > 0)
		g_dbus_remove_watch(connection, property_changed_watch);

}

OFONO_PLUGIN_DEFINE(stemgr, "ST-Ericsson Modem Init Daemon detection", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, stemgr_init, stemgr_exit)
