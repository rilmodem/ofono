/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010-2011  ProFUSION embedded systems
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

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

#include "bluetooth.h"
#include "util.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define BLUEZ_SERIAL_INTERFACE	BLUEZ_SERVICE ".Serial"

static DBusConnection *connection;
static GHashTable *modem_hash = NULL;
static struct ofono_modem *sap_hw_modem = NULL;
static struct bluetooth_sap_driver *sap_hw_driver = NULL;

struct sap_data {
	struct ofono_modem *hw_modem;
	struct bluetooth_sap_driver *sap_driver;
	DBusPendingCall *call;
};

int bluetooth_sap_client_register(struct bluetooth_sap_driver *sap,
					struct ofono_modem *modem)
{
	if (sap_hw_modem != NULL)
		return -EPERM;

	sap_hw_modem = modem;
	sap_hw_driver = sap;

	bluetooth_get_properties();

	return 0;
}

void bluetooth_sap_client_unregister(struct ofono_modem *modem)
{
	GHashTableIter iter;
	gpointer key, value;

	if (sap_hw_modem == NULL)
		return;

	g_hash_table_iter_init(&iter, modem_hash);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_iter_remove(&iter);

		ofono_modem_remove(value);
	}

	sap_hw_modem = NULL;
	sap_hw_driver = NULL;
}

static int sap_probe(struct ofono_modem *modem)
{
	struct sap_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct sap_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void sap_remove(struct ofono_modem *modem)
{
	struct sap_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->call != NULL)
		dbus_pending_call_cancel(data->call);

	g_free(data);

	ofono_modem_set_data(modem, NULL);
}

static void sap_connect_reply(DBusPendingCall *call, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sap_data *data = ofono_modem_get_data(modem);
	DBusError derr;
	DBusMessage *reply;
	int fd, err;

	DBG("");

	reply = dbus_pending_call_steal_reply(call);

	data->call = NULL;

	if (ofono_modem_get_powered(modem))
		goto done;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {

		DBG("Connect reply: %s", derr.message);

		dbus_error_free(&derr);
		goto done;
	}

	if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd,
				DBUS_TYPE_INVALID))
		goto done;

	data->hw_modem = sap_hw_modem;
	data->sap_driver = sap_hw_driver;

	err = data->sap_driver->enable(data->hw_modem, modem, fd);
	if (!err || err == -EINPROGRESS) {
		dbus_message_unref(reply);
		return;
	}

done:
	ofono_modem_set_powered(modem, FALSE);
	dbus_message_unref(reply);
}

/* power up hardware */
static int sap_enable(struct ofono_modem *modem)
{
	struct sap_data *data = ofono_modem_get_data(modem);
	DBusPendingCall *call;
	int status;
	const char *str = "sap";
	const char *server_path = ofono_modem_get_string(modem, "ServerPath");

	DBG("%p", modem);

	status = bluetooth_send_with_reply(server_path, BLUEZ_SERIAL_INTERFACE,
					"ConnectFD", &call, sap_connect_reply,
					modem, NULL, DBUS_TIMEOUT,
					DBUS_TYPE_STRING, &str,
					DBUS_TYPE_INVALID);

	if (status < 0)
		return -EINVAL;

	data->call = call;

	return -EINPROGRESS;
}

static int sap_disable(struct ofono_modem *modem)
{
	struct sap_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	return data->sap_driver->disable(data->hw_modem);
}

static void sap_pre_sim(struct ofono_modem *modem)
{
	struct sap_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->sap_driver->pre_sim(data->hw_modem);
}

static void sap_post_sim(struct ofono_modem *modem)
{
	struct sap_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->sap_driver->post_sim(data->hw_modem);
}

static void sap_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct sap_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->sap_driver->set_online(data->hw_modem, online, cb, user_data);
}

static void sap_post_online(struct ofono_modem *modem)
{
	struct sap_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->sap_driver->post_online(data->hw_modem);
}

static int bluetooth_sap_probe(const char *device, const char *dev_addr,
				const char *adapter_addr, const char *alias)
{
	struct ofono_modem *modem;
	char buf[256];

	if (sap_hw_modem == NULL)
		return -ENODEV;

	/* We already have this device in our hash, ignore */
	if (g_hash_table_lookup(modem_hash, device) != NULL)
		return -EALREADY;

	ofono_info("Using device: %s, devaddr: %s, adapter: %s",
			device, dev_addr, adapter_addr);

	strcpy(buf, "sap/");
	bluetooth_create_path(dev_addr, adapter_addr, buf + 4,
						sizeof(buf) - 4);

	modem = ofono_modem_create(buf, "sap");
	if (modem == NULL)
		return -ENOMEM;

	ofono_modem_set_string(modem, "ServerPath", device);
	ofono_modem_set_name(modem, alias);
	ofono_modem_register(modem);

	g_hash_table_insert(modem_hash, g_strdup(device), modem);

	return 0;
}

static void bluetooth_sap_remove(const char *prefix)
{
	GHashTableIter iter;
	gpointer key, value;

	DBG("%s", prefix);

	if (modem_hash == NULL)
		return;

	g_hash_table_iter_init(&iter, modem_hash);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (prefix && g_str_has_prefix((char *)key, prefix) == FALSE)
			continue;

		g_hash_table_iter_remove(&iter);

		ofono_modem_remove(value);
	}
}

static void bluetooth_sap_set_alias(const char *device, const char *alias)
{
	struct ofono_modem *modem;

	if (device == NULL || alias == NULL)
		return;

	modem =	g_hash_table_lookup(modem_hash, device);
	if (modem == NULL)
		return;

	ofono_modem_set_name(modem, alias);
}

static struct ofono_modem_driver sap_driver = {
	.name		= "sap",
	.modem_type	= OFONO_MODEM_TYPE_SAP,
	.probe		= sap_probe,
	.remove		= sap_remove,
	.enable		= sap_enable,
	.disable	= sap_disable,
	.pre_sim	= sap_pre_sim,
	.post_sim	= sap_post_sim,
	.set_online	= sap_set_online,
	.post_online	= sap_post_online,
};

static struct bluetooth_profile sap = {
	.name		= "sap",
	.probe		= bluetooth_sap_probe,
	.remove		= bluetooth_sap_remove,
	.set_alias	= bluetooth_sap_set_alias,
};

static int sap_init(void)
{
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	connection = ofono_dbus_get_connection();

	err = ofono_modem_driver_register(&sap_driver);
	if (err < 0)
		return err;

	err = bluetooth_register_uuid(SAP_UUID, &sap);
	if (err < 0) {
		ofono_modem_driver_unregister(&sap_driver);
		return err;
	}

	modem_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	return 0;
}

static void sap_exit(void)
{
	DBG("");

	bluetooth_unregister_uuid(SAP_UUID);
	ofono_modem_driver_unregister(&sap_driver);
	g_hash_table_destroy(modem_hash);
	modem_hash = NULL;
}

OFONO_PLUGIN_DEFINE(sap, "Sim Access Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, sap_init, sap_exit)
