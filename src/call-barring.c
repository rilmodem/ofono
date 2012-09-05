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
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"

#define CALL_BARRING_FLAG_CACHED 0x1
#define NUM_OF_BARRINGS 5

static GSList *g_drivers = NULL;

static void cb_ss_query_next_lock(struct ofono_call_barring *cb);
static void get_query_next_lock(struct ofono_call_barring *cb);
static void set_query_next_lock(struct ofono_call_barring *cb);

struct ofono_call_barring {
	int flags;
	DBusMessage *pending;
	int cur_locks[NUM_OF_BARRINGS];
	int new_locks[NUM_OF_BARRINGS];
	int query_start;
	int query_end;
	int query_next;
	int ss_req_type;
	int ss_req_cls;
	int ss_req_lock;
	struct ofono_ussd *ussd;
	unsigned int ussd_watch;
	const struct ofono_call_barring_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct call_barring_lock {
	const char *name;
	const char *value;
	const char *fac;
};

static struct call_barring_lock cb_locks[] = {
	{ "AllOutgoing",			"all",			"AO" },
	{ "InternationalOutgoing",		"international",	"OI" },
	{ "InternationalOutgoingExceptHome",	"internationalnothome",	"OX" },
	{ "AllIncoming",			"always",		"AI" },
	{ "IncomingWhenRoaming",		"whenroaming",		"IR" },
	{ "AllBarringServices",			NULL,			"AB" },
	{ "AllOutgoingServices",		NULL,			"AG" },
	{ "AllIncomingServices",		NULL,			"AC" },
	{ NULL,					NULL,			NULL },
};

/* These are inclusive */
#define CB_OUTGOING_START 0
#define CB_OUTGOING_END 2
#define CB_INCOMING_START 3
#define CB_INCOMING_END 4
#define CB_ALL_START 0
#define CB_ALL_END 4
#define CB_ALL_OUTGOING 6
#define CB_ALL_INCOMING 7

static inline void emit_barring_changed(struct ofono_call_barring *cb,
					int start, int end,
					const char *type, int cls)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cb->atom);
	char property_name[64];
	const char *value;
	int i;
	int j;

	for (i = start; i <= end; i++)
		if (cb->cur_locks[i] & cls)
			break;

	for (j = start; j <= end; j++)
		if (cb->new_locks[j] & cls)
			break;

	if (i == j)
		return;

	if (j > end)
		value = "disabled";
	else
		value = cb_locks[j].value;

	snprintf(property_name, sizeof(property_name), "%s%s",
			bearer_class_to_string(cls), type);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_BARRING_INTERFACE,
						property_name, DBUS_TYPE_STRING,
						&value);
}

static void update_barrings(struct ofono_call_barring *cb, int mask)
{
	int cls;
	int i;

	/* We're only interested in emitting signals for Voice, Fax & Data */
	for (cls = 1; cls <= BEARER_CLASS_PAD; cls = cls << 1) {
		if ((cls & mask) == 0)
			continue;

		emit_barring_changed(cb, cb->query_start, CB_OUTGOING_END,
					"Outgoing", cls);
		emit_barring_changed(cb, CB_INCOMING_START, cb->query_end,
					"Incoming", cls);
	}

	for (i = cb->query_start; i <= cb->query_end; i++)
		cb->cur_locks[i] = cb->new_locks[i];
}

static void cb_ss_property_append(struct ofono_call_barring *cb,
					DBusMessageIter *dict, int lock,
					int mask)
{
	int i;
	char property_name[64];
	const char *strvalue;

	for (i = 1; i <= BEARER_CLASS_PAD; i = i << 1) {
		if (!(mask & i))
			continue;

		strvalue = (cb->new_locks[lock] & i) ? "enabled" : "disabled";

		snprintf(property_name, sizeof(property_name), "%s%s",
				bearer_class_to_string(i),
				cb_locks[lock].name);

		ofono_dbus_dict_append(dict, property_name, DBUS_TYPE_STRING,
					&strvalue);
	}
}

static void cb_set_query_bounds(struct ofono_call_barring *cb,
				const char *fac, gboolean fac_only)
{
	int i;

	if (!strcmp("AB", fac)) {
		cb->query_start = CB_ALL_START;
		cb->query_end = CB_ALL_END;
		cb->query_next = CB_ALL_START;
		return;
	}

	if (!strcmp("AG", fac))
		goto outgoing;

	if (!strcmp("AC", fac))
		goto incoming;

	for (i = 0; cb_locks[i].name; i++) {
		if (strcmp(cb_locks[i].fac, fac))
			continue;

		if (fac_only) {
			cb->query_start = i;
			cb->query_end = i;
			cb->query_next = i;
			return;
		}

		if ((i >= CB_OUTGOING_START) &&
			(i <= CB_OUTGOING_END))
			goto outgoing;
		else if ((i >= CB_INCOMING_START) &&
				(i <= CB_INCOMING_END))
			goto incoming;
	}

	ofono_error("Unable to set query boundaries for %s", fac);
	return;

outgoing:
	cb->query_start = CB_OUTGOING_START;
	cb->query_end = CB_OUTGOING_END;
	cb->query_next = CB_OUTGOING_START;
	return;

incoming:
	cb->query_start = CB_INCOMING_START;
	cb->query_end = CB_INCOMING_END;
	cb->query_next = CB_INCOMING_START;
	return;
}

static void generate_ss_query_reply(struct ofono_call_barring *cb)
{
	const char *context = "CallBarring";
	const char *sig = "(ssa{sv})";
	const char *ss_type = ss_control_type_to_string(cb->ss_req_type);
	const char *ss_fac = cb_locks[cb->ss_req_lock].name;
	DBusMessageIter iter;
	DBusMessageIter variant;
	DBusMessageIter vstruct;
	DBusMessageIter dict;
	DBusMessage *reply;
	int lock;
	int start, end;

	reply = dbus_message_new_method_return(cb->pending);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &context);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, sig,
						&variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, NULL,
						&vstruct);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING, &ss_type);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING, &ss_fac);

	dbus_message_iter_open_container(&vstruct, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	/* We report all affected locks only for the special case ones */
	if (cb->ss_req_lock <= CB_ALL_END) {
		start = cb->ss_req_lock;
		end = cb->ss_req_lock;
	} else {
		start = cb->query_start;
		end = cb->query_end;
	}

	for (lock = start; lock <= end; lock++)
		cb_ss_property_append(cb, &dict, lock, cb->ss_req_cls);

	dbus_message_iter_close_container(&vstruct, &dict);

	dbus_message_iter_close_container(&variant, &vstruct);

	dbus_message_iter_close_container(&iter, &variant);

	__ofono_dbus_pending_reply(&cb->pending, reply);
}

static void cb_ss_query_next_lock_callback(const struct ofono_error *error,
					int status, void *data)
{
	struct ofono_call_barring *cb = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Query failed with error: %s",
						telephony_error_to_str(error));

		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

		__ofono_dbus_pending_reply(&cb->pending,
				__ofono_error_from_error(error, cb->pending));
		return;
	}

	cb->new_locks[cb->query_next] = status;

	if (cb->query_next < cb->query_end) {
		cb->query_next += 1;
		cb_ss_query_next_lock(cb);
		return;
	}

	generate_ss_query_reply(cb);
	update_barrings(cb, BEARER_CLASS_VOICE);
}

static void cb_ss_query_next_lock(struct ofono_call_barring *cb)
{
	int cls;

	cls = (cb->ss_req_type == SS_CONTROL_TYPE_QUERY) ?
			cb->ss_req_cls : cb->ss_req_cls | BEARER_CLASS_DEFAULT;

	cb->driver->query(cb, cb_locks[cb->query_next].fac, cls,
			cb_ss_query_next_lock_callback, cb);
}

static void cb_ss_set_lock_callback(const struct ofono_error *error,
		void *data)
{
	struct ofono_call_barring *cb = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Enabling/disabling Call Barring via SS failed with err:%s",
			telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&cb->pending,
			__ofono_error_from_error(error, cb->pending));
		return;
	}

	/* Assume we have query always */
	cb_ss_query_next_lock(cb);
}

static const char *cb_ss_service_to_fac(const char *svc)
{
	if (!strcmp(svc, "33"))
		return "AO";
	else if (!strcmp(svc, "331"))
		return "OI";
	else if (!strcmp(svc, "332"))
		return "OX";
	else if (!strcmp(svc, "35"))
		return "AI";
	else if (!strcmp(svc, "351"))
		return "IR";
	else if (!strcmp(svc, "330"))
		return "AB";
	else if (!strcmp(svc, "333"))
		return "AG";
	else if (!strcmp(svc, "353"))
		return "AC";

	return NULL;
}

static gboolean cb_ss_control(int type, const char *sc,
				const char *sia, const char *sib,
				const char *sic, const char *dn,
				DBusMessage *msg, void *data)
{
	struct ofono_call_barring *cb = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	int cls = BEARER_CLASS_DEFAULT;
	const char *fac;
	DBusMessage *reply;
	void *operation = NULL;
	int i;

	if (__ofono_call_barring_is_busy(cb)) {
		reply = __ofono_error_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	DBG("Received call barring ss control request");

	DBG("type: %d, sc: %s, sia: %s, sib: %s, sic: %s, dn: %s",
		type, sc, sia, sib, sic, dn);

	fac = cb_ss_service_to_fac(sc);
	if (fac == NULL)
		return FALSE;

	cb_set_query_bounds(cb, fac, type == SS_CONTROL_TYPE_QUERY);

	i = 0;
	while (cb_locks[i].name && strcmp(cb_locks[i].fac, fac))
		i++;

	cb->ss_req_lock = i;

	if (strlen(sic) > 0)
		goto bad_format;

	if (strlen(dn) > 0)
		goto bad_format;

	if (type != SS_CONTROL_TYPE_QUERY && !__ofono_is_valid_net_pin(sia))
		goto bad_format;

	switch (type) {
	case SS_CONTROL_TYPE_ACTIVATION:
	case SS_CONTROL_TYPE_DEACTIVATION:
	case SS_CONTROL_TYPE_REGISTRATION:
	case SS_CONTROL_TYPE_ERASURE:
		operation = cb->driver->set;
		break;
	case SS_CONTROL_TYPE_QUERY:
		operation = cb->driver->query;
		break;
	default:
		break;
	}

	if (operation == NULL) {
		reply = __ofono_error_not_implemented(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	/*
	 * According to 27.007, AG, AC and AB only work with mode = 0
	 * We support query by querying all relevant types, since we must
	 * do this for the deactivation case anyway
	 */
	if ((!strcmp(fac, "AG") || !strcmp(fac, "AC") || !strcmp(fac, "AB")) &&
		(type == SS_CONTROL_TYPE_ACTIVATION ||
			type == SS_CONTROL_TYPE_REGISTRATION))
		goto bad_format;

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

	cb->ss_req_cls = cls;
	cb->pending = dbus_message_ref(msg);

	switch (type) {
	case SS_CONTROL_TYPE_ACTIVATION:
	case SS_CONTROL_TYPE_REGISTRATION:
		cb->ss_req_type = SS_CONTROL_TYPE_ACTIVATION;
		cb->driver->set(cb, fac, 1, sia, cls,
				cb_ss_set_lock_callback, cb);
		break;
	case SS_CONTROL_TYPE_ERASURE:
	case SS_CONTROL_TYPE_DEACTIVATION:
		cb->ss_req_type = SS_CONTROL_TYPE_DEACTIVATION;
		cb->driver->set(cb, fac, 0, sia, cls,
				cb_ss_set_lock_callback, cb);
		break;
	case SS_CONTROL_TYPE_QUERY:
		cb->ss_req_type = SS_CONTROL_TYPE_QUERY;
		cb_ss_query_next_lock(cb);
		break;
	}

	return TRUE;

bad_format:
	reply = __ofono_error_invalid_format(msg);
	g_dbus_send_message(conn, reply);
	return TRUE;
}

static void cb_set_passwd_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_barring *cb = data;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(cb->pending);
	else {
		DBG("Changing Call Barring password via SS failed with err: %s",
				telephony_error_to_str(error));
		reply = __ofono_error_from_error(error, cb->pending);
	}

	__ofono_dbus_pending_reply(&cb->pending, reply);
}

static gboolean cb_ss_passwd(const char *sc,
				const char *old, const char *new,
				DBusMessage *msg, void *data)
{
	struct ofono_call_barring *cb = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	const char *fac;

	if (__ofono_call_barring_is_busy(cb)) {
		reply = __ofono_error_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	DBG("Received call barring ss password change request");

	DBG("sc: %s", sc);

	if (!strcmp(sc, ""))
		fac = "AB";
	else
		fac = cb_ss_service_to_fac(sc);

	if (fac == NULL)
		return FALSE;

	if (!__ofono_is_valid_net_pin(old) || !__ofono_is_valid_net_pin(new))
		goto bad_format;

	cb->pending = dbus_message_ref(msg);
	cb->driver->set_passwd(cb, fac, old, new, cb_set_passwd_callback, cb);

	return TRUE;
bad_format:
	reply = __ofono_error_invalid_format(msg);
	g_dbus_send_message(conn, reply);
	return TRUE;
}

static void cb_register_ss_controls(struct ofono_call_barring *cb)
{
	__ofono_ussd_ssc_register(cb->ussd, "33", cb_ss_control, cb, NULL);
	__ofono_ussd_ssc_register(cb->ussd, "331", cb_ss_control, cb, NULL);
	__ofono_ussd_ssc_register(cb->ussd, "332", cb_ss_control, cb, NULL);
	__ofono_ussd_ssc_register(cb->ussd, "35", cb_ss_control, cb, NULL);
	__ofono_ussd_ssc_register(cb->ussd, "351", cb_ss_control, cb, NULL);
	__ofono_ussd_ssc_register(cb->ussd, "330", cb_ss_control, cb, NULL);
	__ofono_ussd_ssc_register(cb->ussd, "333", cb_ss_control, cb, NULL);
	__ofono_ussd_ssc_register(cb->ussd, "353", cb_ss_control, cb, NULL);

	__ofono_ussd_passwd_register(cb->ussd, "", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "33", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "331", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "332", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "35", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "351", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "330", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "333", cb_ss_passwd, cb, NULL);
	__ofono_ussd_passwd_register(cb->ussd, "353", cb_ss_passwd, cb, NULL);
}

static void cb_unregister_ss_controls(struct ofono_call_barring *cb)
{
	__ofono_ussd_ssc_unregister(cb->ussd, "33");
	__ofono_ussd_ssc_unregister(cb->ussd, "331");
	__ofono_ussd_ssc_unregister(cb->ussd, "332");
	__ofono_ussd_ssc_unregister(cb->ussd, "35");
	__ofono_ussd_ssc_unregister(cb->ussd, "351");
	__ofono_ussd_ssc_unregister(cb->ussd, "330");
	__ofono_ussd_ssc_unregister(cb->ussd, "333");
	__ofono_ussd_ssc_unregister(cb->ussd, "353");

	__ofono_ussd_passwd_unregister(cb->ussd, "");
	__ofono_ussd_passwd_unregister(cb->ussd, "33");
	__ofono_ussd_passwd_unregister(cb->ussd, "331");
	__ofono_ussd_passwd_unregister(cb->ussd, "332");
	__ofono_ussd_passwd_unregister(cb->ussd, "35");
	__ofono_ussd_passwd_unregister(cb->ussd, "351");
	__ofono_ussd_passwd_unregister(cb->ussd, "330");
	__ofono_ussd_passwd_unregister(cb->ussd, "333");
	__ofono_ussd_passwd_unregister(cb->ussd, "353");
}

gboolean __ofono_call_barring_is_busy(struct ofono_call_barring *cb)
{
	return cb->pending ? TRUE : FALSE;
}

static inline void cb_append_property(struct ofono_call_barring *cb,
					DBusMessageIter *dict, int start,
					int end, int cls, const char *property)
{
	char property_name[64];
	const char *value = "disabled";
	int i;

	for (i = start; i <= end; i++)
		if (cb->new_locks[i] & cls)
			break;

	if (i <= end)
		value = cb_locks[i].value;

	snprintf(property_name, sizeof(property_name), "%s%s",
			bearer_class_to_string(cls), property);

	ofono_dbus_dict_append(dict, property_name, DBUS_TYPE_STRING,
				&value);
}

static void cb_get_properties_reply(struct ofono_call_barring *cb, int mask)
{
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	int j;

	if (!(cb->flags & CALL_BARRING_FLAG_CACHED))
		ofono_error("Generating a get_properties reply with no cache");

	reply = dbus_message_new_method_return(cb->pending);
	if (reply == NULL)
		return;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	for (j = 1; j <= BEARER_CLASS_PAD; j = j << 1) {
		if ((j & mask) == 0)
			continue;

		cb_append_property(cb, &dict, CB_OUTGOING_START,
					CB_OUTGOING_END, j, "Outgoing");
		cb_append_property(cb, &dict, CB_INCOMING_START,
					CB_INCOMING_END, j, "Incoming");
	}

	dbus_message_iter_close_container(&iter, &dict);

	__ofono_dbus_pending_reply(&cb->pending, reply);
}

static void get_query_lock_callback(const struct ofono_error *error,
					int status, void *data)
{
	struct ofono_call_barring *cb = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		cb->new_locks[cb->query_next] = status;

		if (cb->query_next == CB_ALL_END)
			cb->flags |= CALL_BARRING_FLAG_CACHED;
	}

	if (cb->query_next < CB_ALL_END) {
		cb->query_next = cb->query_next + 1;
		get_query_next_lock(cb);
		return;
	}

	cb_get_properties_reply(cb, BEARER_CLASS_VOICE);
	update_barrings(cb, BEARER_CLASS_VOICE);
}

static void get_query_next_lock(struct ofono_call_barring *cb)
{
	cb->driver->query(cb, cb_locks[cb->query_next].fac,
			BEARER_CLASS_DEFAULT, get_query_lock_callback, cb);
}

static DBusMessage *cb_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_barring *cb = data;

	if (__ofono_call_barring_is_busy(cb) || __ofono_ussd_is_busy(cb->ussd))
		return __ofono_error_busy(msg);

	if (cb->driver->query == NULL)
		return __ofono_error_not_implemented(msg);

	cb->pending = dbus_message_ref(msg);

	if (cb->flags & CALL_BARRING_FLAG_CACHED)
		cb_get_properties_reply(cb, BEARER_CLASS_VOICE);
	else {
		cb->query_next = CB_ALL_START;
		get_query_next_lock(cb);
	}

	return NULL;
}

static void set_query_lock_callback(const struct ofono_error *error,
				int status, void *data)
{
	struct ofono_call_barring *cb = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Disabling all barring successful, "
				"but query was not");

		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

		__ofono_dbus_pending_reply(&cb->pending,
					__ofono_error_failed(cb->pending));
		return;
	}

	cb->new_locks[cb->query_next] = status;

	if (cb->query_next < cb->query_end) {
		cb->query_next += 1;
		set_query_next_lock(cb);
		return;
	}

	__ofono_dbus_pending_reply(&cb->pending,
				dbus_message_new_method_return(cb->pending));
	update_barrings(cb, BEARER_CLASS_VOICE);
}

static void set_query_next_lock(struct ofono_call_barring *cb)
{
	cb->driver->query(cb, cb_locks[cb->query_next].fac,
			BEARER_CLASS_DEFAULT, set_query_lock_callback, cb);
}

static void set_lock_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_barring *cb = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Enabling/disabling a lock failed");
		__ofono_dbus_pending_reply(&cb->pending,
					__ofono_error_failed(cb->pending));
		return;
	}

	/*
	 * If we successfully set the value, we must query it back
	 * Call Barring is a special case, since according to 22.088 2.2.1:
	 * "The PLMN will ensure that only one of the barring programs is
	 * active per basic service group. The activation of one specific
	 * barring program will override an already active one (i.e. the
	 * old one will be permanently deactivated)."
	 * So we actually query all outgoing / incoming barrings depending
	 * on what kind we set.
	 */
	set_query_next_lock(cb);
}

static gboolean cb_lock_property_lookup(const char *property, const char *value,
					int mask, int *out_which, int *out_cls,
					int *out_mode)
{
	int i, j;
	const char *prefix;
	size_t len;
	int start, end;

	for (i = 1; i <= BEARER_CLASS_PAD; i = i << 1) {
		if ((i & mask) == 0)
			continue;

		prefix = bearer_class_to_string(i);
		len = strlen(prefix);

		if (!strncmp(property, prefix, len))
			break;
	}

	if (i > BEARER_CLASS_PAD)
		return FALSE;

	property += len;

	if (!strcmp(property, "Outgoing")) {
		start = CB_OUTGOING_START;
		end = CB_OUTGOING_END;
	} else if (!strcmp(property, "Incoming")) {
		start = CB_INCOMING_START;
		end = CB_INCOMING_END;
	} else {
		return FALSE;
	}

	/*
	 * Gah, this is a special case.  If we're setting a barring to
	 * disabled, then generate a disable all outgoing/incoming
	 * request for a particular basic service
	 */
	if (!strcmp(value, "disabled")) {
		*out_mode = 0;
		*out_cls = i;

		if (!strcmp(property, "Outgoing"))
			*out_which = CB_ALL_OUTGOING;
		else
			*out_which = CB_ALL_INCOMING;

		return TRUE;
	}

	for (j = start; j <= end; j++) {
		if (strcmp(value, cb_locks[j].value))
			continue;

		*out_mode = 1;
		*out_cls = i;
		*out_which = j;

		return TRUE;
	}

	return FALSE;
}

static DBusMessage *cb_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_barring *cb = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name, *passwd = "";
	const char *value;
	int lock;
	int cls;
	int mode;

	if (__ofono_call_barring_is_busy(cb) || __ofono_ussd_is_busy(cb->ussd))
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

	if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&var, &value);

	if (!cb_lock_property_lookup(name, value, BEARER_CLASS_VOICE,
					&lock, &cls, &mode))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_next(&iter)) {
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&iter, &passwd);
		if (!__ofono_is_valid_net_pin(passwd))
			return __ofono_error_invalid_format(msg);
	}

	if (cb->driver->set == NULL)
		return __ofono_error_not_implemented(msg);

	cb_set_query_bounds(cb, cb_locks[lock].fac, FALSE);

	cb->pending = dbus_message_ref(msg);
	cb->driver->set(cb, cb_locks[lock].fac, mode, passwd, cls,
			set_lock_callback, cb);

	return NULL;
}

static void disable_all_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_barring *cb = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Disabling all barring failed");
		__ofono_dbus_pending_reply(&cb->pending,
					__ofono_error_failed(cb->pending));
		return;
	}

	/* Assume if we have set, we have query */
	set_query_next_lock(cb);
}

static DBusMessage *cb_disable_all(DBusConnection *conn, DBusMessage *msg,
					void *data, const char *fac)
{
	struct ofono_call_barring *cb = data;
	const char *passwd;

	if (cb->driver->set == NULL)
		return __ofono_error_not_implemented(msg);

	if (__ofono_call_barring_is_busy(cb) || __ofono_ussd_is_busy(cb->ussd))
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &passwd,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_is_valid_net_pin(passwd))
		return __ofono_error_invalid_format(msg);

	cb_set_query_bounds(cb, fac, FALSE);

	cb->pending = dbus_message_ref(msg);
	cb->driver->set(cb, fac, 0, passwd,
			BEARER_CLASS_DEFAULT, disable_all_callback, cb);

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

static DBusMessage *cb_set_passwd(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_barring *cb = data;
	const char *old_passwd;
	const char *new_passwd;

	if (cb->driver->set_passwd == NULL)
		return __ofono_error_not_implemented(msg);

	if (__ofono_call_barring_is_busy(cb) || __ofono_ussd_is_busy(cb->ussd))
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &old_passwd,
					DBUS_TYPE_STRING, &new_passwd,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_is_valid_net_pin(old_passwd))
		return __ofono_error_invalid_format(msg);

	if (!__ofono_is_valid_net_pin(new_passwd))
		return __ofono_error_invalid_format(msg);

	cb->pending = dbus_message_ref(msg);
	cb->driver->set_passwd(cb, "AB", old_passwd, new_passwd,
			cb_set_passwd_callback, cb);

	return NULL;
}

static const GDBusMethodTable cb_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetProperties",
				NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
				cb_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" },
					{ "value", "v" }, { "pin2", "s" }),
			NULL, cb_set_property) },
	{ GDBUS_ASYNC_METHOD("DisableAll", GDBUS_ARGS({ "password", "s" }),
			NULL, cb_disable_ab) },
	{ GDBUS_ASYNC_METHOD("DisableAllIncoming",
			GDBUS_ARGS({ "password", "s" }), NULL,
			cb_disable_ac) },
	{ GDBUS_ASYNC_METHOD("DisableAllOutgoing",
			GDBUS_ARGS({ "password", "s" }), NULL,
			cb_disable_ag) },
	{ GDBUS_ASYNC_METHOD("ChangePassword",
			GDBUS_ARGS({ "old", "s" }, { "new", "s" }),
			NULL, cb_set_passwd) },
	{ }
};

static const GDBusSignalTable cb_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

int ofono_call_barring_driver_register(const struct ofono_call_barring_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_call_barring_driver_unregister(const struct ofono_call_barring_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void call_barring_unregister(struct ofono_atom *atom)
{
	struct ofono_call_barring *cb = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(cb->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cb->atom);

	ofono_modem_remove_interface(modem, OFONO_CALL_BARRING_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_CALL_BARRING_INTERFACE);

	if (cb->ussd)
		cb_unregister_ss_controls(cb);

	if (cb->ussd_watch)
		__ofono_modem_remove_atom_watch(modem, cb->ussd_watch);
}

static void call_barring_remove(struct ofono_atom *atom)
{
	struct ofono_call_barring *cb = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cb == NULL)
		return;

	if (cb->driver != NULL && cb->driver->remove != NULL)
		cb->driver->remove(cb);

	g_free(cb);
}

struct ofono_call_barring *ofono_call_barring_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data)
{
	struct ofono_call_barring *cb;
	GSList *l;

	if (driver == NULL)
		return NULL;

	cb = g_try_new0(struct ofono_call_barring, 1);

	if (cb == NULL)
		return NULL;

	cb->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_CALL_BARRING,
						call_barring_remove, cb);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_call_barring_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cb, vendor, data) < 0)
			continue;

		cb->driver = drv;
		break;
	}

	return cb;
}

static void ussd_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *data)
{
	struct ofono_call_barring *cb = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		cb->ussd = NULL;
		return;
	}

	cb->ussd = __ofono_atom_get_data(atom);
	cb_register_ss_controls(cb);
}

void ofono_call_barring_register(struct ofono_call_barring *cb)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cb->atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(cb->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CALL_BARRING_INTERFACE,
					cb_methods, cb_signals, NULL, cb,
					NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CALL_BARRING_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_CALL_BARRING_INTERFACE);

	cb->ussd_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_USSD,
					ussd_watch, cb, NULL);

	__ofono_atom_register(cb->atom, call_barring_unregister);
}

void ofono_call_barring_remove(struct ofono_call_barring *cb)
{
	__ofono_atom_free(cb->atom);
}

void ofono_call_barring_set_data(struct ofono_call_barring *cb, void *data)
{
	cb->driver_data = data;
}

void *ofono_call_barring_get_data(struct ofono_call_barring *cb)
{
	return cb->driver_data;
}
