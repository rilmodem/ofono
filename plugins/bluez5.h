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

#define HFP_HS_UUID	"0000111e-0000-1000-8000-00805f9b34fb"
#define HFP_AG_UUID	"0000111f-0000-1000-8000-00805f9b34fb"

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH		31
#define PF_BLUETOOTH		AF_BLUETOOTH
#endif

#define BTPROTO_SCO		2

#define SOL_SCO			17

#ifndef SOL_BLUETOOTH
#define SOL_BLUETOOTH		274
#endif

#define BT_DEFER_SETUP		7

/* BD Address */
typedef struct {
	uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

#define BDADDR_ANY   (&(bdaddr_t) {{0, 0, 0, 0, 0, 0}})

/* RFCOMM socket address */
struct sockaddr_rc {
	sa_family_t	rc_family;
	bdaddr_t	rc_bdaddr;
	uint8_t		rc_channel;
};

/* SCO socket address */
struct sockaddr_sco {
	sa_family_t	sco_family;
	bdaddr_t	sco_bdaddr;
};

void bt_bacpy(bdaddr_t *dst, const bdaddr_t *src);

int bt_ba2str(const bdaddr_t *ba, char *str);

int bt_bacmp(const bdaddr_t *ba1, const bdaddr_t *ba2);

int bt_register_profile(DBusConnection *conn, const char *uuid,
					const char *name, const char *object);

void bt_unregister_profile(DBusConnection *conn, const char *object);
