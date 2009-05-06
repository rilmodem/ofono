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
#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "dbus-gsm.h"

#include "modem.h"
#include "driver.h"

#define MANAGER_INTERFACE "org.ofono.Manager"
#define MANAGER_PATH "/"

static GSList *g_modem_list = NULL;
static int g_next_modem_id = 1;

#if 0
struct ofono_modem *manager_find_modem_by_id(int id)
{
	GSList *l;
	struct ofono_modem *modem;

	for (l = g_modem_list; l; l = l->next) {
		modem = l->data;

		if (modem->id == id)
			return modem;
	}

	return NULL;
}
#endif

/* Clients only need to free *modems */
static int modem_list(char ***modems)
{
	GSList *l;
	int i;
	struct ofono_modem *modem;

	*modems = g_new0(char *, g_slist_length(g_modem_list) + 1);

	if (!*modems)
		return -1;

	for (l = g_modem_list, i = 0; l; l = l->next, i++) {
		modem = l->data;

		(*modems)[i] = modem->path;
	}

	return 0;
}

struct ofono_modem *ofono_modem_register(struct ofono_modem_attribute_ops *ops)
{
	struct ofono_modem *modem;
	DBusConnection *conn = dbus_gsm_connection();
	char **modems;

	modem = modem_create(g_next_modem_id, ops);

	if (modem == NULL)
		return 0;

	++g_next_modem_id;

	g_modem_list = g_slist_prepend(g_modem_list, modem);

	if (modem_list(&modems) == 0) {
		dbus_gsm_signal_array_property_changed(conn, MANAGER_PATH,
				MANAGER_INTERFACE, "Modems",
				DBUS_TYPE_OBJECT_PATH, &modems);

		g_free(modems);
	}

	return modem;
}

int ofono_modem_unregister(struct ofono_modem *m)
{
	struct ofono_modem *modem = m;
	DBusConnection *conn = dbus_gsm_connection();
	char **modems;

	if (modem == NULL)
		return -1;

	modem_remove(modem);

	g_modem_list = g_slist_remove(g_modem_list, modem);

	if (modem_list(&modems) == 0) {
		dbus_gsm_signal_array_property_changed(conn, MANAGER_PATH,
				MANAGER_INTERFACE, "Modems",
				DBUS_TYPE_OBJECT_PATH, &modems);

		g_free(modems);
	}

	return 0;
}

static DBusMessage *manager_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessage *reply;
	char **modems;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	if (modem_list(&modems) == -1)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	dbus_gsm_dict_append_array(&dict, "Modems", DBUS_TYPE_OBJECT_PATH,
					&modems);

	g_free(modems);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	manager_get_properties },
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged", "sv" },
	{ }
};

int __ofono_manager_init()
{
	DBusConnection *conn = dbus_gsm_connection();
	gboolean ret;

	ret = g_dbus_register_interface(conn, "/", MANAGER_INTERFACE,
					manager_methods, manager_signals,
					NULL, NULL, NULL);

	if (ret == FALSE)
		return -1;

	return 0;
}

void __ofono_manager_cleanup()
{
	GSList *l;
	struct ofono_modem *modem;
	DBusConnection *conn = dbus_gsm_connection();

	/* Clean up in case plugins didn't unregister the modems */
	for (l = g_modem_list; l; l = l->next) {
		modem = l->data;

		if (!modem)
			continue;

		ofono_debug("plugin owning %s forgot to unregister, cleaning up",
				modem->path);
		modem_remove(modem);
	}

	g_slist_free(g_modem_list);
	g_modem_list = 0;

	g_dbus_unregister_interface(conn, "/", MANAGER_INTERFACE);
}
