/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011 Nokia Corporation. All rights reserved.
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
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"
#include "common.h"

static GSList *g_drivers;

struct cdma_connman_settings {
	char *interface;
	gboolean static_ip;
	char *ip;
	char *netmask;
	char *gateway;
	char **dns;
};

struct ofono_cdma_connman {
	ofono_bool_t powered;
	ofono_bool_t dormant;
	struct cdma_connman_settings *settings;
	DBusMessage *pending;
	const struct ofono_cdma_connman_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	char username[OFONO_CDMA_CONNMAN_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_CDMA_CONNMAN_MAX_PASSWORD_LENGTH + 1];
};

static void cdma_connman_settings_free(struct cdma_connman_settings *settings)
{
	DBG("");

	g_free(settings->interface);
	g_free(settings->ip);
	g_free(settings->netmask);
	g_free(settings->gateway);
	g_strfreev(settings->dns);

	g_free(settings);
}

static void cdma_connman_ifupdown(const char *interface, ofono_bool_t active)
{
	struct ifreq ifr;
	int sk;

	DBG("");

	if (interface == NULL)
		return;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface, IFNAMSIZ);

	if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0)
		goto done;

	if (active == TRUE) {
		if (ifr.ifr_flags & IFF_UP)
			goto done;
		ifr.ifr_flags |= IFF_UP;
	} else {
		if (!(ifr.ifr_flags & IFF_UP))
			goto done;
		ifr.ifr_flags &= ~IFF_UP;
	}

	if (ioctl(sk, SIOCSIFFLAGS, &ifr) < 0)
		ofono_error("Failed to change interface flags");

done:
	close(sk);
}

static void cdma_connman_settings_append_variant(
					struct cdma_connman_settings *settings,
					DBusMessageIter *iter)
{
	DBusMessageIter variant;
	DBusMessageIter array;
	char typesig[5];
	char arraysig[6];
	const char *method;

	DBG("");

	arraysig[0] = DBUS_TYPE_ARRAY;
	arraysig[1] = typesig[0] = DBUS_DICT_ENTRY_BEGIN_CHAR;
	arraysig[2] = typesig[1] = DBUS_TYPE_STRING;
	arraysig[3] = typesig[2] = DBUS_TYPE_VARIANT;
	arraysig[4] = typesig[3] = DBUS_DICT_ENTRY_END_CHAR;
	arraysig[5] = typesig[4] = '\0';

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						arraysig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						typesig, &array);

	if (settings == NULL)
		goto done;

	ofono_dbus_dict_append(&array, "Interface",
				DBUS_TYPE_STRING, &settings->interface);

	if (settings->static_ip == TRUE)
		method = "static";
	else
		method = "dhcp";

	ofono_dbus_dict_append(&array, "Method", DBUS_TYPE_STRING, &method);

	if (settings->ip)
		ofono_dbus_dict_append(&array, "Address", DBUS_TYPE_STRING,
					&settings->ip);

	if (settings->netmask)
		ofono_dbus_dict_append(&array, "Netmask", DBUS_TYPE_STRING,
					&settings->netmask);

	if (settings->gateway)
		ofono_dbus_dict_append(&array, "Gateway", DBUS_TYPE_STRING,
					&settings->gateway);

	if (settings->dns)
		ofono_dbus_dict_append_array(&array, "DomainNameServers",
						DBUS_TYPE_STRING,
						&settings->dns);

done:
	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

static void cdma_connman_settings_signal(struct ofono_cdma_connman *cm)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	DBusMessage *signal;
	DBusMessageIter iter;
	const char *prop = "Settings";

	DBG("");

	path = __ofono_atom_get_path(cm->atom);

	signal = dbus_message_new_signal(path,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE,
				"PropertyChanged");
	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);

	cdma_connman_settings_append_variant(cm->settings, &iter);

	g_dbus_send_message(conn, signal);
}

static void cdma_connman_settings_update(struct ofono_cdma_connman *cm,
					const char *interface,
					ofono_bool_t static_ip,
					const char *ip, const char *netmask,
					const char *gateway, const char **dns)
{
	DBG("");

	if (cm->settings)
		cdma_connman_settings_free(cm->settings);

	cm->settings = g_try_new0(struct cdma_connman_settings, 1);
	if (cm->settings == NULL)
		return;

	cm->settings->interface = g_strdup(interface);
	cm->settings->static_ip = static_ip;
	cm->settings->ip = g_strdup(ip);
	cm->settings->netmask = g_strdup(netmask);
	cm->settings->gateway = g_strdup(gateway);
	cm->settings->dns = g_strdupv((char **)dns);

	cdma_connman_ifupdown(interface, TRUE);

	cdma_connman_settings_signal(cm);
}

static void cdma_connman_settings_reset(struct ofono_cdma_connman *cm)
{
	char *interface;

	DBG("");

	if (cm->settings == NULL)
		return;

	interface = cm->settings->interface;
	cm->settings->interface = NULL;

	cdma_connman_settings_free(cm->settings);
	cm->settings = NULL;

	cdma_connman_settings_signal(cm);

	cdma_connman_ifupdown(interface, FALSE);

	g_free(interface);
}

static void activate_callback(const struct ofono_error *error,
				const char *interface,
				ofono_bool_t static_ip,
				const char *ip, const char *netmask,
				const char *gateway, const char **dns,
				void *data)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_cdma_connman *cm = data;
	dbus_bool_t value;
	const char *path;

	DBG("%p %s", cm, interface);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Activating packet data service failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		return;
	}

	cm->powered = TRUE;
	__ofono_dbus_pending_reply(&cm->pending,
				dbus_message_new_method_return(cm->pending));

	/*
	 * If we don't have the interface, don't bother emitting any settings,
	 * as nobody can make use of them
	 */
	if (interface != NULL)
		cdma_connman_settings_update(cm, interface, static_ip,
						ip, netmask, gateway, dns);

	path = __ofono_atom_get_path(cm->atom);
	value = cm->powered;
	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE,
				"Powered", DBUS_TYPE_BOOLEAN, &value);
}

static void deactivate_callback(const struct ofono_error *error, void *data)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_cdma_connman *cm = data;
	dbus_bool_t value;
	const char *path;

	DBG("");

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Deactivating packet data service failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		return;
	}

	cm->powered = FALSE;
	__ofono_dbus_pending_reply(&cm->pending,
				dbus_message_new_method_return(cm->pending));

	cdma_connman_settings_reset(cm);

	path = __ofono_atom_get_path(cm->atom);
	value = cm->powered;
	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE,
				"Powered", DBUS_TYPE_BOOLEAN, &value);
}

static void cdma_connman_settings_append_properties(
						struct ofono_cdma_connman *cm,
						DBusMessageIter *dict)
{
	DBusMessageIter entry;
	const char *key = "Settings";

	DBG("");

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	cdma_connman_settings_append_variant(cm->settings, &entry);

	dbus_message_iter_close_container(dict, &entry);
}

static DBusMessage *cdma_connman_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_cdma_connman *cm = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t value;

	DBG("");

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	value = cm->powered;
	ofono_dbus_dict_append(&dict, "Powered", DBUS_TYPE_BOOLEAN, &value);

	value = cm->dormant;
	ofono_dbus_dict_append(&dict, "Dormant", DBUS_TYPE_BOOLEAN, &value);

	if (cm->settings)
		cdma_connman_settings_append_properties(cm, &dict);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *cdma_connman_set_username(struct ofono_cdma_connman *cm,
					DBusConnection *conn, DBusMessage *msg,
					const char *username)
{
	const char *path;

	if (strlen(username) > OFONO_CDMA_CONNMAN_MAX_USERNAME_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(username, cm->username))
		return dbus_message_new_method_return(msg);

	strcpy(cm->username, username);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	path = __ofono_atom_get_path(cm->atom);
	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE,
				"Username", DBUS_TYPE_STRING, &username);

	return NULL;
}

static DBusMessage *cdma_connman_set_password(struct ofono_cdma_connman *cm,
					DBusConnection *conn, DBusMessage *msg,
					const char *password)
{
	const char *path;

	if (strlen(password) > OFONO_CDMA_CONNMAN_MAX_PASSWORD_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(password, cm->password))
		return dbus_message_new_method_return(msg);

	strcpy(cm->password, password);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	path = __ofono_atom_get_path(cm->atom);
	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE,
				"Password", DBUS_TYPE_STRING, &password);

	return NULL;
}

static DBusMessage *cdma_connman_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_cdma_connman *cm = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	dbus_bool_t value;
	const char *str;

	DBG("");

	if (cm->pending)
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

	if (!strcmp(property, "Powered")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (cm->powered == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		if (cm->driver == NULL || cm->driver->activate == NULL ||
				cm->driver->deactivate == NULL)
			return __ofono_error_not_implemented(msg);

		cm->pending = dbus_message_ref(msg);

		/* TODO: add logic to support CDMA Network Registration */
		if (value)
			cm->driver->activate(cm, cm->username, cm->password,
						activate_callback, cm);
		else
			cm->driver->deactivate(cm, deactivate_callback, cm);

		return NULL;
	} else if (!strcmp(property, "Username")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);
		return cdma_connman_set_username(cm, conn, msg, str);
	} else if (!strcmp(property, "Password")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);
		return cdma_connman_set_password(cm, conn, msg, str);
	}

	/* TODO: Dormant property. Not yet supported. */

	return __ofono_error_invalid_args(msg);
}

static GDBusMethodTable cdma_connman_methods[] = {
	{ "GetProperties",	"",	"a{sv}",
						cdma_connman_get_properties },
	{ "SetProperty",	"sv",	"",	cdma_connman_set_property,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cdma_connman_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

int ofono_cdma_connman_driver_register(
				const struct ofono_cdma_connman_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_cdma_connman_driver_unregister(
				const struct ofono_cdma_connman_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void cdma_connman_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	DBG("");

	g_dbus_unregister_interface(conn, path,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE);
	ofono_modem_remove_interface(modem,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE);
}

static void cdma_connman_remove(struct ofono_atom *atom)
{
	struct ofono_cdma_connman *cm = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cm == NULL)
		return;

	if (cm->driver && cm->driver->remove)
		cm->driver->remove(cm);

	g_free(cm);
}

struct ofono_cdma_connman *ofono_cdma_connman_create(
						struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data)
{
	struct ofono_cdma_connman *cm;
	GSList *l;

	DBG("");

	if (driver == NULL)
		return NULL;

	cm = g_try_new0(struct ofono_cdma_connman, 1);
	if (cm == NULL)
		return NULL;

	cm->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPE_CDMA_CONNMAN,
					cdma_connman_remove, cm);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_cdma_connman_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cm, vendor, data) < 0)
			continue;

		cm->driver = drv;
		break;
	}

	return cm;
}

void ofono_cdma_connman_register(struct ofono_cdma_connman *cm)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cm->atom);
	const char *path = __ofono_atom_get_path(cm->atom);

	DBG("");

	if (!g_dbus_register_interface(conn, path,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE,
				cdma_connman_methods, cdma_connman_signals,
				NULL, cm, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem,
				OFONO_CDMA_CONNECTION_MANAGER_INTERFACE);

	/* TODO: add watch to support CDMA Network Registration atom */

	__ofono_atom_register(cm->atom, cdma_connman_unregister);
}

void ofono_cdma_connman_remove(struct ofono_cdma_connman *cm)
{
	__ofono_atom_free(cm->atom);
}

void ofono_cdma_connman_set_data(struct ofono_cdma_connman *cm, void *data)
{
	cm->driver_data = data;
}

void *ofono_cdma_connman_get_data(struct ofono_cdma_connman *cm)
{
	return cm->driver_data;
}
