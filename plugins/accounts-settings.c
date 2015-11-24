/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C)  2016 Canonical Ltd.
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/types.h>
#include <pwd.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>
#include <gdbus.h>
#include <systemd/sd-login.h>

#include <ofono.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/system-settings.h>

#define ACCOUNTS_SERVICE          "org.freedesktop.Accounts"
#define ACCOUNTS_PATH             "/org/freedesktop/Accounts/User"
#define ACCOUNTS_PHONE_INTERFACE  "com.ubuntu.touch.AccountsService.Phone"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

struct setting {
	char* ofono_name;
	char* property_name;
};

static const struct setting setting_names[] = {
	{ PREFERRED_VOICE_MODEM, "DefaultSimForCalls" }
};

#define NUM_SETTINGS G_N_ELEMENTS(setting_names)

struct accounts_data {
	sd_login_monitor *login_monitor;
	guint login_watch;
	char *property_values[NUM_SETTINGS];
	uid_t current_uid;
	guint prop_change_watch;
	char uid_path[sizeof(ACCOUNTS_PATH) + 32];
};

static struct accounts_data g_accounts;

static int translate_name(const char *name)
{
	size_t i;

	for (i = 0; i < NUM_SETTINGS; ++i) {
		if (g_strcmp0(setting_names[i].ofono_name, name) != 0)
			continue;

		return i;
	}

	return -1;
}

static char *accounts_settings_get_string_value(const char *name)
{
	int id;

	id = translate_name(name);
	if (id < 0)
		return NULL;

	return g_strdup(g_accounts.property_values[id]);
}

static struct ofono_system_settings_driver accounts_settings_driver = {
	.name	 = "Accounts Service System Settings",
	.get_string_value = accounts_settings_get_string_value
};

static int get_id_from_name(const char *name)
{
	size_t i;

	for (i = 0; i < NUM_SETTINGS; ++i) {
		if (g_strcmp0(setting_names[i].property_name, name) != 0)
			continue;

		return i;
	}

	return -1;
}

static char *get_property_value(DBusMessage *reply)
{
	DBusMessageIter iter, val;
	const char *ptr;
	char *property = NULL;

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		ofono_error("%s: ERROR reply to Get", __func__);
		goto done;
	}

	if (dbus_message_iter_init(reply, &iter) == FALSE) {
		ofono_error("%s: error initializing array iter", __func__);
		goto done;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
		ofono_error("%s: type != VARIANT!", __func__);
		goto done;
	}

	dbus_message_iter_recurse(&iter, &val);

	if (dbus_message_iter_get_arg_type(&val) != DBUS_TYPE_STRING) {
		ofono_error("%s: type != STRING!", __func__);
		goto done;
	}

	dbus_message_iter_get_basic(&val, &ptr);

	property = g_strdup(ptr);

done:
	return property;
}

struct get_property_data {
	struct accounts_data *accounts;
	int prop_id;
};

static void get_property_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply;
	struct get_property_data *gpd = user_data;
	char **values = gpd->accounts->property_values;
	int id = gpd->prop_id;

	reply = dbus_pending_call_steal_reply(call);
	if (reply == NULL) {
		ofono_error("%s: failed to get reply", __func__);
		goto done;
	}

	g_free(values[id]);
	values[id] = get_property_value(reply);
	dbus_message_unref(reply);

	DBG("property %s has value %s",
		setting_names[id].property_name, PRINTABLE_STR(values[id]));

done:
	dbus_pending_call_unref(call);
	g_free(gpd);
}

static void get_property(struct accounts_data *accounts, int id)
{
	DBusMessageIter iter;
	DBusMessage *msg;
	const char *iface = ACCOUNTS_PHONE_INTERFACE;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusPendingCall *call;
	struct get_property_data *gpd;

	msg = dbus_message_new_method_call(ACCOUNTS_SERVICE, accounts->uid_path,
						DBUS_PROPERTIES_INTERFACE,
						"Get");
	if (msg == NULL) {
		ofono_error("%s: dbus_message_new_method failed", __func__);
		return;
	}

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
					&setting_names[id].property_name);

	if (!dbus_connection_send_with_reply(conn, msg, &call, -1)) {
		ofono_error("%s: Sending Get failed", __func__);
		goto done;
	}

	gpd = g_malloc0(sizeof(*gpd));
	gpd->accounts = accounts;
	gpd->prop_id = id;
	dbus_pending_call_set_notify(call, get_property_reply, gpd, NULL);

done:
	dbus_message_unref(msg);
}

static gboolean property_changed(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct accounts_data *accounts = user_data;
	const char *iface;
	DBusMessageIter iter, dict, inv_it;

	dbus_message_iter_init(msg, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
		ofono_error("%s: iface != TYPE_STRING!", __func__);
		goto done;
	}

	dbus_message_iter_get_basic(&iter, &iface);

	/* We can receive notifications from other AccountsService interfaces */
	if (g_str_equal(iface, ACCOUNTS_PHONE_INTERFACE) != TRUE)
		goto done;

	if (!dbus_message_iter_next(&iter)) {
		ofono_error("%s: advance iter failed!", __func__);
		goto done;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		ofono_error("%s: type != ARRAY!", __func__);
		goto done;
	}

	dbus_message_iter_recurse(&iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, val;
		const char *key, *str_val;
		int id;

		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) !=
				DBUS_TYPE_STRING) {
			ofono_error("%s: key type != STRING!", __func__);
			goto done;
		}

		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) !=
				DBUS_TYPE_VARIANT) {
			ofono_error("%s: val != VARIANT", __func__);
			goto done;
		}

		dbus_message_iter_recurse(&entry, &val);

		if (dbus_message_iter_get_arg_type(&val) != DBUS_TYPE_STRING) {
			ofono_error("%s: *val != STRING", __func__);
			goto done;
		}

		dbus_message_iter_get_basic(&val, &str_val);

		DBG("property %s changed to %s", key, PRINTABLE_STR(str_val));

		id = get_id_from_name(key);
		if (id >= 0)
			accounts->property_values[id] = g_strdup(str_val);

		dbus_message_iter_next(&dict);
	}

	/* Check invalidated properties */
	if (!dbus_message_iter_next(&iter)) {
		ofono_error("%s: advance iter failed!", __func__);
		goto done;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		ofono_error("%s: type != ARRAY!", __func__);
		goto done;
	}

	dbus_message_iter_recurse(&iter, &inv_it);

	while (dbus_message_iter_get_arg_type(&inv_it) == DBUS_TYPE_STRING) {
		const char *inv_name;
		int id;

		dbus_message_iter_get_basic(&inv_it, &inv_name);

		DBG("property %s invalidated", inv_name);

		id = get_id_from_name(inv_name);
		if (id >= 0)
			get_property(accounts, id);

		dbus_message_iter_next(&inv_it);
	}

done:
	return TRUE;
}

static void get_user_settings(struct accounts_data *accounts)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	size_t i;

	snprintf(accounts->uid_path, sizeof(accounts->uid_path),
			ACCOUNTS_PATH"%u", (unsigned) accounts->current_uid);

	/*
	 * Register for property changes. Note that AccountsService is D-Bus,
	 * initiated, so there is no risk for race conditions on start.
	 */
	accounts->prop_change_watch =
		g_dbus_add_signal_watch(conn, ACCOUNTS_SERVICE,
						accounts->uid_path,
						DBUS_INTERFACE_PROPERTIES,
						"PropertiesChanged",
						property_changed,
						accounts, NULL);

	/* Retrieve AccountService properties */
	for (i = 0; i < NUM_SETTINGS; ++i)
		get_property(accounts, i);
}

static void release_accounts_data(struct accounts_data *accounts)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	size_t i;

	g_dbus_remove_watch(conn, accounts->prop_change_watch);

	for (i = 0; i < NUM_SETTINGS; ++i) {
		g_free(accounts->property_values[i]);
		accounts->property_values[i] = NULL;
	}
}

static uid_t get_active_seat_uid(void)
{
	char **seats, **iter;
	int res;
	gboolean found = FALSE;
	uid_t uid;

	res = sd_get_seats(&seats);
	if (res < 0) {
		ofono_error("Error retrieving seats: %s (%d)",
							strerror(-res), -res);
		goto end;
	} else if (res == 0) {
		ofono_info("No seats found");
		goto end;
	}

	for (iter = seats; *iter; ++iter) {

		if (!found && sd_seat_get_active(*iter, NULL, &uid) >= 0) {
			DBG("seat %s with uid %d", *iter, uid);
			found = TRUE;
		}

		free(*iter);
	}

	free(seats);

end:
	if (!found)
		uid = geteuid();

	return uid;
}

static gboolean sd_changed(GIOChannel *stream, GIOCondition condition,
							gpointer user_data)
{
	struct accounts_data *accounts = user_data;
	uid_t new_uid;

	sd_login_monitor_flush(accounts->login_monitor);

	new_uid = get_active_seat_uid();
	if (new_uid != accounts->current_uid) {
		/* User in seat changed, resetting data */
		release_accounts_data(accounts);

		accounts->current_uid = new_uid;
		get_user_settings(accounts);
	}

	return TRUE;
}

static void sd_init(struct accounts_data *accounts)
{
	int status;
	GIOChannel *stream;
	int fd;

	status = sd_login_monitor_new(NULL, &accounts->login_monitor);
	if (status < 0) {
		ofono_error("Error creating systemd login monitor: %d", status);
		accounts->login_monitor = NULL;
		return;
	}

	fd = sd_login_monitor_get_fd(accounts->login_monitor);
	stream = g_io_channel_unix_new(fd);
	accounts->login_watch =
			g_io_add_watch(stream, G_IO_IN, sd_changed, accounts);

	g_io_channel_unref(stream);
}

static void sd_finalize(struct accounts_data *accounts)
{
	if (accounts->login_monitor == NULL)
		return;

	g_source_remove(accounts->login_watch);
	sd_login_monitor_unref(accounts->login_monitor);
}

static int accounts_settings_init(void)
{
	g_accounts.current_uid = get_active_seat_uid();

	/* Register for login changes */
	sd_init(&g_accounts);

	/* Get property values for current user */
	get_user_settings(&g_accounts);

	return ofono_system_settings_driver_register(&accounts_settings_driver);
}

static void accounts_settings_exit(void)
{
	ofono_system_settings_driver_unregister(&accounts_settings_driver);

	sd_finalize(&g_accounts);

	release_accounts_data(&g_accounts);
}

OFONO_PLUGIN_DEFINE(accounts_settings,
			"Accounts Service System Settings Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			accounts_settings_init, accounts_settings_exit)
