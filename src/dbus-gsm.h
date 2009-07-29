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

#include <ofono/dbus.h>
#include <gdbus.h>

#define MAX_DBUS_PATH_LEN 64

/* Essentially a{sv} */
#define PROPERTIES_ARRAY_SIGNATURE DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING \
					DBUS_TYPE_STRING_AS_STRING \
					DBUS_TYPE_VARIANT_AS_STRING \
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING

void dbus_gsm_dict_append(DBusMessageIter *dict, const char *key, int type,
				void *value);

void dbus_gsm_append_variant(DBusMessageIter *iter, int type, void *value);

void dbus_gsm_append_array_variant(DBusMessageIter *iter, int type, void *val);

void dbus_gsm_dict_append_array(DBusMessageIter *dict, const char *key,
				int type, void *val);


static inline void dbus_gsm_pending_reply(DBusMessage **msg, DBusMessage *reply)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	g_dbus_send_message(conn, reply);

	dbus_message_unref(*msg);
	*msg = NULL;
}
