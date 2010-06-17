/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ProFUSION embedded systems
 *  Copyright (C) 2010 Gustavo F. Padovan <gustavo@padovan.org>
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <glib.h>
#include <gdbus.h>
#include <ofono.h>

#include <ofono/dbus.h>

#include "bluetooth.h"

static DBusConnection *connection;
static GHashTable *uuid_hash = NULL;
static GHashTable *adapter_address_hash = NULL;

void bluetooth_create_path(const char *dev_addr, const char *adapter_addr, char *buf, int size)
{
	int i, j;

	for (i = 0, j = 0; adapter_addr[j] && i < size - 1; j++)
		if (adapter_addr[j] >= '0' && adapter_addr[j] <= '9')
			buf[i++] = adapter_addr[j];
		else if (adapter_addr[j] >= 'A' && adapter_addr[j] <= 'F')
			buf[i++] = adapter_addr[j];

	if (i < size - 1)
		buf[i++] = '_';

	for (j = 0; dev_addr[j] && i < size - 1; j++)
		if (dev_addr[j] >= '0' && dev_addr[j] <= '9')
			buf[i++] = dev_addr[j];
		else if (dev_addr[j] >= 'A' && dev_addr[j] <= 'F')
			buf[i++] = dev_addr[j];

	buf[i] = '\0';
}

int bluetooth_send_with_reply(const char *path, const char *interface,
				const char *method,
				DBusPendingCallNotifyFunction cb,
				void *user_data, DBusFreeFunction free_func,
				int timeout, int type, ...)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	va_list args;
	int err;

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, path,
						interface, method);
	if (!msg) {
		ofono_error("Unable to allocate new D-Bus %s message", method);
		err = -ENOMEM;
		goto fail;
	}

	va_start(args, type);

	if (!dbus_message_append_args_valist(msg, type, args)) {
		va_end(args);
		err = -EIO;
		goto fail;
	}

	va_end(args);

	if (timeout > 0)
		timeout *= 1000;

	if (!dbus_connection_send_with_reply(connection, msg, &call, timeout)) {
		ofono_error("Sending %s failed", method);
		err = -EIO;
		goto fail;
	}

	dbus_pending_call_set_notify(call, cb, user_data, free_func);
	dbus_pending_call_unref(call);
	dbus_message_unref(msg);

	return 0;

fail:
	if (free_func && user_data)
		free_func(user_data);

	if (msg)
		dbus_message_unref(msg);

	return err;
}

typedef void (*PropertyHandler)(DBusMessageIter *iter, gpointer user_data);

struct property_handler {
	const char *property;
	PropertyHandler callback;
	gpointer user_data;
};

static gint property_handler_compare(gconstpointer a, gconstpointer b)
{
	const struct property_handler *handler = a;
	const char *property = b;

	return strcmp(handler->property, property);
}

void bluetooth_parse_properties(DBusMessage *reply, const char *property, ...)
{
	va_list args;
	GSList *prop_handlers = NULL;
	DBusMessageIter array, dict;

	va_start(args, property);

	while (property != NULL) {
		struct property_handler *handler =
					g_new0(struct property_handler, 1);

		handler->property = property;
		handler->callback = va_arg(args, PropertyHandler);
		handler->user_data = va_arg(args, gpointer);

		property = va_arg(args, const char *);

		prop_handlers = g_slist_prepend(prop_handlers, handler);
	}

	va_end(args);

	if (dbus_message_iter_init(reply, &array) == FALSE)
		goto done;

	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY)
		goto done;

	dbus_message_iter_recurse(&array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;
		GSList *l;

		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			goto done;

		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			goto done;

		dbus_message_iter_recurse(&entry, &value);

		l = g_slist_find_custom(prop_handlers, key,
					property_handler_compare);

		if (l) {
			struct property_handler *handler = l->data;

			handler->callback(&value, handler->user_data);
		}

		dbus_message_iter_next(&dict);
	}

done:
	g_slist_foreach(prop_handlers, (GFunc)g_free, NULL);
	g_slist_free(prop_handlers);
}

int bluetooth_register_uuid(const char *uuid, struct bluetooth_profile *profile)
{
	if (uuid_hash)
		goto done;

	connection = ofono_dbus_get_connection();

	uuid_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	adapter_address_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, g_free);

done:
	g_hash_table_insert(uuid_hash, g_strdup(uuid), profile);

	return 0;
}

void bluetooth_unregister_uuid(const char *uuid)
{
	g_hash_table_remove(uuid_hash, uuid);

	if (g_hash_table_size(uuid_hash))
		return;

	g_hash_table_destroy(uuid_hash);
	g_hash_table_destroy(adapter_address_hash);
	uuid_hash = NULL;
}

OFONO_PLUGIN_DEFINE(bluetooth, "Bluetooth Utils Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, NULL, NULL)
