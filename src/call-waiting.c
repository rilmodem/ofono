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
#include <stdlib.h>
#include <stdio.h>

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "dbus-gsm.h"
#include "modem.h"
#include "ussd.h"

#define CALL_WAITING_INTERFACE "org.ofono.CallWaiting"

#define CALL_WAITING_FLAG_CACHED 0x1

struct call_waiting_data {
	struct ofono_call_waiting_ops *ops;
	int flags;
	DBusMessage *pending;
	int conditions;
	int ss_req_type;
	int ss_req_cls;
};

static void cw_register_ss_controls(struct ofono_modem *modem);
static void cw_unregister_ss_controls(struct ofono_modem *modem);

static struct call_waiting_data *call_waiting_create()
{
	struct call_waiting_data *r;

	r = g_new0(struct call_waiting_data, 1);

	return r;
}

static void call_waiting_destroy(gpointer data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;

	cw_unregister_ss_controls(modem);

	g_free(cw);
}

static void update_conditions(struct ofono_modem *modem, int new_conditions,
				int mask)
{
	struct call_waiting_data *cw = modem->call_waiting;
	DBusConnection *conn = dbus_gsm_connection();
	char buf[64];
	int j;
	const char *value;

	for (j = 1; j <= BEARER_CLASS_PAD; j = j << 1) {
		if ((j & mask) == 0)
			continue;

		if ((cw->conditions & j) == (new_conditions & j))
			continue;

		if (new_conditions & j)
			value = "enabled";
		else
			value = "disabled";

		sprintf(buf, "%s", bearer_class_to_string(j));
		dbus_gsm_signal_property_changed(conn, modem->path,
							CALL_WAITING_INTERFACE,
							buf, DBUS_TYPE_STRING,
							&value);
	}

	cw->conditions = new_conditions;
}

static void property_append_cw_conditions(DBusMessageIter *dict,
						int conditions, int mask)
{
	int i;
	const char *prop;
	const char *value;

	for (i = 1; i <= BEARER_CLASS_PAD; i = i << 1) {
		if (!(mask & i))
			continue;

		prop = bearer_class_to_string(i);

		if (conditions & i)
			value = "enabled";
		else
			value = "disabled";

		dbus_gsm_dict_append(dict, prop, DBUS_TYPE_STRING, &value);
	}
}

static void generate_ss_query_reply(struct ofono_modem *modem)
{
	struct call_waiting_data *cw = modem->call_waiting;
	const char *sig = "(sa{sv})";
	const char *ss_type = ss_control_type_to_string(cw->ss_req_type);
	const char *context = "CallWaiting";
	DBusMessageIter iter;
	DBusMessageIter var;
	DBusMessageIter vstruct;
	DBusMessageIter dict;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(cw->pending);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &context);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, sig, &var);

	dbus_message_iter_open_container(&var, DBUS_TYPE_STRUCT, NULL,
						&vstruct);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING,
					&ss_type);

	dbus_message_iter_open_container(&vstruct, DBUS_TYPE_ARRAY,
					PROPERTIES_ARRAY_SIGNATURE, &dict);

	property_append_cw_conditions(&dict, cw->conditions, cw->ss_req_cls);

	dbus_message_iter_close_container(&vstruct, &dict);

	dbus_message_iter_close_container(&var, &vstruct);

	dbus_message_iter_close_container(&iter, &var);

	dbus_gsm_pending_reply(&cw->pending, reply);
}

static void cw_ss_query_callback(const struct ofono_error *error, int status,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("setting CW via SS failed");

		cw->flags &= ~CALL_WAITING_FLAG_CACHED;
		dbus_gsm_pending_reply(&cw->pending,
					dbus_gsm_failed(cw->pending));

		return;
	}

	update_conditions(modem, status, BEARER_CLASS_VOICE);
	cw->flags |= CALL_WAITING_FLAG_CACHED;

	generate_ss_query_reply(modem);
}

static void cw_ss_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;
	int cls;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("setting CW via SS failed");
		dbus_gsm_pending_reply(&cw->pending,
					dbus_gsm_failed(cw->pending));

		return;
	}

	cls = cw->ss_req_cls | BEARER_CLASS_DEFAULT;

	cw->ops->query(modem, cw->ss_req_cls, cw_ss_query_callback, modem);
}

static gboolean cw_ss_control(struct ofono_modem *modem, int type,
				const char *sc, const char *sia,
				const char *sib, const char *sic,
				const char *dn, DBusMessage *msg)
{
	struct call_waiting_data *cw = modem->call_waiting;
	DBusConnection *conn = dbus_gsm_connection();
	int cls = BEARER_CLASS_SS_DEFAULT;
	DBusMessage *reply;

	if (!cw)
		return FALSE;

	if (strcmp(sc, "43"))
		return FALSE;

	if (cw->pending) {
		reply = dbus_gsm_busy(msg);
		goto error;
	}

	if (strlen(sib) || strlen(sib) || strlen(dn))
		goto bad_format;

	if ((type == SS_CONTROL_TYPE_QUERY && !cw->ops->query) ||
		(type != SS_CONTROL_TYPE_QUERY && !cw->ops->set)) {
		reply = dbus_gsm_not_implemented(msg);
		goto error;
	}

	if (strlen(sia) > 0) {
		long service_code;
		char *end;

		service_code = strtoul(sia, &end, 10);

		if (end == sia || *end != '\0')
			goto bad_format;

		cls = mmi_service_code_to_bearer_class(service_code);
		if (cls == 0)
			goto bad_format;
	}

	cw->ss_req_cls = cls;
	cw->pending = dbus_message_ref(msg);

	cls = BEARER_CLASS_DEFAULT;

	switch (type) {
	case SS_CONTROL_TYPE_REGISTRATION:
	case SS_CONTROL_TYPE_ACTIVATION:
		cw->ss_req_type = SS_CONTROL_TYPE_ACTIVATION;
		cw->ops->set(modem, 1, cls, cw_ss_set_callback, modem);
		break;

	case SS_CONTROL_TYPE_QUERY:
		cw->ss_req_type = SS_CONTROL_TYPE_QUERY;
		cw->ops->query(modem, cls, cw_ss_query_callback, modem);
		break;

	case SS_CONTROL_TYPE_DEACTIVATION:
	case SS_CONTROL_TYPE_ERASURE:
		cw->ss_req_type = SS_CONTROL_TYPE_DEACTIVATION;
		cw->ops->set(modem, 0, cls, cw_ss_set_callback, modem);
		break;
	}

	return TRUE;

bad_format:
	reply = dbus_gsm_invalid_format(msg);
error:
	g_dbus_send_message(conn, reply);
	return TRUE;
}

static void cw_register_ss_controls(struct ofono_modem *modem)
{
	ss_control_register(modem, "43", cw_ss_control);
}

static void cw_unregister_ss_controls(struct ofono_modem *modem)
{
	ss_control_unregister(modem, "43", cw_ss_control);
}

static DBusMessage *generate_get_properties_reply(struct ofono_modem *modem,
							DBusMessage *msg)
{
	struct call_waiting_data *cw = modem->call_waiting;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	property_append_cw_conditions(&dict, cw->conditions,
					BEARER_CLASS_VOICE);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void cw_query_callback(const struct ofono_error *error, int status,
				void *data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during cw query");
		goto out;
	}

	update_conditions(modem, status, BEARER_CLASS_VOICE);
	cw->flags |= CALL_WAITING_FLAG_CACHED;

out:
	if (cw->pending) {
		DBusMessage *reply;

		reply = generate_get_properties_reply(modem, cw->pending);
		dbus_gsm_pending_reply(&cw->pending, reply);
	}
}

static DBusMessage *cw_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;

	if (cw->pending)
		return dbus_gsm_busy(msg);

	if (!cw->ops->query)
		return dbus_gsm_not_implemented(msg);

	if (cw->flags & CALL_WAITING_FLAG_CACHED)
		return generate_get_properties_reply(modem, msg);

	cw->pending = dbus_message_ref(msg);

	cw->ops->query(modem, BEARER_CLASS_DEFAULT, cw_query_callback, modem);

	return NULL;
}

static void set_query_callback(const struct ofono_error *error, int status,
				void *data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("CW set succeeded, but query failed!");
		cw->flags &= ~CALL_WAITING_FLAG_CACHED;

		dbus_gsm_pending_reply(&cw->pending,
					dbus_gsm_failed(cw->pending));
		return;
	}

	dbus_gsm_pending_reply(&cw->pending,
				dbus_message_new_method_return(cw->pending));

	update_conditions(modem, status, BEARER_CLASS_VOICE);
}

static void set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during CW set");

		dbus_gsm_pending_reply(&cw->pending,
					dbus_gsm_failed(cw->pending));

		return;
	}

	cw->ops->query(modem, BEARER_CLASS_DEFAULT, set_query_callback, modem);
}

static DBusMessage *cw_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_waiting_data *cw = modem->call_waiting;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	int i;

	if (cw->pending)
		return dbus_gsm_busy(msg);

	if (!cw->ops->set)
		return dbus_gsm_not_implemented(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return dbus_gsm_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	for (i = 1; i < BEARER_CLASS_SMS; i = i << 1)
		if (!strcmp(property, bearer_class_to_string(i)))
			break;

	if (i < BEARER_CLASS_SMS) {
		const char *value;
		int status;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return dbus_gsm_invalid_format(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (!strcmp(value, "enabled"))
			status = 1;
		else if (!strcmp(value, "disabled"))
			status = 0;
		else
			return dbus_gsm_invalid_format(msg);

		cw->pending = dbus_message_ref(msg);

		cw->ops->set(modem, status, i, set_callback, modem);
	}

	return dbus_gsm_invalid_args(msg);
}

static GDBusMethodTable cw_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cw_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SetProperty",	"sv",	"",		cw_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cw_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

int ofono_call_waiting_register(struct ofono_modem *modem,
				struct ofono_call_waiting_ops *ops)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->call_waiting = call_waiting_create();

	if (!modem->call_waiting)
		return -1;

	modem->call_waiting->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					CALL_WAITING_INTERFACE,
					cw_methods, cw_signals, NULL,
					modem, call_waiting_destroy)) {
		ofono_error("Could not register CallWaiting %s", modem->path);
		call_waiting_destroy(modem);

		return -1;
	}

	ofono_debug("Registered call waiting interface");

	cw_register_ss_controls(modem);

	modem_add_interface(modem, CALL_WAITING_INTERFACE);
	return 0;
}

void ofono_call_waiting_unregister(struct ofono_modem *modem)
{
	struct call_waiting_data *cw = modem->call_waiting;
	DBusConnection *conn = dbus_gsm_connection();

	if (!cw)
		return;

	modem_remove_interface(modem, CALL_WAITING_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path,
					CALL_WAITING_INTERFACE);

	modem->call_waiting = NULL;
}
