/*
 *
 *  oFono - Open Source Telephony
 *
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

#define	BLUEZ_SERVICE "org.bluez"
#define	BLUEZ_MANAGER_INTERFACE		BLUEZ_SERVICE ".Manager"
#define	BLUEZ_ADAPTER_INTERFACE		BLUEZ_SERVICE ".Adapter"
#define	BLUEZ_DEVICE_INTERFACE		BLUEZ_SERVICE ".Device"

#define DBUS_TIMEOUT 15

#define HFP_AG_UUID	"0000111F-0000-1000-8000-00805F9B34FB"

/* Profiles bitfield */
#define HFP_AG 0x01

struct bluetooth_profile {
	const char *name;
	int (*create)(const char *device, const char *dev_addr,
			const char *adapter_addr, const char *alias);
	void (*remove_all)();
	void (*set_alias)(const char *device, const char *);
};

int bluetooth_register_uuid(const char *uuid,
				struct bluetooth_profile *profile);
void bluetooth_unregister_uuid(const char *uuid);

void bluetooth_create_path(const char *dev_addr, const char *adapter_addr,
							char *buf, int size);

int bluetooth_send_with_reply(const char *path, const char *interface,
				const char *method,
				DBusPendingCallNotifyFunction cb,
				void *user_data, DBusFreeFunction free_func,
				int timeout, int type, ...);
void bluetooth_parse_properties(DBusMessage *reply, const char *property, ...);
