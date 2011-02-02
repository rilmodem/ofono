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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-volume.h>

#include <gdbus.h>
#include "ofono.h"
#include "common.h"

static GSList *g_drivers = NULL;

struct ofono_call_volume {
	DBusMessage *pending;
	unsigned char speaker_volume;
	unsigned char microphone_volume;
	unsigned char pending_volume;
	gboolean muted;
	gboolean muted_pending;
	const struct ofono_call_volume_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

void ofono_call_volume_set_speaker_volume(struct ofono_call_volume *cv,
						unsigned char percent)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cv->atom);

	cv->speaker_volume = percent;

	if (__ofono_atom_get_registered(cv->atom) == FALSE)
		return;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_VOLUME_INTERFACE,
						"SpeakerVolume",
						DBUS_TYPE_BYTE, &percent);
}

void ofono_call_volume_set_microphone_volume(struct ofono_call_volume *cv,
						unsigned char percent)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cv->atom);

	cv->microphone_volume = percent;

	if (__ofono_atom_get_registered(cv->atom) == FALSE)
		return;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_VOLUME_INTERFACE,
						"MicrophoneVolume",
						DBUS_TYPE_BYTE, &percent);
}

void ofono_call_volume_set_muted(struct ofono_call_volume *cv, int muted)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cv->atom);
	dbus_bool_t m;

	cv->muted = muted;

	if (__ofono_atom_get_registered(cv->atom) == FALSE)
		return;

	m = muted;
	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_VOLUME_INTERFACE,
						"Muted", DBUS_TYPE_BOOLEAN, &m);
}

static DBusMessage *cv_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_call_volume *cv = data;
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	dbus_bool_t muted;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "SpeakerVolume", DBUS_TYPE_BYTE,
					&cv->speaker_volume);

	ofono_dbus_dict_append(&dict, "MicrophoneVolume",
					DBUS_TYPE_BYTE, &cv->microphone_volume);

	muted = cv->muted;
	ofono_dbus_dict_append(&dict, "Muted", DBUS_TYPE_BOOLEAN, &muted);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void sv_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_volume *cv = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cv->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&cv->pending,
					__ofono_error_failed(cv->pending));
		return;
	}

	cv->speaker_volume = cv->pending_volume;

	__ofono_dbus_pending_reply(&cv->pending,
				dbus_message_new_method_return(cv->pending));

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CALL_VOLUME_INTERFACE,
					"SpeakerVolume",
					DBUS_TYPE_BYTE, &cv->speaker_volume);
}

static void mv_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_volume *cv = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cv->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&cv->pending,
					__ofono_error_failed(cv->pending));
		return;
	}

	cv->microphone_volume = cv->pending_volume;

	__ofono_dbus_pending_reply(&cv->pending,
				dbus_message_new_method_return(cv->pending));

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CALL_VOLUME_INTERFACE,
					"MicrophoneVolume",
					DBUS_TYPE_BYTE, &cv->microphone_volume);
}

static void muted_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_volume *cv = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cv->atom);
	dbus_bool_t m;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		cv->muted_pending = cv->muted;
		__ofono_dbus_pending_reply(&cv->pending,
					__ofono_error_failed(cv->pending));
		return;
	}

	cv->muted = cv->muted_pending;
	m = cv->muted;

	__ofono_dbus_pending_reply(&cv->pending,
				dbus_message_new_method_return(cv->pending));

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_VOLUME_INTERFACE,
						"Muted", DBUS_TYPE_BOOLEAN, &m);
}

static DBusMessage *cv_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_volume *cv = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (cv->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_str_equal(property, "SpeakerVolume") == TRUE) {
		unsigned char percent;

		if (cv->driver->speaker_volume == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BYTE)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &percent);

		if (percent > 100)
			return __ofono_error_invalid_format(msg);

		if (percent == cv->speaker_volume)
			return dbus_message_new_method_return(msg);

		cv->pending_volume = percent;
		cv->pending = dbus_message_ref(msg);
		cv->driver->speaker_volume(cv, percent, sv_set_callback, cv);

		return NULL;
	} else if (g_str_equal(property, "MicrophoneVolume") == TRUE) {
		unsigned char percent;

		if (cv->driver->microphone_volume == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BYTE)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &percent);

		if (percent > 100)
			return __ofono_error_invalid_format(msg);

		if (percent == cv->microphone_volume)
			return dbus_message_new_method_return(msg);

		cv->pending_volume = percent;
		cv->pending = dbus_message_ref(msg);
		cv->driver->microphone_volume(cv, percent, mv_set_callback, cv);

		return NULL;
	} else if (g_str_equal(property, "Muted") == TRUE) {
		dbus_bool_t muted;

		if (cv->driver->mute == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &muted);

		if (muted == (dbus_bool_t) cv->muted)
			return dbus_message_new_method_return(msg);

		cv->muted_pending = muted;
		cv->pending = dbus_message_ref(msg);
		cv->driver->mute(cv, muted, muted_set_callback, cv);

		return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

static GDBusMethodTable cv_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cv_get_properties },
	{ "SetProperty",	"sv",	"",		cv_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cv_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static void call_volume_remove(struct ofono_atom *atom)
{
	struct ofono_call_volume *cv = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cv == NULL)
		return;

	if (cv->driver != NULL && cv->driver->remove != NULL)
		cv->driver->remove(cv);

	g_free(cv);
}

struct ofono_call_volume *ofono_call_volume_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_call_volume *cv;
	GSList *l;

	if (driver == NULL)
		return NULL;

	cv = g_try_new0(struct ofono_call_volume, 1);
	if (cv == NULL)
		return NULL;

	cv->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPES_CALL_VOLUME,
					call_volume_remove, cv);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_call_volume_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cv, vendor, data) < 0)
			continue;

		cv->driver = drv;
		break;
	}

	return cv;
}

static void call_volume_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	ofono_modem_remove_interface(modem, OFONO_CALL_VOLUME_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					OFONO_CALL_VOLUME_INTERFACE);
}

void ofono_call_volume_register(struct ofono_call_volume *cv)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cv->atom);
	const char *path = __ofono_atom_get_path(cv->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CALL_VOLUME_INTERFACE,
					cv_methods, cv_signals, NULL,
					cv, NULL)) {
		ofono_error("Could not create %s interface",
					OFONO_CALL_VOLUME_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_CALL_VOLUME_INTERFACE);

	__ofono_atom_register(cv->atom, call_volume_unregister);
}

int ofono_call_volume_driver_register(const struct ofono_call_volume_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_call_volume_driver_unregister(
				const struct ofono_call_volume_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

void ofono_call_volume_remove(struct ofono_call_volume *cv)
{
	__ofono_atom_free(cv->atom);
}

void ofono_call_volume_set_data(struct ofono_call_volume *cv, void *data)
{
	cv->driver_data = data;
}

void *ofono_call_volume_get_data(struct ofono_call_volume *cv)
{
	return cv->driver_data;
}
