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
