/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  BMW Car IT GmbH. All rights reserved.
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/handsfree.h>

#include <gdbus.h>
#include "ofono.h"
#include "common.h"

static GSList *g_drivers = NULL;

struct ofono_handsfree {
	ofono_bool_t inband_ringing;
	ofono_bool_t voice_recognition;
	ofono_bool_t voice_recognition_pending;
	unsigned int ag_features;
	unsigned char battchg;

	const struct ofono_handsfree_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	DBusMessage *pending;
};

static const char **ag_features_list(unsigned int features)
{
	static const char *list[33];
	unsigned int i = 0;

	if (features & HFP_AG_FEATURE_VOICE_RECOG)
		list[i++] = "voice-recognition";

	if (features & HFP_AG_FEATURE_ATTACH_VOICE_TAG)
		list[i++] = "attach-voice-tag";

	list[i] = NULL;

	return list;
}

void ofono_handsfree_set_inband_ringing(struct ofono_handsfree *hf,
						ofono_bool_t enabled)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(hf->atom);
	dbus_bool_t dbus_enabled = enabled;

	if (hf->inband_ringing == enabled)
		return;

	hf->inband_ringing = enabled;

	if (__ofono_atom_get_registered(hf->atom) == FALSE)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_HANDSFREE_INTERFACE,
					"InbandRinging", DBUS_TYPE_BOOLEAN,
					&dbus_enabled);
}

void ofono_handsfree_voice_recognition_notify(struct ofono_handsfree *hf,
						ofono_bool_t enabled)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(hf->atom);
	dbus_bool_t dbus_enabled = enabled;

	if (hf->voice_recognition == enabled)
		return;

	hf->voice_recognition = enabled;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_HANDSFREE_INTERFACE,
					"VoiceRecognition", DBUS_TYPE_BOOLEAN,
					&dbus_enabled);
}

void ofono_handsfree_set_ag_features(struct ofono_handsfree *hf,
					unsigned int ag_features)
{
	if (hf == NULL)
		return;

	hf->ag_features = ag_features;
}

void ofono_handsfree_battchg_notify(struct ofono_handsfree *hf,
					unsigned char level)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(hf->atom);

	if (hf == NULL)
		return;

	if (hf->battchg == level)
		return;

	hf->battchg = level;

	if (__ofono_atom_get_registered(hf->atom) == FALSE)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_HANDSFREE_INTERFACE,
					"BatteryChargeLevel", DBUS_TYPE_BYTE,
					&level);
}

static DBusMessage *handsfree_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_handsfree *hf = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t inband_ringing;
	dbus_bool_t voice_recognition;
	const char **features;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	inband_ringing = hf->inband_ringing;
	ofono_dbus_dict_append(&dict, "InbandRinging", DBUS_TYPE_BOOLEAN,
				&inband_ringing);

	voice_recognition = hf->voice_recognition;
	ofono_dbus_dict_append(&dict, "VoiceRecognition", DBUS_TYPE_BOOLEAN,
				&voice_recognition);

	features = ag_features_list(hf->ag_features);
	ofono_dbus_dict_append_array(&dict, "Features", DBUS_TYPE_STRING,
					&features);

	ofono_dbus_dict_append(&dict, "BatteryChargeLevel", DBUS_TYPE_BYTE,
				&hf->battchg);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void voicerec_set_cb(const struct ofono_error *error, void *data)
{
	struct ofono_handsfree *hf = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(hf->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&hf->pending,
					__ofono_error_failed(hf->pending));
		return;
	}

	hf->voice_recognition = hf->voice_recognition_pending;

	__ofono_dbus_pending_reply(&hf->pending,
				dbus_message_new_method_return(hf->pending));

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_HANDSFREE_INTERFACE,
					"VoiceRecognition",
					DBUS_TYPE_BOOLEAN,
					&hf->voice_recognition);
}

static DBusMessage *handsfree_set_property(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_handsfree *hf = data;
	DBusMessageIter iter, var;
	const char *name;

	if (hf->pending)
		return __ofono_error_busy(msg);

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_str_equal(name, "VoiceRecognition") == TRUE) {
		ofono_bool_t enabled;

		if (!hf->driver->voice_recognition)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &enabled);

		if (hf->voice_recognition == enabled)
			return dbus_message_new_method_return(msg);

		hf->voice_recognition_pending = enabled;
		hf->pending = dbus_message_ref(msg);
		hf->driver->voice_recognition(hf, enabled, voicerec_set_cb, hf);

		return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

static void request_phone_number_cb(const struct ofono_error *error,
					const struct ofono_phone_number *number,
					void *data)
{
	struct ofono_handsfree *hf = data;
	DBusMessage *reply;
	const char *phone_number;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Phone number request callback returned error: %s",
			telephony_error_to_str(error));

		reply = __ofono_error_failed(hf->pending);
		__ofono_dbus_pending_reply(&hf->pending, reply);
		return;
	}

	phone_number = phone_number_to_string(number);
	reply = dbus_message_new_method_return(hf->pending);
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &phone_number,
					DBUS_TYPE_INVALID);
	__ofono_dbus_pending_reply(&hf->pending, reply);
}

static DBusMessage *handsfree_request_phone_number(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_handsfree *hf = data;

	if (hf->pending)
		return __ofono_error_busy(msg);

	if (!hf->driver->request_phone_number)
		return __ofono_error_not_supported(msg);

	hf->pending = dbus_message_ref(msg);
	hf->driver->request_phone_number(hf, request_phone_number_cb, hf);

	return NULL;
}

static const GDBusMethodTable handsfree_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			handsfree_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, handsfree_set_property) },
	{ GDBUS_ASYNC_METHOD("RequestPhoneNumber",
			NULL, GDBUS_ARGS({ "number", "s" }),
			handsfree_request_phone_number) },
	{ }
};

static const GDBusSignalTable handsfree_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void handsfree_remove(struct ofono_atom *atom)
{
	struct ofono_handsfree *hf = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (hf == NULL)
		return;

	if (hf->driver != NULL && hf->driver->remove != NULL)
		hf->driver->remove(hf);

	g_free(hf);
}

struct ofono_handsfree *ofono_handsfree_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_handsfree *hf;
	GSList *l;

	if (driver == NULL)
		return NULL;

	hf = g_try_new0(struct ofono_handsfree, 1);
	if (hf == NULL)
		return NULL;

	hf->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPE_HANDSFREE,
					handsfree_remove, hf);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_handsfree_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(hf, vendor, data) < 0)
			continue;

		hf->driver = drv;
		break;
	}

	return hf;
}

static void handsfree_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);
	struct ofono_handsfree *hf = __ofono_atom_get_data(atom);

	if (hf->pending) {
		DBusMessage *reply = __ofono_error_failed(hf->pending);
		__ofono_dbus_pending_reply(&hf->pending, reply);
	}

	ofono_modem_remove_interface(modem, OFONO_HANDSFREE_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					OFONO_HANDSFREE_INTERFACE);
}

void ofono_handsfree_register(struct ofono_handsfree *hf)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(hf->atom);
	const char *path = __ofono_atom_get_path(hf->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_HANDSFREE_INTERFACE,
					handsfree_methods, handsfree_signals,
					NULL, hf, NULL)) {
		ofono_error("Could not create %s interface",
					OFONO_HANDSFREE_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_HANDSFREE_INTERFACE);

	__ofono_atom_register(hf->atom, handsfree_unregister);
}

int ofono_handsfree_driver_register(const struct ofono_handsfree_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_handsfree_driver_unregister(
				const struct ofono_handsfree_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

void ofono_handsfree_remove(struct ofono_handsfree *hf)
{
	__ofono_atom_free(hf->atom);
}

void ofono_handsfree_set_data(struct ofono_handsfree *hf, void *data)
{
	hf->driver_data = data;
}

void *ofono_handsfree_get_data(struct ofono_handsfree *hf)
{
	return hf->driver_data;
}
