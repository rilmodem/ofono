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
#include <stdlib.h>
#include <time.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "cssn.h"
#include "ussd.h"

#define CALL_BARRING_INTERFACE "org.ofono.CallBarring"

#define CALL_BARRING_FLAG_CACHED 0x1

static void cb_ss_query_next_lock(struct ofono_modem *modem);
static void get_query_next_lock(struct ofono_modem *modem);
static void set_query_next_lock(struct ofono_modem *modem);

struct call_barring_data {
	struct ofono_call_barring_ops *ops;
	int flags;
	DBusMessage *pending;
	int *cur_locks;
	int *new_locks;
	int query_start;
	int query_end;
	int query_next;
	int ss_req_type;
	int ss_req_cls;
	int ss_req_lock;
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

static inline void emit_barring_changed(struct ofono_modem *modem, int start,
					int end, const char *type, int cls)
{
	struct call_barring_data *cb = modem->call_barring;
	DBusConnection *conn = ofono_dbus_get_connection();
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

	ofono_dbus_signal_property_changed(conn, modem->path,
						CALL_BARRING_INTERFACE,
						property_name, DBUS_TYPE_STRING,
						&value);
}

static void update_barrings(struct ofono_modem *modem, int mask)
{
	struct call_barring_data *cb = modem->call_barring;
	int cls;
	int i;

	/* We're only interested in emitting signals for Voice, Fax & Data */
	for (cls = 1; cls <= BEARER_CLASS_PAD; cls = cls << 1) {
		if ((cls & mask) == 0)
			continue;

		emit_barring_changed(modem, cb->query_start, CB_OUTGOING_END,
					"Outgoing", cls);
		emit_barring_changed(modem, CB_INCOMING_START, cb->query_end,
					"Incoming", cls);
	}

	for (i = cb->query_start; i <= cb->query_end; i++) {
		cb->cur_locks[i] = cb->new_locks[i];
		cb->new_locks[i] = 0;
	}
}

static void cb_ss_property_append(struct call_barring_data *cb,
					DBusMessageIter *dict, int lock,
					int mask)
{
	int i;
	char property_name[64];
	const char *strvalue;

	for (i = 1; i <= BEARER_CLASS_PAD; i = i << 1) {
		if (!(mask & i))
			continue;

		strvalue = (cb->cur_locks[lock] & i) ? "enabled" : "disabled";

		snprintf(property_name, sizeof(property_name), "%s%s",
				bearer_class_to_string(i),
				cb_locks[lock].name);

		ofono_dbus_dict_append(dict, property_name, DBUS_TYPE_STRING,
					&strvalue);
	}
}

static void cb_set_query_bounds(struct call_barring_data *cb,
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

static void generate_ss_query_reply(struct ofono_modem *modem)
{
	struct call_barring_data *cb = modem->call_barring;
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
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		if (cb->ss_req_type != SS_CONTROL_TYPE_QUERY)
			ofono_error("Enabling/disabling Call Barring via SS "
					"successful, but query was not");

		cb->flags &= ~CALL_BARRING_FLAG_CACHED;

		__ofono_dbus_pending_reply(&cb->pending,
					__ofono_error_failed(cb->pending));
		return;
	}

	cb->new_locks[cb->query_next] = status;

	if (cb->query_next < cb->query_end) {
		cb->query_next += 1;
		cb_ss_query_next_lock(modem);
		return;
	}

	generate_ss_query_reply(modem);
	update_barrings(modem, BEARER_CLASS_VOICE);
}

static void cb_ss_query_next_lock(struct ofono_modem *modem)
{
	struct call_barring_data *cb = modem->call_barring;
	int cls;

	cls = cb->ss_req_cls | BEARER_CLASS_DEFAULT;

	cb->ops->query(modem, cb_locks[cb->query_next].fac, cls,
			cb_ss_query_next_lock_callback, modem);
}

static void cb_ss_set_lock_callback(const struct ofono_error *error,
		void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Enabling/disabling Call Barring via SS failed");
		__ofono_dbus_pending_reply(&cb->pending,
					__ofono_error_failed(cb->pending));
		return;
	}

	/* Assume we have query always */
	cb_ss_query_next_lock(modem);
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

static gboolean cb_ss_control(struct ofono_modem *modem,
				enum ss_control_type type, const char *sc,
				const char *sia, const char *sib,
				const char *sic, const char *dn,
				DBusMessage *msg)
{
	struct call_barring_data *cb = modem->call_barring;
	DBusConnection *conn = ofono_dbus_get_connection();
	int cls = BEARER_CLASS_DEFAULT;
	const char *fac;
	DBusMessage *reply;
	void *operation = NULL;
	int i;

	if (cb->pending) {
		reply = __ofono_error_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	ofono_debug("Received call barring ss control request");

	ofono_debug("type: %d, sc: %s, sia: %s, sib: %s, sic: %s, dn: %s",
			type, sc, sia, sib, sic, dn);

	fac = cb_ss_service_to_fac(sc);
	if (!fac)
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

	if (!is_valid_pin(sia))
		goto bad_format;

	switch (type) {
	case SS_CONTROL_TYPE_ACTIVATION:
	case SS_CONTROL_TYPE_DEACTIVATION:
	case SS_CONTROL_TYPE_REGISTRATION:
	case SS_CONTROL_TYPE_ERASURE:
		operation = cb->ops->set;
		break;
	case SS_CONTROL_TYPE_QUERY:
		operation = cb->ops->query;
		break;
	}

	if (!operation) {
		reply = __ofono_error_not_implemented(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	/* According to 27.007, AG, AC and AB only work with mode = 0
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
		cb->ss_req_type = SS_CONTROL_TYPE_REGISTRATION;
		cb->ops->set(modem, fac, 1, sia, cls,
				cb_ss_set_lock_callback, modem);
		break;
	case SS_CONTROL_TYPE_ERASURE:
	case SS_CONTROL_TYPE_DEACTIVATION:
		cb->ss_req_type = SS_CONTROL_TYPE_ERASURE;
		cb->ops->set(modem, fac, 0, sia, cls,
				cb_ss_set_lock_callback, modem);
		break;
	case SS_CONTROL_TYPE_QUERY:
		cb->ss_req_type = SS_CONTROL_TYPE_QUERY;
		cb_ss_query_next_lock(modem);
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
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(cb->pending);
	else {
		reply = __ofono_error_failed(cb->pending);
		ofono_debug("Changing Call Barring password via SS failed");
	}

	__ofono_dbus_pending_reply(&cb->pending, reply);
}

static gboolean cb_ss_passwd(struct ofono_modem *modem, const char *sc,
				const char *old, const char *new,
				DBusMessage *msg)
{
	struct call_barring_data *cb = modem->call_barring;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	const char *fac;

	if (cb->pending) {
		reply = __ofono_error_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	ofono_debug("Received call barring ss password change request");

	ofono_debug("sc: %s", sc);

	if (!strcmp(sc, ""))
		fac = "AB";
	else
		fac = cb_ss_service_to_fac(sc);

	if (!fac)
		return FALSE;

	if (!is_valid_pin(old) || !is_valid_pin(new))
		goto bad_format;

	cb->pending = dbus_message_ref(msg);
	cb->ops->set_passwd(modem, fac, old, new,
			cb_set_passwd_callback, modem);

	return TRUE;
bad_format:
	reply = __ofono_error_invalid_format(msg);
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
	ss_control_register(modem, "353", cb_ss_control);
	ss_passwd_register(modem, "", cb_ss_passwd);
	ss_passwd_register(modem, "33", cb_ss_passwd);
	ss_passwd_register(modem, "331", cb_ss_passwd);
	ss_passwd_register(modem, "332", cb_ss_passwd);
	ss_passwd_register(modem, "35", cb_ss_passwd);
	ss_passwd_register(modem, "351", cb_ss_passwd);
	ss_passwd_register(modem, "330", cb_ss_passwd);
	ss_passwd_register(modem, "333", cb_ss_passwd);
	ss_passwd_register(modem, "353", cb_ss_passwd);
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
	ss_control_unregister(modem, "353", cb_ss_control);
	ss_passwd_unregister(modem, "", cb_ss_passwd);
	ss_passwd_unregister(modem, "33", cb_ss_passwd);
	ss_passwd_unregister(modem, "331", cb_ss_passwd);
	ss_passwd_unregister(modem, "332", cb_ss_passwd);
	ss_passwd_unregister(modem, "35", cb_ss_passwd);
	ss_passwd_unregister(modem, "351", cb_ss_passwd);
	ss_passwd_unregister(modem, "330", cb_ss_passwd);
	ss_passwd_unregister(modem, "333", cb_ss_passwd);
	ss_passwd_unregister(modem, "353", cb_ss_passwd);
}

static struct call_barring_data *call_barring_create(void)
{
	int lcount;
	struct call_barring_data *cb = g_new0(struct call_barring_data, 1);

	lcount = CB_ALL_END - CB_ALL_START + 1;

	cb->cur_locks = g_new0(int, lcount);
	cb->new_locks = g_new0(int, lcount);

	return cb;
}

static void call_barring_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct call_barring_data *cb = modem->call_barring;

	g_free(cb->cur_locks);
	g_free(cb->new_locks);
	g_free(cb);

	modem->call_barring = NULL;
}

static inline void cb_append_property(struct call_barring_data *cb,
					DBusMessageIter *dict, int start,
					int end, int cls, const char *property)
{
	char property_name[64];
	const char *value = "disabled";
	int i;

	for (i = start; i <= end; i++)
		if (cb->cur_locks[i] & cls)
			break;

	if (i <= end)
		value = cb_locks[i].value;

	snprintf(property_name, sizeof(property_name), "%s%s",
			bearer_class_to_string(cls), property);

	ofono_dbus_dict_append(dict, property_name, DBUS_TYPE_STRING,
				&value);
}

static void cb_get_properties_reply(struct ofono_modem *modem, int mask)
{
	struct call_barring_data *cb = modem->call_barring;
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	int j;

	if (!(cb->flags & CALL_BARRING_FLAG_CACHED))
		ofono_error("Generating a get_properties reply with no cache");

	reply = dbus_message_new_method_return(cb->pending);
	if (!reply)
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
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		cb->new_locks[cb->query_next] = status;

		if (cb->query_next == CB_ALL_END)
			cb->flags |= CALL_BARRING_FLAG_CACHED;
	}

	if (cb->query_next < CB_ALL_END) {
		cb->query_next = cb->query_next + 1;
		get_query_next_lock(modem);
		return;
	}

	cb_get_properties_reply(modem, BEARER_CLASS_VOICE);
	update_barrings(modem, BEARER_CLASS_VOICE);
}

static void get_query_next_lock(struct ofono_modem *modem)
{
	struct call_barring_data *cb = modem->call_barring;

	cb->ops->query(modem, cb_locks[cb->query_next].fac,
			BEARER_CLASS_DEFAULT, get_query_lock_callback, modem);
}

static DBusMessage *cb_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (cb->pending)
		return __ofono_error_busy(msg);

	if (!cb->ops->query)
		return __ofono_error_not_implemented(msg);

	cb->pending = dbus_message_ref(msg);

	if (cb->flags & CALL_BARRING_FLAG_CACHED)
		cb_get_properties_reply(modem, BEARER_CLASS_VOICE);
	else {
		cb->query_next = CB_ALL_START;
		get_query_next_lock(modem);
	}

	return NULL;
}

static void set_query_lock_callback(const struct ofono_error *error,
				int status, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

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
		set_query_next_lock(modem);
		return;
	}

	__ofono_dbus_pending_reply(&cb->pending,
				dbus_message_new_method_return(cb->pending));
	update_barrings(modem, BEARER_CLASS_VOICE);
}

static void set_query_next_lock(struct ofono_modem *modem)
{
	struct call_barring_data *cb = modem->call_barring;

	cb->ops->query(modem, cb_locks[cb->query_next].fac,
			BEARER_CLASS_DEFAULT, set_query_lock_callback, modem);
}

static void set_lock_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Enabling/disabling a lock failed");
		__ofono_dbus_pending_reply(&cb->pending,
					__ofono_error_failed(cb->pending));
		return;
	}

	/* If we successfully set the value, we must query it back
	 * Call Barring is a special case, since according to 22.088 2.2.1:
	 * "The PLMN will ensure that only one of the barring programs is
	 * active per basic service group. The activation of one specific
	 * barring program will override an already active one (i.e. the
	 * old one will be permanently deactivated)."
	 * So we actually query all outgoing / incoming barrings depending
	 * on what kind we set.
	 */
	set_query_next_lock(modem);
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
	} else
		return FALSE;

	/* Gah, this is a special case.  If we're setting a barring to
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
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name, *passwd = "";
	const char *value;
	int lock;
	int cls;
	int mode;

	if (cb->pending)
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
		if (!is_valid_pin(passwd))
			return __ofono_error_invalid_format(msg);
	}

	if (!cb->ops->set)
		return __ofono_error_not_implemented(msg);

	cb_set_query_bounds(cb, cb_locks[lock].fac, FALSE);

	cb->pending = dbus_message_ref(msg);
	cb->ops->set(modem, cb_locks[lock].fac, mode, passwd, cls,
			set_lock_callback, modem);

	return NULL;
}

static void disable_all_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Disabling all barring failed");
		__ofono_dbus_pending_reply(&cb->pending,
					__ofono_error_failed(cb->pending));
		return;
	}

	/* Assume if we have set, we have query */
	set_query_next_lock(modem);
}

static DBusMessage *cb_disable_all(DBusConnection *conn, DBusMessage *msg,
					void *data, const char *fac)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessageIter iter;
	const char *passwd = "";

	if (cb->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &passwd);
	if (!is_valid_pin(passwd))
		return __ofono_error_invalid_format(msg);

	if (!cb->ops->set)
		return __ofono_error_not_implemented(msg);

	cb_set_query_bounds(cb, fac, FALSE);

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

static DBusMessage *cb_set_passwd(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_barring_data *cb = modem->call_barring;
	DBusMessageIter iter;
	const char *old_passwd, *new_passwd;

	if (cb->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &old_passwd);
	if (!is_valid_pin(old_passwd))
		return __ofono_error_invalid_format(msg);

	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &new_passwd);
	if (!is_valid_pin(new_passwd))
		return __ofono_error_invalid_format(msg);

	if (!cb->ops->set_passwd)
		return __ofono_error_not_implemented(msg);

	cb->pending = dbus_message_ref(msg);
	cb->ops->set_passwd(modem, "AB", old_passwd, new_passwd,
			cb_set_passwd_callback, modem);

	return NULL;
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
	{ "ChangePassword",	"ss",	"",		cb_set_passwd,
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
	DBusConnection *conn = ofono_dbus_get_connection();
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
	DBusConnection *conn = ofono_dbus_get_connection();
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
	DBusConnection *conn = ofono_dbus_get_connection();

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

	ofono_modem_add_interface(modem, CALL_BARRING_INTERFACE);

	cb_register_ss_controls(modem);

	ofono_mo_ss_register(modem, SS_MO_INCOMING_BARRING,
			call_barring_incoming_enabled_notify, modem);
	ofono_mo_ss_register(modem, SS_MO_OUTGOING_BARRING,
			call_barring_outgoing_enabled_notify, modem);

	return 0;
}

void ofono_call_barring_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (!modem->call_barring)
		return;

	ofono_modem_remove_interface(modem, CALL_BARRING_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path, CALL_BARRING_INTERFACE);

	cb_unregister_ss_controls(modem);

	ofono_mo_ss_unregister(modem, SS_MO_INCOMING_BARRING,
			call_barring_incoming_enabled_notify, modem);
	ofono_mo_ss_unregister(modem, SS_MO_OUTGOING_BARRING,
			call_barring_outgoing_enabled_notify, modem);

	modem->call_barring = NULL;
}
