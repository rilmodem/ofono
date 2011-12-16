/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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
#include <time.h>
#include <errno.h>
#include <stdint.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"

static GSList *g_drivers;

struct ofono_cdma_voicecall {
	struct ofono_cdma_phone_number phone_number;
	struct ofono_cdma_phone_number waiting_number;
	int direction;
	enum cdma_call_status status;
	time_t start_time;
	DBusMessage *pending;
	const struct ofono_cdma_voicecall_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static const char *disconnect_reason_to_string(enum ofono_disconnect_reason r)
{
	switch (r) {
	case OFONO_DISCONNECT_REASON_LOCAL_HANGUP:
		return "local";
	case OFONO_DISCONNECT_REASON_REMOTE_HANGUP:
		return "remote";
	default:
		return "network";
	}
}

static const char *cdma_call_status_to_string(enum cdma_call_status status)
{
	switch (status) {
	case CDMA_CALL_STATUS_ACTIVE:
		return "active";
	case CDMA_CALL_STATUS_DIALING:
		return "dialing";
	case CDMA_CALL_STATUS_ALERTING:
		return "alerting";
	case CDMA_CALL_STATUS_INCOMING:
		return "incoming";
	case CDMA_CALL_STATUS_DISCONNECTED:
		return "disconnected";
	}

	return NULL;
}

static const char *time_to_str(const time_t *t)
{
	static char buf[128];
	struct tm tm;

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime_r(t, &tm));
	buf[127] = '\0';

	return buf;
}

static void generic_callback(const struct ofono_error *error, void *data)
{
	struct ofono_cdma_voicecall *vc = data;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(vc->pending);
	else
		reply = __ofono_error_failed(vc->pending);

	__ofono_dbus_pending_reply(&vc->pending, reply);
}

static void append_voicecall_properties(struct ofono_cdma_voicecall *vc,
					DBusMessageIter *dict)
{
	const char *status;
	const char *lineid;
	const char *waiting_call;
	dbus_bool_t call_waiting = FALSE;

	status = cdma_call_status_to_string(vc->status);
	ofono_dbus_dict_append(dict, "State", DBUS_TYPE_STRING, &status);

	lineid = cdma_phone_number_to_string(&vc->phone_number);
	ofono_dbus_dict_append(dict, "LineIdentification",
					DBUS_TYPE_STRING, &lineid);

	if (vc->waiting_number.number[0] != '\0') {
		waiting_call = cdma_phone_number_to_string(&vc->waiting_number);
		ofono_dbus_dict_append(dict, "CallWaitingNumber",
					DBUS_TYPE_STRING, &waiting_call);
		call_waiting = TRUE;
	}

	ofono_dbus_dict_append(dict, "CallWaiting",
					DBUS_TYPE_BOOLEAN, &call_waiting);

	if (vc->status == CDMA_CALL_STATUS_ACTIVE) {
		const char *timestr = time_to_str(&vc->start_time);

		ofono_dbus_dict_append(dict, "StartTime", DBUS_TYPE_STRING,
					&timestr);
	}
}

static DBusMessage *voicecall_manager_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_cdma_voicecall *vc = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);

	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_voicecall_properties(vc, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void voicecall_emit_disconnect_reason(struct ofono_cdma_voicecall *vc,
					enum ofono_disconnect_reason reason)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(vc->atom);
	const char *reason_str;

	reason_str = disconnect_reason_to_string(reason);

	g_dbus_emit_signal(conn, path, OFONO_CDMA_VOICECALL_MANAGER_INTERFACE,
				"DisconnectReason",
				DBUS_TYPE_STRING, &reason_str,
				DBUS_TYPE_INVALID);
}

static void voicecall_set_call_status(struct ofono_cdma_voicecall *vc,
						enum cdma_call_status status)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(vc->atom);
	const char *status_str;
	enum cdma_call_status old_status;

	DBG("status: %s", cdma_call_status_to_string(status));

	if (vc->status == status)
		return;

	old_status = vc->status;

	vc->status = status;

	status_str = cdma_call_status_to_string(status);

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CDMA_VOICECALL_MANAGER_INTERFACE,
					"State", DBUS_TYPE_STRING,
					&status_str);

	if (status == CDMA_CALL_STATUS_ACTIVE &&
			old_status == CDMA_CALL_STATUS_DIALING) {
		const char *timestr;

		vc->start_time = time(NULL);
		timestr = time_to_str(&vc->start_time);

		ofono_dbus_signal_property_changed(conn, path,
					OFONO_CDMA_VOICECALL_MANAGER_INTERFACE,
					"StartTime", DBUS_TYPE_STRING,
					&timestr);
	}

	/* TODO: Properly signal property changes here */
	if (status == CDMA_CALL_STATUS_DISCONNECTED) {
		memset(&vc->phone_number, 0,
				sizeof(struct ofono_cdma_phone_number));

		memset(&vc->waiting_number, 0,
			sizeof(struct ofono_cdma_phone_number));
	}
}

static void voicecall_set_call_lineid(struct ofono_cdma_voicecall *vc)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path  = __ofono_atom_get_path(vc->atom);
	const char *lineid_str;

	/* For MO calls, LineID is the dialed phone number */
	lineid_str = cdma_phone_number_to_string(&vc->phone_number);

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CDMA_VOICECALL_MANAGER_INTERFACE,
					"LineIdentification",
					DBUS_TYPE_STRING, &lineid_str);
}

static void manager_dial_callback(const struct ofono_error *error, void *data)
{
	struct ofono_cdma_voicecall *vc = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		reply = __ofono_error_failed(vc->pending);
		__ofono_dbus_pending_reply(&vc->pending, reply);

		return;
	}

	voicecall_set_call_lineid(vc);
	vc->direction = CALL_DIRECTION_MOBILE_ORIGINATED;
	voicecall_set_call_status(vc, CDMA_CALL_STATUS_DIALING);

	reply = dbus_message_new_method_return(vc->pending);
	__ofono_dbus_pending_reply(&vc->pending, reply);
}

static DBusMessage *voicecall_manager_dial(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_cdma_voicecall *vc = data;
	const char *number;

	if (vc->pending)
		return __ofono_error_busy(msg);

	if (vc->status != CDMA_CALL_STATUS_DISCONNECTED)
		return __ofono_error_failed(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &number,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!valid_cdma_phone_number_format(number))
		return __ofono_error_invalid_format(msg);

	if (vc->driver->dial == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	string_to_cdma_phone_number(number, &vc->phone_number);
	vc->driver->dial(vc, &vc->phone_number, manager_dial_callback, vc);

	return NULL;
}

static DBusMessage *voicecall_manager_hangup(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_cdma_voicecall *vc = data;

	if (vc->pending)
		return __ofono_error_busy(msg);

	if (vc->driver->hangup == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->status == CDMA_CALL_STATUS_DISCONNECTED)
		return __ofono_error_failed(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->hangup(vc, generic_callback, vc);

	return NULL;
}

static DBusMessage *voicecall_manager_answer(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_cdma_voicecall *vc = data;

	if (vc->pending)
		return __ofono_error_busy(msg);

	if (vc->driver->answer == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->status != CDMA_CALL_STATUS_INCOMING)
		return __ofono_error_failed(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->answer(vc, generic_callback, vc);

	return NULL;
}

static DBusMessage *voicecall_manager_flash(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_cdma_voicecall *vc = data;
	const char *string;

	if (vc->pending)
		return __ofono_error_busy(msg);

	if (vc->driver->send_flash == NULL)
		return __ofono_error_not_implemented(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &string,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->send_flash(vc, string, generic_callback, vc);

	return NULL;
}

static ofono_bool_t is_valid_tones(const char *tones)
{
	int len;
	int i;

	if (tones == NULL)
		return FALSE;

	len = strlen(tones);
	if (len == 0)
		return FALSE;

	for (i = 0; i < len; i++) {
		if (g_ascii_isdigit(tones[i]) || tones[i] == '*' ||
				tones[i] == '#')
			continue;
		else
			return FALSE;
	}

	return TRUE;
}

static DBusMessage *voicecall_manager_tone(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_cdma_voicecall *vc = data;
	const char *tones;

	if (vc->pending)
		return __ofono_error_busy(msg);

	if (vc->driver->send_tones == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->status != CDMA_CALL_STATUS_ACTIVE)
		return __ofono_error_failed(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &tones,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (is_valid_tones(tones) == FALSE)
		return __ofono_error_invalid_args(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->send_tones(vc,  tones, generic_callback, vc);

	return NULL;
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",    "",    "a{sv}",
					voicecall_manager_get_properties },
	{ "Dial",             "s",  "o",        voicecall_manager_dial,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "Hangup",           "",    "",         voicecall_manager_hangup,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "Answer",           "",    "",         voicecall_manager_answer,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "SendFlash",      "s",    "",         voicecall_manager_flash,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "SendTones",     "s",    "",        voicecall_manager_tone,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ "DisconnectReason",	"s" },
	{ }
};

void ofono_cdma_voicecall_disconnected(struct ofono_cdma_voicecall *vc,
					enum ofono_disconnect_reason reason,
					const struct ofono_error *error)
{
	DBG("Got disconnection event for reason: %d", reason);

	if (reason != OFONO_DISCONNECT_REASON_UNKNOWN)
		voicecall_emit_disconnect_reason(vc, reason);

	voicecall_set_call_status(vc, CDMA_CALL_STATUS_DISCONNECTED);
}

int ofono_cdma_voicecall_driver_register(
				const struct ofono_cdma_voicecall_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_cdma_voicecall_driver_unregister(
				const struct ofono_cdma_voicecall_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void cdma_voicecall_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	g_dbus_unregister_interface(conn, path,
				OFONO_CDMA_VOICECALL_MANAGER_INTERFACE);
	ofono_modem_remove_interface(modem,
				OFONO_CDMA_VOICECALL_MANAGER_INTERFACE);
}

static void voicecall_manager_remove(struct ofono_atom *atom)
{
	struct ofono_cdma_voicecall *vc = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (vc == NULL)
		return;

	if (vc->driver && vc->driver->remove)
		vc->driver->remove(vc);

	g_free(vc);
}

struct ofono_cdma_voicecall *ofono_cdma_voicecall_create(
						struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data)
{
	struct ofono_cdma_voicecall *vc;
	GSList *l;

	if (driver == NULL)
		return NULL;

	vc = g_try_new0(struct ofono_cdma_voicecall, 1);
	if (vc == NULL)
		return NULL;

	vc->status = CDMA_CALL_STATUS_DISCONNECTED;

	vc->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPE_CDMA_VOICECALL_MANAGER,
					voicecall_manager_remove, vc);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_cdma_voicecall_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(vc, vendor, data) < 0)
			continue;

		vc->driver = drv;
		break;
	}

	return vc;
}

void ofono_cdma_voicecall_register(struct ofono_cdma_voicecall *vc)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	const char *path = __ofono_atom_get_path(vc->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CDMA_VOICECALL_MANAGER_INTERFACE,
					manager_methods, manager_signals, NULL,
					vc, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CDMA_VOICECALL_MANAGER_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem,
				OFONO_CDMA_VOICECALL_MANAGER_INTERFACE);

	__ofono_atom_register(vc->atom, cdma_voicecall_unregister);
}

void ofono_cdma_voicecall_remove(struct ofono_cdma_voicecall *vc)
{
	__ofono_atom_free(vc->atom);
}

void ofono_cdma_voicecall_set_data(struct ofono_cdma_voicecall *vc, void *data)
{
	vc->driver_data = data;
}

void *ofono_cdma_voicecall_get_data(struct ofono_cdma_voicecall *vc)
{
	return vc->driver_data;
}
