/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

static GDBusMethodTable manager_methods[] = {
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "ModemAdded",        "oa{sv}" },
	{ }
};

int __ofono_manager_init()
{
	DBusConnection *conn = ofono_dbus_get_connection();
	gboolean ret;

	ret = g_dbus_register_interface(conn, OFONO_MANAGER_PATH,
					OFONO_MANAGER_INTERFACE,
					manager_methods, manager_signals,
					NULL, NULL, NULL);

	if (ret == FALSE)
		return -1;

	return 0;
}

void __ofono_manager_cleanup()
{
	DBusConnection *conn = ofono_dbus_get_connection();

	g_dbus_unregister_interface(conn, OFONO_MANAGER_PATH,
					OFONO_MANAGER_INTERFACE);
}
