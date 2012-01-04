/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include "plugins/bluetooth.h"

#include "dundee.h"

static GHashTable *bluetooth_hash;

struct bluetooth_device {
	struct dundee_device *device;

	char *path;
	char *address;
	char *name;

	DBusPendingCall *call;
};

static void bt_disconnect(struct dundee_device *device,
				dundee_device_disconnect_cb_t cb, void *data)
{
	struct bluetooth_device *bt = dundee_device_get_data(device);

	DBG("%p", bt);

	CALLBACK_WITH_SUCCESS(cb, data);
}

static void bt_connect_reply(DBusPendingCall *call, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	dundee_device_connect_cb_t cb = cbd->cb;
	struct bluetooth_device *bt = cbd->user;
	DBusMessage *reply;
	DBusError derr;
	int fd;

	DBG("%p", bt);

	reply = dbus_pending_call_steal_reply(call);

	bt->call = NULL;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		DBG("Connection to bt serial returned with error: %s, %s",
						derr.name, derr.message);

		dbus_error_free(&derr);

		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		goto done;
	}

	dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd,
			DBUS_TYPE_INVALID);

	DBG("%p fd %d", bt, fd);

	if (fd < 0) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, fd, cbd->data);

done:
	dbus_message_unref(reply);
	g_free(cbd);
}

static void bt_connect(struct dundee_device *device,
			dundee_device_connect_cb_t cb, void *data)
{
	struct bluetooth_device *bt = dundee_device_get_data(device);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *profile = "dun";
	int status;

	DBG("%p", bt);

	cbd->user = bt;

	status = bluetooth_send_with_reply(bt->path,
					BLUEZ_SERIAL_INTERFACE, "ConnectFD",
					&bt->call, bt_connect_reply,
					cbd, NULL, DBUS_TIMEOUT,
					DBUS_TYPE_STRING, &profile,
					DBUS_TYPE_INVALID);
	if (status == 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

struct dundee_device_driver bluetooth_driver = {
	.name = "bluetooth",
	.connect = bt_connect,
	.disconnect = bt_disconnect,
};

static int bt_probe(const char *path, const char *dev_addr,
				const char *adapter_addr, const char *alias)
{
	struct bluetooth_device *bt;
	struct dundee_device *device;
	char buf[256];

	DBG("");

	/* We already have this device in our hash, ignore */
	if (g_hash_table_lookup(bluetooth_hash, path) != NULL)
		return -EALREADY;

	ofono_info("Using device: %s, devaddr: %s, adapter: %s",
			path, dev_addr, adapter_addr);

	strcpy(buf, "dun/");
	bluetooth_create_path(dev_addr, adapter_addr, buf + 4, sizeof(buf) - 4);

	bt = g_try_new0(struct bluetooth_device, 1);
	if (bt == NULL)
		return -ENOMEM;

	DBG("%p", bt);

	device = dundee_device_create(&bluetooth_driver);
	if (device == NULL)
		goto free;

	dundee_device_set_data(device, bt);

	bt->path = g_strdup(path);
	if (bt->path == NULL)
		goto free;

	bt->address = g_strdup(dev_addr);
	if (bt->address == NULL)
		goto free;

	bt->name = g_strdup(alias);
	if (bt->name == NULL)
		goto free;

	dundee_device_set_name(device, bt->name);

	if (dundee_device_register(device) < 0) {
		g_free(device);
		goto free;
	}

	bt->device = device;
	g_hash_table_insert(bluetooth_hash, g_strdup(path), bt);

	return 0;

free:
	g_free(bt->path);
	g_free(bt->address);
	g_free(bt->name);
	g_free(bt);

	return -ENOMEM;
}

static void destroy_device(gpointer user)
{
	struct bluetooth_device *bt = user;

	DBG("%p", bt);

	if (bt->call != NULL)
		dbus_pending_call_cancel(bt->call);

	g_free(bt->path);
	g_free(bt->address);

	g_free(bt);
}

static gboolean bt_remove_device(gpointer key, gpointer value,
					gpointer user_data)
{
	struct bluetooth_device *bt = value;
	const char *path = key;
	const char *prefix = user_data;

	DBG("%p", bt);

	if (prefix && g_str_has_prefix(path, prefix) == FALSE)
		return FALSE;

	dundee_device_unregister(bt->device);

	return TRUE;
}

static void bt_remove(const char *prefix)
{
	DBG("%s", prefix);

	if (bluetooth_hash == NULL)
		return;

	g_hash_table_foreach_remove(bluetooth_hash, bt_remove_device,
							(gpointer) prefix);
}

static void bt_set_alias(const char *path, const char *alias)
{
	struct bluetooth_device *bt;

	DBG("");

	if (path == NULL || alias == NULL)
		return;

	bt = g_hash_table_lookup(bluetooth_hash, path);
	if (bt == NULL)
		return;

	g_free(bt->name);
	bt->name = g_strdup(alias);

	dundee_device_set_name(bt->device, bt->name);
}

static struct bluetooth_profile dun_profile = {
	.name		= "dun_dt",
	.probe		= bt_probe,
	.remove		= bt_remove,
	.set_alias	= bt_set_alias,
};

int __dundee_bluetooth_init(void)
{
	int err;

	DBG("");

	err = bluetooth_register_uuid(DUN_GW_UUID, &dun_profile);
	if (err < 0)
		return err;

	bluetooth_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, destroy_device);

	return 0;
}

void  __dundee_bluetooth_cleanup(void)
{
	DBG("");

	bluetooth_unregister_uuid(DUN_GW_UUID);
	g_hash_table_destroy(bluetooth_hash);
}
