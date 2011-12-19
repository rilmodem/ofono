/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012  BMW Car IT GmbH. All rights reserved.
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

#include <glib.h>
#include <gdbus.h>

#include "dundee.h"

#define DUNDEE_ERROR_INTERFACE "org.ofono.dundee.Error"

DBusMessage *__dundee_error_invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg, DUNDEE_ERROR_INTERFACE
					".InvalidArguments",
					"Invalid arguments in method call");
}

DBusMessage *__dundee_error_failed(DBusMessage *msg)
{
	return g_dbus_create_error(msg, DUNDEE_ERROR_INTERFACE
					".Failed",
					"Operation failed");
}
