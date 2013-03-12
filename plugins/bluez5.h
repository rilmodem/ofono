/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
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

#define BLUEZ_SERVICE			"org.bluez"
#define BLUEZ_MANAGER_PATH		"/"
#define BLUEZ_PROFILE_INTERFACE		BLUEZ_SERVICE ".Profile1"
#define BLUEZ_DEVICE_INTERFACE		BLUEZ_SERVICE ".Device1"
#define BLUEZ_ERROR_INTERFACE		BLUEZ_SERVICE ".Error"

#define DUN_GW_UUID	"00001103-0000-1000-8000-00805f9b34fb"
#define HFP_HS_UUID	"0000111e-0000-1000-8000-00805f9b34fb"
#define HFP_AG_UUID	"0000111f-0000-1000-8000-00805f9b34fb"

int bt_register_profile_with_role(DBusConnection *conn, const char *uuid,
					uint16_t version, const char *name,
					const char *object, const char *role);

int bt_register_profile(DBusConnection *conn, const char *uuid,
					uint16_t version, const char *name,
					const char *object);

void bt_unregister_profile(DBusConnection *conn, const char *object);

typedef void (*bt_finish_cb)(gboolean success, gpointer user_data);

void bt_connect_profile(DBusConnection *conn,
				const char *device, const char *uuid,
				bt_finish_cb cb, gpointer user_data);

void bt_disconnect_profile(DBusConnection *conn,
				const char *device, const char *uuid,
				bt_finish_cb cb, gpointer user_data);
