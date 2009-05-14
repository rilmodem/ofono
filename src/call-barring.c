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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "driver.h"
#include "common.h"
#include "log.h"
#include "dbus-gsm.h"
#include "modem.h"
#include "cssn.h"
#include "ussd.h"

#define CALL_BARRING_INTERFACE "org.ofono.CallBarring"

#define CALL_BARRING_FLAG_CACHED 0x1

struct call_barring_data {
	struct ofono_call_barring_ops *ops;
	int flags;
	DBusMessage *pending;

	int *lock_enable;
	int query_next;
	int ss_req_type;
	int ss_req_cls;
};

struct call_barring_lock {
	const char *name;
	const char *fac;
};

static struct call_barring_lock cb_locks[] = {
	{ "BarrAllOutgoingCalls",	"AO" },
	{ "BarrOutgoingIntl",		"OI" },
	{ "BarrOutgoingIntlExceptHome",	"OX" },
	{ "BarrAllIncomingCalls",	"AI" },
	{ "BarrIncomingWhenRoaming",	"IR" },
	{ NULL,				NULL },
};

enum bearer_class cb_bearer_cls[] = {
	BEARER_CLASS_VOICE,
	BEARER_CLASS_DATA,
	BEARER_CLASS_FAX,
	0
};

static void set_lock(struct ofono_modem *modem, int value, int which)
{
	struct call_barring_data *cb = modem->call_barring;
	enum bearer_class *cls;
	char property_name[64];

	for (cls = cb_bearer_cls; *cls; cls++)
		if (*cls & (cb->lock_enable[which] ^ value)) {
			DBusConnection *conn = dbus_gsm_connection();
			dbus_bool_t dbusval = !!(*cls & value);

			snprintf(property_name, sizeof(property_name), "%s%s",
					bearer_class_to_string(*cls),
					cb_locks[which].name);
			dbus_gsm_signal_property_changed(conn, modem->path,
							CALL_BARRING_INTERFACE,
							property_name,
							DBUS_TYPE_BOOLEAN,
							&dbusval);
	}

	cb->lock_enable[which] = value;
}

static void cb_ss_dict_append(struct call_barring_data *cb,
		DBusMessageIter *dict, int which)
{
	dbus_bool_t dbus_value;

	dbus_value = !!(cb->lock_enable[which] & cb->ss_req_cls);
	dbus_gsm_dict_append(dict, cb_locks[which].name,
			DBUS_TYPE_BOOLEAN, &dbus_value);
}

static void generate_ss_query_reply(struct ofono_modem *modem)
{
	struct call_barring_data *cb = modem->call_barring;
	const char *context = "CallBarring";
	const char *sig = "(sa{sv})";
	const char *ss_type = ss_control_type_to_string(cb->ss_req_type);
	DBusConnection *conn = dbus_gsm_connection();
	DBusMessageIter iter;
	DBusMessageIter variant;
	DBusMessageIter vstruct;
	DBusMessageIter dict;
	DBusMessage *reply;
	int lck;

	reply = dbus_message_new_method_return(cb->pending);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &context);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, sig,
						&variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, NULL,
						&vstruct);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING,
					&ss_type);

	dbus_message_iter_open_container(&vstruct, DBUS_TYPE_ARRAY,
					PROPERTIES_ARRAY_SIGNATURE, &dict);

	if (cb_locks[cb->query_next].name)
		cb_ss_dict_append(cb, &dict, cb->query_next);
	else
		for (lck = 0; cb_locks[lck].name; lck++)
			cb_ss_dict_append(cb, &dict, lck);

	dbus_message_iter_close_container(&vstruct, &dict);

	dbus_message_iter_close_container(&variant, &vstruct);

	dbus_message_iter_close_container(&iter, &variant);

	g_dbus_send_message(conn, reply);
	cb->pending = NULL;
}

static void cb_ss_query_lock_callback(const struct ofono_error *error,
				int status, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Querrying a CB service via SS failed");
		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

		dbus_gsm_pending_reply(&cb->pending,
					dbus_gsm_failed(cb->pending));
		return;
	}

	if (cb_locks[cb->query_next].name)
		set_lock(modem, status, cb->query_next);

	generate_ss_query_reply(modem);
}

static gboolean cb_ss_query_all(gpointer user);
static void cb_ss_query_all_callback(const struct ofono_error *error,
				int status, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_lock(modem, status, cb->query_next);
	else {
		cb->flags &= ~CALL_BARRING_FLAG_CACHED;
		ofono_debug("Enabling/disabling Call Barring via SS "
				"successful, but query was not");

		dbus_gsm_pending_reply(&cb->pending,
					dbus_gsm_failed(cb->pending));
		return;
	}

	if (cb_locks[++cb->query_next].name)
		g_timeout_add(0, cb_ss_query_all, modem);
	else
		generate_ss_query_reply(modem);
}

static gboolean cb_ss_query_all(gpointer user)
{
	struct ofono_modem *modem = user;
	struct call_barring_data *cb = modem->call_barring;

	cb->ops->query(modem, cb_locks[cb->query_next].fac,
			cb_ss_query_all_callback, modem);

	return FALSE;
}

static void cb_ss_enable_lock_callback(const struct ofono_error *error,
		void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Enabling/disabling Call Barring via SS failed");
		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

		dbus_gsm_pending_reply(&cb->pending,
					dbus_gsm_failed(cb->pending));
		return;
	}

	if (cb->ops->query) {
		cb->flags |= CALL_BARRING_FLAG_CACHED;
		cb->query_next = 0;
		cb_ss_query_all(modem);
	}
}

static gboolean cb_ss_control(struct ofono_modem *modem,
				int type, const char *sc,
				const char *sia, const char *sib,
				const char *sic, const char *dn,
				DBusMessage *msg)
{
	struct call_barring_data *cb = modem->call_barring;
	DBusConnection *conn = dbus_gsm_connection();
	int cls = BEARER_CLASS_DEFAULT;
	const char *fac;
	DBusMessage *reply;
	void *operation;

	if (cb->pending) {
		reply = dbus_gsm_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	ofono_debug("Received call barring ss control request");

	ofono_debug("type: %d, sc: %s, sia: %s, sib: %s, sic: %s, dn: %s",
			type, sc, sia, sib, sic, dn);

	if (!strcmp(sc, "33"))
		fac = "AO";
	else if (!strcmp(sc, "331"))
		fac = "OI";
	else if (!strcmp(sc, "332"))
		fac = "OX";
	else if (!strcmp(sc, "35"))
		fac = "AI";
	else if (!strcmp(sc, "351"))
		fac = "IR";
	else if (!strcmp(sc, "330"))
		fac = "AB";
	else if (!strcmp(sc, "333"))
		fac = "AG";
	else if (!strcmp(sc, "335"))
		fac = "AI";
	else
		return FALSE;

	for (cb->query_next = 0;
		cb_locks[cb->query_next].name &&
		strcmp(fac, cb_locks[cb->query_next].fac);
		cb->query_next++);

	switch (type) {
	case SS_CONTROL_TYPE_REGISTRATION:
	case SS_CONTROL_TYPE_ERASURE:
		operation = cb->ops->set;
		break;
	case SS_CONTROL_TYPE_QUERY:
		operation = cb->ops->query;
		break;
	case SS_CONTROL_TYPE_ACTIVATION:
	case SS_CONTROL_TYPE_DEACTIVATION:
		goto bad_format;
	}

	if (!operation) {
		reply = dbus_gsm_not_implemented(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	if (strlen(sib) > 0) {
		long service_code;
		char *end;

		service_code = strtoul(sib, &end, 10);

		if (end == sib || *end != '\0')
			goto bad_format;

		cls = mmi_service_code_to_bearer_class(service_code);

		if (cls == 0)
			goto bad_format;
	}

	if (strlen(sic) > 0)
		goto bad_format;

	cb->ss_req_cls = cls;
	cb->pending = dbus_message_ref(msg);

	switch (type) {
	case SS_CONTROL_TYPE_REGISTRATION:
		cb->ss_req_type = SS_CONTROL_TYPE_ACTIVATION;
		cb->ops->set(modem, fac, 1, sia, cls,
				cb_ss_enable_lock_callback, modem);
		break;
	case SS_CONTROL_TYPE_ERASURE:
		cb->ss_req_type = SS_CONTROL_TYPE_DEACTIVATION;
		cb->ops->set(modem, fac, 0, sia, cls,
				cb_ss_enable_lock_callback, modem);
		break;
	case SS_CONTROL_TYPE_QUERY:
		cb->ss_req_type = SS_CONTROL_TYPE_QUERY;
		cb->ops->query(modem, fac, cb_ss_query_lock_callback, modem);
		break;
	}

	return TRUE;

bad_format:
	reply = dbus_gsm_invalid_format(msg);
	g_dbus_send_message(conn, reply);
	return TRUE;
}

static void cb_register_ss_controls(struct ofono_modem *modem)
{
	ss_control_register(modem, "33", cb_ss_control);
	ss_control_register(modem, "331", cb_ss_control);
	ss_control_register(modem, "332", cb_ss_control);
	ss_control_register(modem, "35", cb_ss_control);
	ss_control_register(modem, "351", cb_ss_control);
	ss_control_register(modem, "330", cb_ss_control);
	ss_control_register(modem, "333", cb_ss_control);
	ss_control_register(modem, "335", cb_ss_control);
}

static void cb_unregister_ss_controls(struct ofono_modem *modem)
{
	ss_control_unregister(modem, "33", cb_ss_control);
	ss_control_unregister(modem, "331", cb_ss_control);
	ss_control_unregister(modem, "332", cb_ss_control);
	ss_control_unregister(modem, "35", cb_ss_control);
	ss_control_unregister(modem, "351", cb_ss_control);
	ss_control_unregister(modem, "330", cb_ss_control);
	ss_control_unregister(modem, "333", cb_ss_control);
	ss_control_unregister(modem, "335", cb_ss_control);
}

static struct call_barring_data *call_barring_create(void)
{
	int lcount;
	struct call_barring_data *cb = g_try_new0(struct call_barring_data, 1);

	for (lcount = 0; cb_locks[lcount].name; lcount++);

	cb->lock_enable = g_try_new0(int, lcount);

	return cb;
}

static void call_barring_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct call_barring_data *cb = modem->call_barring;

	g_free(cb->lock_enable);
	g_free(cb);

	modem->call_barring = NULL;
}

static void cb_get_properties_reply(struct ofono_modem *modem)
{
	struct call_barring_data *cb = modem->call_barring;
	struct call_barring_lock *lock;
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	int *enable;
	enum bearer_class *cls;
	char property_name[64];
	dbus_bool_t dbus_value;

	if (!(cb->flags & CALL_BARRING_FLAG_CACHED)) {
		dbus_gsm_pending_reply(&cb->pending,
					dbus_gsm_failed(cb->pending));
		return;
	}

	reply = dbus_message_new_method_return(cb->pending);
	if (!reply)
		return;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			PROPERTIES_ARRAY_SIGNATURE, &dict);

	for (lock = cb_locks, enable = cb->lock_enable; lock->name;
			lock++, enable++)
		for (cls = cb_bearer_cls; *cls; cls ++) {
			dbus_value = !!(*enable & *cls);

			snprintf(property_name, sizeof(property_name), "%s%s",
					bearer_class_to_string(*cls),
					lock->name);
			dbus_gsm_dict_append(&dict, property_name,
					DBUS_TYPE_BOOLEAN, &dbus_value);
		}

	dbus_message_iter_close_container(&iter, &dict);

	dbus_gsm_pending_reply(&cb->pending, reply);
}

static gboolean query_lock(gpointer user);
static void query_lock_callback(const struct ofono_error *error,
				int status, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_lock(modem, status, cb->query_next);
	else
		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

	if (cb_locks[++cb->query_next].name)
		g_timeout_add(0, query_lock, modem);
	else if (cb->pending)
		cb_get_properties_reply(modem);
}

static gboolean query_lock(gpointer user)
{
	struct ofono_modem *modem = user;
	struct call_barring_data *cb = modem->call_barring;

	cb->ops->query(modem, cb_locks[cb->query_next].fac,
			query_lock_callback, modem);

	return FALSE;
}

static DBusMessage *cb_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (cb->pending)
		return dbus_gsm_busy(msg);

	if (!cb->ops->query)
		return dbus_gsm_not_implemented(msg);

	cb->pending = dbus_message_ref(msg);

	if (cb->flags & CALL_BARRING_FLAG_CACHED)
		cb_get_properties_reply(modem);
	else {
		cb->flags |= CALL_BARRING_FLAG_CACHED;
		cb->query_next = 0;
		query_lock(modem);
	}

	return NULL;
}

static void set_lock_query_callback(const struct ofono_error *error, int value,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessage *reply;

	if (!cb->pending)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Enabling/disabling a lock successful, "
				"but query was not");

		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

		dbus_gsm_pending_reply(&cb->pending,
					dbus_gsm_failed(cb->pending));
		return;
	}

	reply = dbus_message_new_method_return(cb->pending);
	dbus_gsm_pending_reply(&cb->pending, reply);

	set_lock(modem, value, cb->query_next);
}

static void set_lock_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Enabling/disabling a lock failed");
		dbus_gsm_pending_reply(&cb->pending,
					dbus_gsm_failed(cb->pending));
		return;
	}

	cb->ops->query(modem, cb_locks[cb->query_next].fac,
			set_lock_query_callback, modem);
}

static gboolean cb_lock_property_lookup(const char *property,
		int *out_which, enum bearer_class *out_cls)
{
	enum bearer_class *cls;
	const char *prefix;
	int which;
	size_t len;

	/* We check the bearer classes here, e.g. voice, data, fax, sms */
	for (cls = cb_bearer_cls; *cls; cls++) {
		prefix = bearer_class_to_string(*cls);
		len = strlen(prefix);

		if (!strncmp(property, prefix, len))
			break;
	}
	if (!*cls)
		return FALSE;

	property += len;

	/* We look up the lock category now */
	for (which = 0; cb_locks[which].name; which++)
		if (!strcmp(property, cb_locks[which].name)) {
			*out_which = which;
			*out_cls = *cls;
			return TRUE;
		}

	return FALSE;
}

static DBusMessage *cb_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name, *passwd = "";
	enum bearer_class cls;
	dbus_bool_t value;

	if (cb->pending)
		return dbus_gsm_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return dbus_gsm_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);
	if (!cb_lock_property_lookup(name, &cb->query_next, &cls))
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_next(&iter);
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);
	if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
		return dbus_gsm_invalid_format(msg);

	dbus_message_iter_get_basic(&var, &value);

	if (dbus_message_iter_next(&iter)) {
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
			return dbus_gsm_invalid_args(msg);

		dbus_message_iter_get_basic(&iter, &passwd);
		if (!is_valid_pin(passwd))
			return dbus_gsm_invalid_format(msg);
	}

	if (!cb->ops->set)
		return dbus_gsm_not_implemented(msg);

	cb->pending = dbus_message_ref(msg);
	cb->ops->set(modem, cb_locks[cb->query_next].fac, value, passwd,
			cls, set_lock_callback, modem);

	return NULL;
}

static gboolean disable_all_query(gpointer user);
static void disable_all_query_callback(const struct ofono_error *error,
				int status, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_lock(modem, status, cb->query_next);
	else
		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

	if (cb_locks[++cb->query_next].name)
		g_timeout_add(0, disable_all_query, modem);
	else if (cb->pending) {
		if (!(cb->flags & CALL_BARRING_FLAG_CACHED)) {
			ofono_error("Disabling all barring successful, "
					"but query was not");
			reply = dbus_gsm_failed(cb->pending);
		} else
			reply = dbus_message_new_method_return(cb->pending);

		dbus_gsm_pending_reply(&cb->pending, reply);
	}
}

static gboolean disable_all_query(gpointer user)
{
	struct ofono_modem *modem = user;
	struct call_barring_data *cb = modem->call_barring;

	cb->ops->query(modem, cb_locks[cb->query_next].fac,
			disable_all_query_callback, modem);

	return FALSE;
}

static void disable_all_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Disabling all barring failed");
		dbus_gsm_pending_reply(&cb->pending,
					dbus_gsm_failed(cb->pending));
		return;
	}

	/* Re-query all */
	if (cb->ops->query) {
		cb->flags |= CALL_BARRING_FLAG_CACHED;
		cb->query_next = 0;
		disable_all_query(modem);
	}
}

static DBusMessage *cb_disable_all(DBusConnection *conn, DBusMessage *msg,
					void *data, const char *fac)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessageIter iter;
	const char *passwd = "";

	if (cb->pending)
		return dbus_gsm_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return dbus_gsm_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &passwd);
	if (!is_valid_pin(passwd))
		return dbus_gsm_invalid_format(msg);

	if (!cb->ops->set)
		return dbus_gsm_not_implemented(msg);

	cb->pending = dbus_message_ref(msg);
	cb->ops->set(modem, fac, 0, passwd,
			BEARER_CLASS_DEFAULT, disable_all_callback, modem);

	return NULL;
}

static DBusMessage *cb_disable_ab(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	return cb_disable_all(conn, msg, data, "AB");
}

static DBusMessage *cb_disable_ac(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	return cb_disable_all(conn, msg, data, "AC");
}

static DBusMessage *cb_disable_ag(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	return cb_disable_all(conn, msg, data, "AG");
}

static GDBusMethodTable cb_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cb_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SetProperty",	"svs",	"",		cb_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "DisableAll",		"s",	"",		cb_disable_ab,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "DisableAllIncoming",	"s",	"",		cb_disable_ac,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "DisableAllOutgoing",	"s",	"",		cb_disable_ag,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cb_signals[] = {
	{ "IncomingBarringInEffect",	"" },
	{ "OutgoingBarringInEffect",	"" },
	{ "PropertyChanged",		"sv" },
	{ }
};

static void call_barring_incoming_enabled_notify(int idx, void *userdata)
{
	struct ofono_modem *modem = userdata;
	DBusConnection *conn = dbus_gsm_connection();
	DBusMessage *signal;

	signal = dbus_message_new_signal(modem->path,
			CALL_BARRING_INTERFACE, "IncomingBarringInEffect");
	if (!signal) {
		ofono_error("Unable to allocate new %s.IncomingBarringInEffect"
				" signal", CALL_BARRING_INTERFACE);
		return;
	}

	g_dbus_send_message(conn, signal);
}

static void call_barring_outgoing_enabled_notify(int idx, void *userdata)
{
	struct ofono_modem *modem = userdata;
	DBusConnection *conn = dbus_gsm_connection();
	DBusMessage *signal;

	signal = dbus_message_new_signal(modem->path,
			CALL_BARRING_INTERFACE, "OutgoingBarringInEffect");
	if (!signal) {
		ofono_error("Unable to allocate new %s.OutgoingBarringInEffect"
				" signal", CALL_BARRING_INTERFACE);
		return;
	}

	g_dbus_send_message(conn, signal);
}

int ofono_call_barring_register(struct ofono_modem *modem,
				struct ofono_call_barring_ops *ops)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (!modem || !ops)
		return -1;

	modem->call_barring = call_barring_create();

	if (!modem->call_barring)
		return -1;

	modem->call_barring->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					CALL_BARRING_INTERFACE,
					cb_methods, cb_signals, NULL, modem,
					call_barring_destroy)) {
		ofono_error("Could not create %s interface",
				CALL_BARRING_INTERFACE);
		call_barring_destroy(modem);

		return -1;
	}

	modem_add_interface(modem, CALL_BARRING_INTERFACE);

	cb_register_ss_controls(modem);

	ofono_mo_ss_register(modem, SS_MO_INCOMING_BARRING,
			call_barring_incoming_enabled_notify, modem);
	ofono_mo_ss_register(modem, SS_MO_OUTGOING_BARRING,
			call_barring_outgoing_enabled_notify, modem);

	return 0;
}

void ofono_call_barring_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (!modem->call_barring)
		return;

	modem_remove_interface(modem, CALL_BARRING_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path, CALL_BARRING_INTERFACE);

	cb_unregister_ss_controls(modem);

	ofono_mo_ss_unregister(modem, SS_MO_INCOMING_BARRING,
			call_barring_incoming_enabled_notify, modem);
	ofono_mo_ss_unregister(modem, SS_MO_OUTGOING_BARRING,
			call_barring_outgoing_enabled_notify, modem);

	modem->call_barring = NULL;
}
