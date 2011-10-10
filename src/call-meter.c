/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"

#define CALL_METER_FLAG_CACHED 0x1
#define CALL_METER_FLAG_HAVE_PUCT 0x2

static GSList *g_drivers = NULL;

struct ofono_call_meter {
	int flags;
	DBusMessage *pending;
	int call_meter;
	int acm;
	int acm_max;
	double ppu;
	char currency[4];
	const struct ofono_call_meter_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static void set_call_meter(struct ofono_call_meter *cm, int value)
{
	DBusConnection *conn;
	const char *path;

	if (cm->call_meter == value)
		return;

	cm->call_meter = value;

	conn = ofono_dbus_get_connection();
	path = __ofono_atom_get_path(cm->atom);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_METER_INTERFACE,
						"CallMeter", DBUS_TYPE_UINT32,
						&cm->call_meter);
}

static void set_acm(struct ofono_call_meter *cm, int value)
{
	DBusConnection *conn;
	const char *path;

	if (cm->acm == value)
		return;

	cm->acm = value;

	conn = ofono_dbus_get_connection();
	path = __ofono_atom_get_path(cm->atom);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_METER_INTERFACE,
						"AccumulatedCallMeter",
						DBUS_TYPE_UINT32, &cm->acm);
}

static void set_acm_max(struct ofono_call_meter *cm, int value)
{
	DBusConnection *conn;
	const char *path;

	if (cm->acm_max == value)
		return;

	cm->acm_max = value;

	conn = ofono_dbus_get_connection();
	path = __ofono_atom_get_path(cm->atom);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_METER_INTERFACE,
						"AccumulatedCallMeterMaximum",
						DBUS_TYPE_UINT32, &cm->acm_max);
}

static void set_ppu(struct ofono_call_meter *cm, double value)
{
	DBusConnection *conn;
	const char *path;

	if (cm->ppu == value)
		return;

	cm->ppu = value;

	conn = ofono_dbus_get_connection();
	path = __ofono_atom_get_path(cm->atom);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_METER_INTERFACE,
						"PricePerUnit",
						DBUS_TYPE_DOUBLE, &cm->ppu);
}

static void set_currency(struct ofono_call_meter *cm, const char *value)
{
	DBusConnection *conn;
	const char *path;
	const char *dbusval;

	if (strlen(value) > 3) {
		ofono_error("Currency reported with size > 3: %s", value);
		return;
	}

	if (!strcmp(cm->currency, value))
		return;

	strncpy(cm->currency, value, 3);
	cm->currency[3] = '\0';

	conn = ofono_dbus_get_connection();
	path = __ofono_atom_get_path(cm->atom);
	dbusval = cm->currency;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_METER_INTERFACE,
						"Currency", DBUS_TYPE_STRING,
						&dbusval);
}

static void cm_get_properties_reply(struct ofono_call_meter *cm)
{
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	const char *currency = cm->currency;

	reply = dbus_message_new_method_return(cm->pending);
	if (reply == NULL)
		return;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "CallMeter", DBUS_TYPE_UINT32,
				&cm->call_meter);

	ofono_dbus_dict_append(&dict, "AccumulatedCallMeter", DBUS_TYPE_UINT32,
				&cm->acm);

	ofono_dbus_dict_append(&dict, "AccumulatedCallMeterMaximum",
				DBUS_TYPE_UINT32, &cm->acm_max);

	ofono_dbus_dict_append(&dict, "PricePerUnit", DBUS_TYPE_DOUBLE,
				&cm->ppu);

	ofono_dbus_dict_append(&dict, "Currency", DBUS_TYPE_STRING, &currency);

	dbus_message_iter_close_container(&iter, &dict);

	__ofono_dbus_pending_reply(&cm->pending, reply);
}

static void query_call_meter_callback(const struct ofono_error *error,
					int value, void *data)
{
	struct ofono_call_meter *cm = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_call_meter(cm, value);

	if (cm->pending)
		cm_get_properties_reply(cm);
}

static void query_call_meter(struct ofono_call_meter *cm)
{
	if (cm->driver->call_meter_query == NULL) {
		if (cm->pending)
			cm_get_properties_reply(cm);

		return;
	}

	cm->driver->call_meter_query(cm, query_call_meter_callback, cm);
}

static void query_acm_callback(const struct ofono_error *error, int value,
					void *data)
{
	struct ofono_call_meter *cm = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_acm(cm, value);

	query_call_meter(cm);
}

static void query_acm(struct ofono_call_meter *cm)
{
	if (cm->driver->acm_query == NULL) {
		query_call_meter(cm);
		return;
	}

	cm->driver->acm_query(cm, query_acm_callback, cm);
}

static void query_acm_max_callback(const struct ofono_error *error, int value,
					void *data)
{
	struct ofono_call_meter *cm = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_acm_max(cm, value);

	cm->flags |= CALL_METER_FLAG_CACHED;

	query_acm(cm);
}

static void query_acm_max(struct ofono_call_meter *cm)
{
	if (cm->driver->acm_max_query == NULL) {
		cm->flags |= CALL_METER_FLAG_CACHED;

		query_acm(cm);
		return;
	}

	cm->driver->acm_max_query(cm, query_acm_max_callback, cm);
}

static void query_puct_callback(const struct ofono_error *error,
				const char *currency, double ppu, void *data)
{
	struct ofono_call_meter *cm = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		cm->flags |= CALL_METER_FLAG_HAVE_PUCT;
		set_currency(cm, currency);
		set_ppu(cm, ppu);
	}

	query_acm_max(cm);
}

static void query_puct(struct ofono_call_meter *cm)
{
	if (cm->driver->puct_query == NULL)
		query_acm_max(cm);
	else
		cm->driver->puct_query(cm, query_puct_callback, cm);
}

static DBusMessage *cm_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_meter *cm = data;

	if (cm->pending)
		return __ofono_error_busy(msg);

	cm->pending = dbus_message_ref(msg);

	/*
	 * We don't need to query ppu, currency & acm_max every time
	 * Not sure if we have to query acm & call_meter every time
	 * so lets play on the safe side and query them.  They should be
	 * fast to query anyway
	 */
	if (cm->flags & CALL_METER_FLAG_CACHED)
		query_acm(cm);
	else
		query_puct(cm);

	return NULL;
}

static void set_acm_max_query_callback(const struct ofono_error *error,
					int value, void *data)
{
	struct ofono_call_meter *cm = data;
	DBusMessage *reply;

	if (cm->pending == NULL)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Setting acm_max successful, but query was not");

		cm->flags &= ~CALL_METER_FLAG_CACHED;

		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		return;
	}

	reply = dbus_message_new_method_return(cm->pending);
	__ofono_dbus_pending_reply(&cm->pending, reply);

	set_acm_max(cm, value);
}

static void check_pin2_state(struct ofono_call_meter *cm)
{
	struct ofono_atom *sim_atom;

	sim_atom = __ofono_modem_find_atom(__ofono_atom_get_modem(cm->atom),
						OFONO_ATOM_TYPE_SIM);
	if (sim_atom == NULL)
		return;

	__ofono_sim_recheck_pin(__ofono_atom_get_data(sim_atom));
}

static void set_acm_max_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_meter *cm = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Setting acm_max failed");
		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		check_pin2_state(cm);
		return;
	}

	/* Assume if we have acm_reset, we have acm_query */
	cm->driver->acm_max_query(cm, set_acm_max_query_callback, cm);
}

static DBusMessage *prop_set_acm_max(DBusMessage *msg,
					struct ofono_call_meter *cm,
					DBusMessageIter *dbus_value,
					const char *pin2)
{
	dbus_uint32_t value;

	if (cm->driver->acm_max_set == NULL)
		return __ofono_error_not_implemented(msg);

	dbus_message_iter_get_basic(dbus_value, &value);

	cm->pending = dbus_message_ref(msg);

	cm->driver->acm_max_set(cm, value, pin2, set_acm_max_callback, cm);

	return NULL;
}

static void set_puct_query_callback(const struct ofono_error *error,
					const char *currency, double ppu,
					void *data)
{
	struct ofono_call_meter *cm = data;
	DBusMessage *reply;

	if (cm->pending == NULL)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Setting PUCT successful, but query was not");

		cm->flags &= ~CALL_METER_FLAG_CACHED;

		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		return;
	}

	reply = dbus_message_new_method_return(cm->pending);
	__ofono_dbus_pending_reply(&cm->pending, reply);

	set_currency(cm, currency);
	set_ppu(cm, ppu);
}

static void set_puct_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_meter *cm = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("setting puct failed");
		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		check_pin2_state(cm);
		return;
	}

	/* Assume if we have puct_set, we have puct_query */
	cm->driver->puct_query(cm, set_puct_query_callback, cm);
}

/*
 * This function is for the really bizarre case of someone trying to call
 * SetProperty before GetProperties.  But we must handle it...
 */
static void set_puct_initial_query_callback(const struct ofono_error *error,
						const char *currency,
						double ppu, void *data)
{
	struct ofono_call_meter *cm = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name;
	const char *pin2;

	if (cm->pending == NULL)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		return;
	}

	set_currency(cm, currency);
	set_ppu(cm, ppu);

	cm->flags |= CALL_METER_FLAG_HAVE_PUCT;

	dbus_message_iter_init(cm->pending, &iter);
	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &var);
	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &pin2);

	if (!strcmp(name, "PricePerUnit"))
		dbus_message_iter_get_basic(&var, &ppu);
	else
		dbus_message_iter_get_basic(&var, &currency);

	cm->driver->puct_set(cm, currency, ppu, pin2,
				set_puct_callback, cm);
}

static DBusMessage *prop_set_ppu(DBusMessage *msg, struct ofono_call_meter *cm,
				DBusMessageIter *var, const char *pin2)
{
	double ppu;

	if (cm->driver->puct_set == NULL || cm->driver->puct_query == NULL)
		return __ofono_error_not_implemented(msg);

	dbus_message_iter_get_basic(var, &ppu);

	if (ppu < 0.0)
		return __ofono_error_invalid_format(msg);

	cm->pending = dbus_message_ref(msg);

	if (cm->flags & CALL_METER_FLAG_HAVE_PUCT)
		cm->driver->puct_set(cm, cm->currency, ppu, pin2,
					set_puct_callback, cm);
	else
		cm->driver->puct_query(cm, set_puct_initial_query_callback, cm);

	return NULL;
}

static DBusMessage *prop_set_cur(DBusMessage *msg, struct ofono_call_meter *cm,
				DBusMessageIter *var, const char *pin2)
{
	const char *value;

	if (cm->driver->puct_set == NULL || cm->driver->puct_query == NULL)
		return __ofono_error_not_implemented(msg);

	dbus_message_iter_get_basic(var, &value);

	if (strlen(value) > 3)
		return __ofono_error_invalid_format(msg);

	cm->pending = dbus_message_ref(msg);

	if (cm->flags & CALL_METER_FLAG_HAVE_PUCT)
		cm->driver->puct_set(cm, value, cm->ppu, pin2,
					set_puct_callback, cm);
	else
		cm->driver->puct_query(cm, set_puct_initial_query_callback, cm);

	return NULL;
}

struct call_meter_property {
	const char *name;
	int type;
	DBusMessage* (*set)(DBusMessage *msg, struct ofono_call_meter *cm,
				DBusMessageIter *var, const char *pin2);
};

static struct call_meter_property cm_properties[] = {
	{ "AccumulatedCallMeterMaximum",DBUS_TYPE_UINT32,	prop_set_acm_max },
	{ "PricePerUnit",		DBUS_TYPE_DOUBLE,	prop_set_ppu },
	{ "Currency",			DBUS_TYPE_STRING,	prop_set_cur },
	{ NULL, 0, 0 },
};

static DBusMessage *cm_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_meter *cm = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name, *passwd = "";
	struct call_meter_property *property;

	if (cm->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);

	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (!dbus_message_iter_next(&iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &passwd);

	if (!__ofono_is_valid_sim_pin(passwd, OFONO_SIM_PASSWORD_SIM_PIN2))
		return __ofono_error_invalid_format(msg);

	for (property = cm_properties; property->name; property++) {
		if (strcmp(name, property->name))
			continue;

		if (dbus_message_iter_get_arg_type(&var) != property->type)
			return __ofono_error_invalid_args(msg);

		return property->set(msg, cm, &var, passwd);
	}

	return __ofono_error_invalid_args(msg);
}

static void reset_acm_query_callback(const struct ofono_error *error, int value,
					void *data)
{
	struct ofono_call_meter *cm = data;
	DBusMessage *reply;

	if (cm->pending == NULL)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Reseting ACM successful, but query was not");

		cm->flags &= ~CALL_METER_FLAG_CACHED;

		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		return;
	}

	reply = dbus_message_new_method_return(cm->pending);
	__ofono_dbus_pending_reply(&cm->pending, reply);

	set_acm(cm, value);
}

static void acm_reset_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_meter *cm = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("reseting acm failed");
		__ofono_dbus_pending_reply(&cm->pending,
					__ofono_error_failed(cm->pending));
		check_pin2_state(cm);
		return;
	}

	/* Assume if we have acm_reset, we have acm_query */
	cm->driver->acm_query(cm, reset_acm_query_callback, cm);
}

static DBusMessage *cm_acm_reset(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_meter *cm = data;
	const char *pin2;

	if (cm->driver->acm_reset == NULL)
		return __ofono_error_not_implemented(msg);

	if (cm->pending)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pin2,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_is_valid_sim_pin(pin2, OFONO_SIM_PASSWORD_SIM_PIN2))
		return __ofono_error_invalid_format(msg);

	cm->pending = dbus_message_ref(msg);

	cm->driver->acm_reset(cm, pin2, acm_reset_callback, cm);

	return NULL;
}

static GDBusMethodTable cm_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cm_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SetProperty",	"svs",	"",		cm_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "Reset", 		"s",	"",		cm_acm_reset,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cm_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ "NearMaximumWarning",	"" },
	{ }
};

void ofono_call_meter_changed_notify(struct ofono_call_meter *cm, int new_value)
{
	set_call_meter(cm, new_value);
}

void ofono_call_meter_maximum_notify(struct ofono_call_meter *cm)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cm->atom);

	g_dbus_emit_signal(conn, path, OFONO_CALL_METER_INTERFACE,
			"NearMaximumWarning", DBUS_TYPE_INVALID);
}

int ofono_call_meter_driver_register(const struct ofono_call_meter_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_call_meter_driver_unregister(const struct ofono_call_meter_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void call_meter_unregister(struct ofono_atom *atom)
{
	struct ofono_call_meter *cm = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(cm->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cm->atom);

	ofono_modem_remove_interface(modem, OFONO_CALL_METER_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_CALL_METER_INTERFACE);
}

static void call_meter_remove(struct ofono_atom *atom)
{
	struct ofono_call_meter *cm = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cm == NULL)
		return;

	if (cm->driver && cm->driver->remove)
		cm->driver->remove(cm);

	g_free(cm);
}

struct ofono_call_meter *ofono_call_meter_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data)
{
	struct ofono_call_meter *cm;
	GSList *l;

	if (driver == NULL)
		return NULL;

	cm = g_try_new0(struct ofono_call_meter, 1);

	if (cm == NULL)
		return NULL;

	cm->atom = __ofono_modem_add_atom(modem,
						OFONO_ATOM_TYPE_CALL_METER,
						call_meter_remove, cm);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_call_meter_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cm, vendor, data) < 0)
			continue;

		cm->driver = drv;
		break;
	}

	return cm;
}

void ofono_call_meter_register(struct ofono_call_meter *cm)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cm->atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(cm->atom);

	if (!g_dbus_register_interface(conn, path, OFONO_CALL_METER_INTERFACE,
					cm_methods, cm_signals, NULL, cm,
					NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CALL_METER_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_CALL_METER_INTERFACE);

	__ofono_atom_register(cm->atom, call_meter_unregister);
}

void ofono_call_meter_remove(struct ofono_call_meter *cm)
{
	__ofono_atom_free(cm->atom);
}

void ofono_call_meter_set_data(struct ofono_call_meter *cm, void *data)
{
	cm->driver_data = data;
}

void *ofono_call_meter_get_data(struct ofono_call_meter *cm)
{
	return cm->driver_data;
}
