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
#include <stdio.h>

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "dbus-gsm.h"
#include "modem.h"
#include "driver.h"
#include "common.h"
#include "util.h"
#include "sim.h"

#define SMS_MANAGER_INTERFACE "org.ofono.SmsManager"

#define SMS_MANAGER_FLAG_CACHED 0x1

struct sms_manager_data {
	struct ofono_sms_ops *ops;
	int flags;
	DBusMessage *pending;
	struct ofono_phone_number sca;
};

static struct sms_manager_data *sms_manager_create()
{
	struct sms_manager_data *sms;

	sms = g_new0(struct sms_manager_data, 1);

	sms->sca.type = 129;

	return sms;
}

static void sms_manager_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct sms_manager_data *data = modem->sms_manager;

	g_free(data);
}

static void set_sca(struct ofono_modem *modem,
			const struct ofono_phone_number *sca)
{
	struct sms_manager_data *sms = modem->sms_manager;
	DBusConnection *conn = dbus_gsm_connection();
	const char *value;

	if (sms->sca.type == sca->type &&
			!strcmp(sms->sca.number, sca->number))
		return;

	sms->sca.type = sca->type;
	strncpy(sms->sca.number, sca->number, OFONO_MAX_PHONE_NUMBER_LENGTH);
	sms->sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';

	value = phone_number_to_string(&sms->sca);

	dbus_gsm_signal_property_changed(conn, modem->path,
						SMS_MANAGER_INTERFACE,
						"ServiceCenterAddress",
						DBUS_TYPE_STRING, &value);
}

static DBusMessage *generate_get_properties_reply(struct ofono_modem *modem,
							DBusMessage *msg)
{
	struct sms_manager_data *sms = modem->sms_manager;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *sca;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	sca = phone_number_to_string(&sms->sca);

	dbus_gsm_dict_append(&dict, "ServiceCenterAddress", DBUS_TYPE_STRING,
				&sca);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void sms_sca_query_cb(const struct ofono_error *error,
				const struct ofono_phone_number *sca, void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto out;

	set_sca(modem, sca);

	sms->flags |= SMS_MANAGER_FLAG_CACHED;

out:
	if (sms->pending) {
		DBusMessage *reply = generate_get_properties_reply(modem,
								sms->pending);
		dbus_gsm_pending_reply(&sms->pending, reply);
	}
}

static DBusMessage *sms_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;

	if (sms->pending)
		return dbus_gsm_busy(msg);

	if (!sms->ops->sca_query)
		return dbus_gsm_not_implemented(msg);

	if (sms->flags & SMS_MANAGER_FLAG_CACHED)
		return generate_get_properties_reply(modem, msg);

	sms->pending = dbus_message_ref(msg);

	sms->ops->sca_query(modem, sms_sca_query_cb, modem);

	return NULL;
}

static void sca_set_query_callback(const struct ofono_error *error,
					const struct ofono_phone_number *sca,
					void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Set SCA succeeded, but query failed");
		sms->flags &= ~SMS_MANAGER_FLAG_CACHED;
		reply = dbus_gsm_failed(sms->pending);
		dbus_gsm_pending_reply(&sms->pending, reply);
		return;
	}

	set_sca(modem, sca);

	reply = dbus_message_new_method_return(sms->pending);
	dbus_gsm_pending_reply(&sms->pending, reply);
}

static void sca_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Setting SCA failed");
		dbus_gsm_pending_reply(&sms->pending,
					dbus_gsm_failed(sms->pending));
		return;
	}

	sms->ops->sca_query(modem, sca_set_query_callback, modem);
}

static DBusMessage *sms_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (sms->pending)
		return dbus_gsm_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return dbus_gsm_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (!strcmp(property, "ServiceCenterAddress")) {
		const char *value;
		struct ofono_phone_number sca;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return dbus_gsm_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (strlen(value) == 0 || !valid_phone_number_format(value))
			return dbus_gsm_invalid_format(msg);

		if (!sms->ops->sca_set)
			return dbus_gsm_not_implemented(msg);

		string_to_phone_number(value, &sca);

		sms->pending = dbus_message_ref(msg);

		sms->ops->sca_set(modem, &sca, sca_set_callback, modem);
		return NULL;
	}

	return dbus_gsm_invalid_args(msg);
}

static GDBusMethodTable sms_manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	sms_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SetProperty",	"sv",	"",		sms_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable sms_manager_signals[] = {
	{ "PropertyChanged",	"sv"	},
	{ }
};

int ofono_sms_manager_register(struct ofono_modem *modem,
					struct ofono_sms_ops *ops)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->sms_manager = sms_manager_create();

	if (!modem->sms_manager)
		return -1;

	modem->sms_manager->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					SMS_MANAGER_INTERFACE,
					sms_manager_methods,
					sms_manager_signals,
					NULL, modem,
					sms_manager_destroy)) {
		ofono_error("Could not register SmsManager interface");
		sms_manager_destroy(modem);

		return -1;
	}

	ofono_debug("SmsManager interface for modem: %s created",
			modem->path);

	modem_add_interface(modem, SMS_MANAGER_INTERFACE);

	return 0;
}

void ofono_sms_manager_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();

	g_dbus_unregister_interface(conn, modem->path,
					SMS_MANAGER_INTERFACE);

	modem_remove_interface(modem, SMS_MANAGER_INTERFACE);
}
