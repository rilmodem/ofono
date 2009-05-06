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

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "dbus-gsm.h"
#include "modem.h"
#include "driver.h"
#include "common.h"
#include "ussd.h"

#define CALL_FORWARDING_INTERFACE "org.ofono.CallForwarding"

#define CALL_FORWARDING_FLAG_CACHED 0x1

/* According to 27.007 Spec */
#define DEFAULT_NO_REPLY_TIMEOUT 20

struct call_forwarding_data {
	struct ofono_call_forwarding_ops *ops;
	GSList *cf_conditions[4];
	int flags;
	DBusMessage *pending;
	struct cf_ss_request *ss_req;
};

static void cf_busy_callback(const struct ofono_error *error, int total,
				const struct ofono_cf_condition *list, void *data);
static void cf_unconditional_callback(const struct ofono_error *error, int total,
				const struct ofono_cf_condition *list, void *data);

static void cf_register_ss_controls(struct ofono_modem *modem);
static void cf_unregister_ss_controls(struct ofono_modem *modem);

struct set_cf_request {
	struct ofono_modem *modem;
	int type;
	int cls;
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 1];
	int number_type;
	int timeout;
};

struct cf_ss_request {
	int ss_type;
	int cf_type;
	int cls;
	GSList *cf_list[4];
};

static gint cf_condition_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_cf_condition *ca = a;
	const struct ofono_cf_condition *cb = b;

	if (ca->cls < cb->cls)
		return -1;

	if (ca->cls > cb->cls)
		return 1;

	return 0;
}

static gint cf_condition_find_with_cls(gconstpointer a, gconstpointer b)
{
	const struct ofono_cf_condition *c = a;
	int cls = GPOINTER_TO_INT(b);

	if (c->cls < cls)
		return -1;

	if (c->cls > cls)
		return 1;

	return 0;
}

static int cf_find_timeout(GSList *cf_list, int cls)
{
	GSList *l;
	struct ofono_cf_condition *c;

	l = g_slist_find_custom(cf_list, GINT_TO_POINTER(cls),
		cf_condition_find_with_cls);

	if (!l)
		return DEFAULT_NO_REPLY_TIMEOUT;

	c = l->data;

	return c->time;
}

static void cf_cond_list_print(GSList *list)
{
	GSList *l;
	struct ofono_cf_condition *cond;

	for (l = list; l; l = l->next) {
		cond = l->data;

		ofono_debug("CF Condition status: %d, class: %d, number: %s,"
			" number_type: %d, time: %d",
			cond->status, cond->cls, cond->phone_number,
			cond->number_type, cond->time);
	}
}

static GSList *cf_cond_list_create(int total,
					const struct ofono_cf_condition *list)
{
	GSList *l = NULL;
	int i;
	int j;
	struct ofono_cf_condition *cond;

	/* Specification is not really clear how the results are reported,
	 * so assume both multiple list items & compound values of class
	 * are possible
	 */
	for (i = 0; i < total; i++) {
		for (j = 1; j <= BEARER_CLASS_PAD; j = j << 1) {
			if (!(list[i].cls & j))
				continue;

			if (list[i].status == 0)
				continue;

			cond = g_try_new0(struct ofono_cf_condition, 1);
			if (!cond)
				continue;

			memcpy(cond, &list[i], sizeof(struct ofono_cf_condition));
			cond->cls = j;

			l = g_slist_insert_sorted(l, cond,
							cf_condition_compare);
		}
	}

	return l;
}

static inline void cf_list_clear(GSList *cf_list)
{
	GSList *l;

	for (l = cf_list; l; l = l->next)
		g_free(l->data);

	g_slist_free(cf_list);
}

static inline void cf_clear_all(struct call_forwarding_data *cf)
{
	int i;

	for (i = 0; i < 4; i++) {
		cf_list_clear(cf->cf_conditions[i]);
		cf->cf_conditions[i] = NULL;
	}
}

static struct call_forwarding_data *call_forwarding_create()
{
	struct call_forwarding_data *r;

	r = g_try_new0(struct call_forwarding_data, 1);

	if (!r)
		return r;

	return r;
}

static void call_forwarding_destroy(gpointer data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;

	cf_clear_all(cf);

	cf_unregister_ss_controls(modem);

	g_free(cf);
}

static const char *cf_type_lut[] = {
	"Unconditional",
	"Busy",
	"NoReply",
	"NotReachable",
	"All",
	"AllConditional"
};

static void set_new_cond_list(struct ofono_modem *modem, int type, GSList *list)
{
	struct call_forwarding_data *cf = modem->call_forwarding;
	GSList *old = cf->cf_conditions[type];
	DBusConnection *conn = dbus_gsm_connection();
	GSList *l;
	GSList *o;
	struct ofono_cf_condition *lc;
	struct ofono_cf_condition *oc;
	const char *number;
	dbus_uint16_t timeout;
	char attr[64];
	char tattr[64];

	for (l = list; l; l = l->next) {
		lc = l->data;

		/* New condition lists might have attributes we don't care about
		 * triggered by e.g. ss control magic strings just skip them
		 * here
		 */
		if (lc->cls > BEARER_CLASS_SMS)
			continue;

		timeout = lc->time;
		number = phone_number_to_string(lc->phone_number,
						lc->number_type);

		sprintf(attr, "%s%s", bearer_class_to_string(lc->cls),
					cf_type_lut[type]);

		if (type == CALL_FORWARDING_TYPE_NO_REPLY)
			sprintf(tattr, "%sTimeout", attr);

		o = g_slist_find_custom(old, GINT_TO_POINTER(lc->cls),
					cf_condition_find_with_cls);

		if (o) { /* On the old list, must be active */
			oc = o->data;

			if (oc->number_type != lc->number_type ||
				strcmp(oc->phone_number, lc->phone_number))
				dbus_gsm_signal_property_changed(conn,
						modem->path,
						CALL_FORWARDING_INTERFACE,
						attr, DBUS_TYPE_STRING,
						&number);

			if (type == CALL_FORWARDING_TYPE_NO_REPLY &&
				oc->time != lc->time)
				dbus_gsm_signal_property_changed(conn,
						modem->path,
						CALL_FORWARDING_INTERFACE,
						tattr, DBUS_TYPE_UINT16,
						&timeout);

			/* Remove from the old list */
			g_free(o->data);
			old = g_slist_remove(old, o->data);
		} else {
			number = phone_number_to_string(lc->phone_number,
							lc->number_type);

			dbus_gsm_signal_property_changed(conn, modem->path,
						CALL_FORWARDING_INTERFACE,
						attr, DBUS_TYPE_STRING,
						&number);

			if (type == CALL_FORWARDING_TYPE_NO_REPLY &&
				lc->time != DEFAULT_NO_REPLY_TIMEOUT)
				dbus_gsm_signal_property_changed(conn,
						modem->path,
						CALL_FORWARDING_INTERFACE,
						tattr, DBUS_TYPE_UINT16,
						&timeout);
		}
	}

	timeout = DEFAULT_NO_REPLY_TIMEOUT;
	number = "";

	for (o = old; o; o = o->next) {
		oc = o->data;

		sprintf(attr, "%s%s", bearer_class_to_string(oc->cls),
					cf_type_lut[type]);

		if (type == CALL_FORWARDING_TYPE_NO_REPLY)
			sprintf(tattr, "%sTimeout", attr);

		dbus_gsm_signal_property_changed(conn, modem->path,
					CALL_FORWARDING_INTERFACE, attr,
					DBUS_TYPE_STRING, &number);

		if (type == CALL_FORWARDING_TYPE_NO_REPLY &&
			oc->time != DEFAULT_NO_REPLY_TIMEOUT)
			dbus_gsm_signal_property_changed(conn, modem->path,
						CALL_FORWARDING_INTERFACE,
						tattr, DBUS_TYPE_UINT16,
						&timeout);
	}

	cf_list_clear(old);
	cf->cf_conditions[type] = list;
}

static inline void property_append_cf_condition(DBusMessageIter *dict, int cls,
						const char *postfix,
						const char *value,
						dbus_uint16_t timeout)
{
	char attr[64];
	char tattr[64];
	int addt = !strcmp(postfix, "NoReply");

	sprintf(attr, "%s%s", bearer_class_to_string(cls), postfix);

	if (addt)
		sprintf(tattr, "%s%sTimeout", bearer_class_to_string(cls),
			postfix);

	dbus_gsm_dict_append(dict, attr, DBUS_TYPE_STRING, &value);

	if (addt)
		dbus_gsm_dict_append(dict, tattr, DBUS_TYPE_UINT16, &timeout);
}

static void property_append_cf_conditions(DBusMessageIter *dict,
						GSList *cf_list, int mask,
						const char *postfix)
{
	GSList *l;
	int i;
	struct ofono_cf_condition *cf;
	const char *number;

	for (i = 1, l = cf_list; i <= BEARER_CLASS_PAD; i = i << 1) {
		if (!(mask & i))
			continue;

		while (l && (cf = l->data) && (cf->cls < i))
				l = l->next;

		if (!l || cf->cls != i) {
			property_append_cf_condition(dict, i, postfix, "",
						DEFAULT_NO_REPLY_TIMEOUT);
			continue;
		}

		number = phone_number_to_string(cf->phone_number,
						cf->number_type);

		property_append_cf_condition(dict, i, postfix, number,
						cf->time);
	}
}

static DBusMessage *cf_get_properties_reply(DBusMessage *msg,
						struct call_forwarding_data *cf)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	int i;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	for (i = 0; i < 4; i++)
		property_append_cf_conditions(&dict,
				cf->cf_conditions[i],
				BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS,
				cf_type_lut[i]);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *cf_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;

	if (cf->flags & CALL_FORWARDING_FLAG_CACHED)
		return cf_get_properties_reply(msg, cf);

	/* We kicked off the query during interface creation, wait for it */
	cf->pending = dbus_message_ref(msg);

	return NULL;
}

static gboolean cf_condition_enabled_property(struct call_forwarding_data *cf,
			const char *property, int *out_type, int *out_cls)
{
	int i;
	int j;
	int len;
	const char *prefix;

	/* We check the 4 bearer classes here, e.g. voice, data, fax, sms */
	for (i = 0; i < 4; i++) {
		prefix = bearer_class_to_string(1 << i);

		len = strlen(prefix);

		if (strncmp(property, prefix, len))
			continue;

		/* We check the 4 call forwarding types, e.g.
		 * unconditional, busy, no reply, not reachable
		 */
		for (j = 0; j < 4; j++)
			if (!strcmp(property+len, cf_type_lut[j])) {
				*out_type = j;
				*out_cls = 1 << i;
				return TRUE;
			}
	}

	return FALSE;
}

static gboolean cf_condition_timeout_property(const char *property,
						int *out_cls)
{
	int i;
	int len;
	const char *prefix;

	for (i = 0; i < 4; i++) {
		prefix = bearer_class_to_string(1 << i);

		len = strlen(prefix);

		if (strncmp(property, prefix, len))
			continue;

		if (!strcmp(property+len, "NoReplyTimeout")) {
			*out_cls = 1 << i;
			return TRUE;
		}
	}

	return FALSE;
}

static void cf_condition_manual_set(struct set_cf_request *req)
{
	struct ofono_modem *modem = req->modem;
	struct call_forwarding_data *cf = modem->call_forwarding;
	DBusConnection *conn = dbus_gsm_connection();
	int status = req->number[0] == '\0' ? 0 : 1;
	GSList *l;
	struct ofono_cf_condition *c;
	char attr[64];
	char tattr[64];
	const char *number = "";
	dbus_uint16_t timeout;

	l = g_slist_find_custom(cf->cf_conditions[req->type],
					GINT_TO_POINTER(req->cls),
					cf_condition_find_with_cls);

	ofono_debug("L is: %p, status is: %d", l, status);

	if (!l && !status)
		return;

	sprintf(attr, "%s%s", bearer_class_to_string(req->cls),
					cf_type_lut[req->type]);

	if (req->type == CALL_FORWARDING_TYPE_NO_REPLY)
		sprintf(tattr, "%sTimeout", attr);

	if (l && !status) {
		c = l->data;
		timeout = DEFAULT_NO_REPLY_TIMEOUT;

		dbus_gsm_signal_property_changed(conn, modem->path,
			CALL_FORWARDING_INTERFACE,
			attr, DBUS_TYPE_STRING, &number);

		if (req->type == CALL_FORWARDING_TYPE_NO_REPLY &&
			c->time != DEFAULT_NO_REPLY_TIMEOUT)
			dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_FORWARDING_INTERFACE, tattr,
				DBUS_TYPE_UINT16, &timeout);

		ofono_debug("Removing condition");

		g_free(c);
		cf->cf_conditions[req->type] =
			g_slist_remove(cf->cf_conditions[req->type], c);

		return;
	}

	if (l)
		c = l->data;
	else {
		c = g_try_new0(struct ofono_cf_condition, 1);

		if (!c)
			return;

		c->status = 1;
		c->cls = req->cls;
		c->phone_number[0] = '\0';
		c->number_type = 129;
		c->time = DEFAULT_NO_REPLY_TIMEOUT;

		ofono_debug("Inserting condition");
		cf->cf_conditions[req->type] =
			g_slist_insert_sorted(cf->cf_conditions[req->type],
						c, cf_condition_compare);
	}

	if (c->number_type != req->number_type ||
		strcmp(req->number, c->phone_number)) {
		strcpy(c->phone_number, req->number);
		c->number_type = req->number_type;

		number = phone_number_to_string(req->number, req->number_type);

		dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_FORWARDING_INTERFACE,
				attr, DBUS_TYPE_STRING, &number);
	}

	if (req->type == CALL_FORWARDING_TYPE_NO_REPLY &&
		c->time != req->timeout) {
		c->time = req->timeout;

		dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_FORWARDING_INTERFACE,
				tattr, DBUS_TYPE_UINT16, &req->timeout);
	}
}

static void pending_msg_error(struct call_forwarding_data *cf,
				const struct ofono_error *error)
{
	DBusMessage *reply;
	DBusConnection *conn = dbus_gsm_connection();

	reply = dbus_gsm_failed(cf->pending);
	g_dbus_send_message(conn, reply);

	dbus_message_unref(cf->pending);
	cf->pending = NULL;
}

static void property_set_query_callback(const struct ofono_error *error, int total,
					const struct ofono_cf_condition *list,
					void *data)
{
	struct set_cf_request *req = data;
	struct ofono_modem *modem = req->modem;
	struct call_forwarding_data *cf = modem->call_forwarding;
	//DBusConnection *conn = dbus_gsm_connection();
	DBusMessage *reply;
	GSList *new_cf_list;

	reply = dbus_message_new_method_return(cf->pending);
	dbus_gsm_pending_reply(&cf->pending, reply);

	/* Strange, set succeeded but query failed, fallback to direct method */
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during query");
		cf_condition_manual_set(req);
		goto out;
	}

	new_cf_list = cf_cond_list_create(total, list);

	ofono_debug("Query ran successfully");
	cf_cond_list_print(new_cf_list);

	set_new_cond_list(modem, req->type, new_cf_list);

out:
	g_free(req);
}

static void set_property_callback(const struct ofono_error *error, void *data)
{
	struct set_cf_request *req = data;
	struct ofono_modem *modem = req->modem;
	struct call_forwarding_data *cf = modem->call_forwarding;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during set/erasure");

		pending_msg_error(cf, error);
		g_free(req);

		return;
	}

	/* Successfully set, query the entire set just in case */
	cf->ops->query(modem, req->type,
			BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS,
			property_set_query_callback, req);
}

static DBusMessage *set_property_request(struct ofono_modem *modem,
						DBusMessage *msg,
						int type, int cls,
						const char *number,
						int number_type, int timeout)
{
	struct call_forwarding_data *cf = modem->call_forwarding;
	struct set_cf_request *req;

	if (number[0] != '\0' && cf->ops->registration == NULL)
		return dbus_gsm_not_implemented(msg);

	if (number[0] == '\0' && cf->ops->erasure == NULL)
		return dbus_gsm_not_implemented(msg);

	req = g_try_new0(struct set_cf_request, 1);

	if (!req)
		return dbus_gsm_failed(msg);

	req->modem = modem;
	req->type = type;
	req->cls = cls;
	strcpy(req->number, number);
	req->number_type = number_type;
	req->timeout = timeout;

	cf->pending = dbus_message_ref(msg);

	ofono_debug("Farming off request, will be erasure: %d", number[0] == 0);

	if (number[0] != '\0')
		cf->ops->registration(modem, type, cls, number, number_type,
				timeout, set_property_callback, req);
	else
		cf->ops->erasure(modem, type, cls, set_property_callback, req);

	return NULL;
}

static DBusMessage *cf_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	int cls;
	int type;

	if (cf->pending)
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

	if (cf_condition_timeout_property(property, &cls)) {
		dbus_uint16_t timeout;
		GSList *l;
		struct ofono_cf_condition *c;

		type = CALL_FORWARDING_TYPE_NO_REPLY;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_UINT16)
			return dbus_gsm_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &timeout);

		if (timeout < 1 || timeout > 30)
			return dbus_gsm_invalid_format(msg);

		l = g_slist_find_custom(cf->cf_conditions[type],
				GINT_TO_POINTER(cls),
				cf_condition_find_with_cls);

		if (!l)
			return dbus_gsm_failed(msg);

		c = l->data;

		return set_property_request(modem, msg, type, cls,
						c->phone_number,
						c->number_type, timeout);
	} else if (cf_condition_enabled_property(cf, property, &type, &cls)) {
		const char *number;
		int number_type;
		int timeout;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return dbus_gsm_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &number);

		if (strlen(number) > 0 && !valid_phone_number_format(number))
			return dbus_gsm_invalid_format(msg);

		if (number[0] != '\0')
			string_to_phone_number(number, &number_type, &number);
		else
			number_type = 129;

		timeout = cf_find_timeout(cf->cf_conditions[type], cls);

		return set_property_request(modem, msg, type, cls, number,
					number_type, timeout);
	}

	return dbus_gsm_invalid_args(msg);
}

static void disable_conditional_callback(const struct ofono_error *error,
						void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	//DBusConnection *conn = dbus_gsm_connection();
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during conditional erasure");

		pending_msg_error(cf, error);

		return;
	}

	reply = dbus_message_new_method_return(cf->pending);
	dbus_gsm_pending_reply(&cf->pending, reply);

	set_new_cond_list(modem, CALL_FORWARDING_TYPE_NO_REPLY, NULL);
	set_new_cond_list(modem, CALL_FORWARDING_TYPE_NOT_REACHABLE, NULL);
	set_new_cond_list(modem, CALL_FORWARDING_TYPE_BUSY, NULL);

	cf->ops->query(modem, CALL_FORWARDING_TYPE_BUSY,
			BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS,
			cf_busy_callback, modem);
}

static void disable_all_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	//DBusConnection *conn = dbus_gsm_connection();
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during erasure of all");

		pending_msg_error(cf, error);

		return;
	}

	reply = dbus_message_new_method_return(cf->pending);
	dbus_gsm_pending_reply(&cf->pending, reply);

	set_new_cond_list(modem, CALL_FORWARDING_TYPE_UNCONDITIONAL, NULL);
	set_new_cond_list(modem, CALL_FORWARDING_TYPE_NO_REPLY, NULL);
	set_new_cond_list(modem, CALL_FORWARDING_TYPE_NOT_REACHABLE, NULL);
	set_new_cond_list(modem, CALL_FORWARDING_TYPE_BUSY, NULL);

	cf->ops->query(modem, CALL_FORWARDING_TYPE_UNCONDITIONAL,
			BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS,
			cf_unconditional_callback, modem);
}

static DBusMessage *cf_disable_all(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	const char *strtype;
	int type;

	if (cf->pending)
		return dbus_gsm_busy(msg);

	if (!cf->ops->erasure)
		return dbus_gsm_not_implemented(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &strtype,
					DBUS_TYPE_INVALID) == FALSE)
		return dbus_gsm_invalid_args(msg);

	if (!strcmp(strtype, "all") || !strcmp(strtype, ""))
		type = CALL_FORWARDING_TYPE_ALL;
	else if (!strcmp(strtype, "conditional"))
		type = CALL_FORWARDING_TYPE_ALL_CONDITIONAL;
	else
		return dbus_gsm_invalid_format(msg);

	cf->pending = dbus_message_ref(msg);

	if (type == CALL_FORWARDING_TYPE_ALL)
		cf->ops->erasure(modem, type,
				BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS,
				disable_all_callback, modem);
	else
		cf->ops->erasure(modem, type,
				BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS,
				disable_conditional_callback, modem);

	return NULL;
}

static GDBusMethodTable cf_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cf_get_properties },
	{ "SetProperty",	"sv",	"",		cf_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "DisableAll",		"s",	"",		cf_disable_all,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cf_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static void cf_ss_control_reply(struct ofono_modem *modem,
					struct cf_ss_request *req)
{
	struct call_forwarding_data *cf = modem->call_forwarding;
	const char *context = "CallForwarding";
	const char *sig = "(ssa{sv})";
	const char *ss_type = ss_control_type_to_string(req->ss_type);
	const char *cf_type = cf_type_lut[req->cf_type];
	DBusConnection *conn = dbus_gsm_connection();
	DBusMessageIter iter;
	DBusMessageIter variant;
	DBusMessageIter vstruct;
	DBusMessageIter dict;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(cf->pending);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &context);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, sig,
						&variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, NULL,
						&vstruct);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING,
					&ss_type);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING,
					&cf_type);

	dbus_message_iter_open_container(&vstruct, DBUS_TYPE_ARRAY,
					PROPERTIES_ARRAY_SIGNATURE, &dict);

	if (req->cf_type == CALL_FORWARDING_TYPE_UNCONDITIONAL ||
		req->cf_type == CALL_FORWARDING_TYPE_ALL)
		property_append_cf_conditions(&dict,
			req->cf_list[CALL_FORWARDING_TYPE_UNCONDITIONAL],
			req->cls,
			cf_type_lut[CALL_FORWARDING_TYPE_UNCONDITIONAL]);

	if (req->cf_type == CALL_FORWARDING_TYPE_NO_REPLY ||
		req->cf_type == CALL_FORWARDING_TYPE_ALL ||
		req->cf_type == CALL_FORWARDING_TYPE_ALL_CONDITIONAL)
		property_append_cf_conditions(&dict,
			req->cf_list[CALL_FORWARDING_TYPE_NO_REPLY],
			req->cls, cf_type_lut[CALL_FORWARDING_TYPE_NO_REPLY]);

	if (req->cf_type == CALL_FORWARDING_TYPE_NOT_REACHABLE ||
		req->cf_type == CALL_FORWARDING_TYPE_ALL ||
		req->cf_type == CALL_FORWARDING_TYPE_ALL_CONDITIONAL)
		property_append_cf_conditions(&dict,
			req->cf_list[CALL_FORWARDING_TYPE_NOT_REACHABLE],
			req->cls,
			cf_type_lut[CALL_FORWARDING_TYPE_NOT_REACHABLE]);

	if (req->cf_type == CALL_FORWARDING_TYPE_BUSY ||
		req->cf_type == CALL_FORWARDING_TYPE_ALL ||
		req->cf_type == CALL_FORWARDING_TYPE_ALL_CONDITIONAL)
		property_append_cf_conditions(&dict,
			req->cf_list[CALL_FORWARDING_TYPE_BUSY],
			req->cls, cf_type_lut[CALL_FORWARDING_TYPE_BUSY]);

	dbus_message_iter_close_container(&vstruct, &dict);

	dbus_message_iter_close_container(&variant, &vstruct);

	dbus_message_iter_close_container(&iter, &variant);

	g_dbus_send_message(conn, reply);
}

static void cf_ss_control_query_callback(const struct ofono_error *error,
					int total,
					const struct ofono_cf_condition *list,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	//DBusConnection *conn = dbus_gsm_connection();
	//DBusMessage *reply;
	GSList *new_cf_list;

	/* Strange, set succeeded but query failed, fallback to direct method */
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during cf ss query");

		pending_msg_error(cf, error);

		return;
	}

	new_cf_list = cf_cond_list_create(total, list);

	ofono_debug("Query ran successfully");
	cf_cond_list_print(new_cf_list);

	cf->ss_req->cf_list[cf->ss_req->cf_type] = new_cf_list;

	set_new_cond_list(modem, cf->ss_req->cf_type, new_cf_list);

	cf_ss_control_reply(modem, cf->ss_req);

	dbus_message_unref(cf->pending);
	cf->pending = NULL;

	g_free(cf->ss_req);
	cf->ss_req = NULL;
}

static void cf_ss_control_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	int cls;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during cf ss control set/erasure");

		pending_msg_error(cf, error);

		return;
	}

	cls = BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS | cf->ss_req->cls;

	/* Successfully set, query the entire set just in case */
	if (cf->ss_req->cf_type == CALL_FORWARDING_TYPE_ALL)
		cf->ops->query(modem, CALL_FORWARDING_TYPE_UNCONDITIONAL,
				cls, cf_unconditional_callback, modem);
	else if (cf->ss_req->cf_type == CALL_FORWARDING_TYPE_ALL_CONDITIONAL)
		cf->ops->query(modem, CALL_FORWARDING_TYPE_BUSY,
				cls, cf_busy_callback, modem);
	else
		cf->ops->query(modem, cf->ss_req->cf_type, cls,
				cf_ss_control_query_callback, modem);
}

static gboolean cf_ss_control(struct ofono_modem *modem, int type, const char *sc,
				const char *sia, const char *sib,
				const char *sic, const char *dn,
				DBusMessage *msg)
{
	struct call_forwarding_data *cf = modem->call_forwarding;
	DBusConnection *conn = dbus_gsm_connection();
	int cls = BEARER_CLASS_DEFAULT;
	int timeout = DEFAULT_NO_REPLY_TIMEOUT;
	int cf_type;
	DBusMessage *reply;
	const char *number;
	int number_type;
	void *operation;

	/* Before we do anything, make sure we're actually initialized */
	if (!cf)
		return FALSE;

	if (cf->pending) {
		reply = dbus_gsm_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	ofono_debug("Received call forwarding ss control request");

	ofono_debug("type: %d, sc: %s, sia: %s, sib: %s, sic: %s, dn: %s",
			type, sc, sia, sib, sic, dn);

	if (!strcmp(sc, "21"))
		cf_type = CALL_FORWARDING_TYPE_UNCONDITIONAL;
	else if (!strcmp(sc, "67"))
		cf_type = CALL_FORWARDING_TYPE_BUSY;
	else if (!strcmp(sc, "61"))
		cf_type = CALL_FORWARDING_TYPE_NO_REPLY;
	else if (!strcmp(sc, "62"))
		cf_type = CALL_FORWARDING_TYPE_NOT_REACHABLE;
	else if (!strcmp(sc, "002"))
		cf_type = CALL_FORWARDING_TYPE_ALL;
	else if (!strcmp(sc, "004"))
		cf_type = CALL_FORWARDING_TYPE_ALL_CONDITIONAL;
	else
		return FALSE;

	if (strlen(sia) &&
		(type == SS_CONTROL_TYPE_QUERY ||
		type == SS_CONTROL_TYPE_ERASURE ||
		type == SS_CONTROL_TYPE_DEACTIVATION))
		goto error;

	/* Activation / Registration is figured context specific according to
	 * 22.030 Section 6.5.2 "The UE shall determine from the context
	 * whether, an entry of a single *, activation or registration
	 * was intended."
	 */
	if (type == SS_CONTROL_TYPE_ACTIVATION && strlen(sia) > 0)
		type = SS_CONTROL_TYPE_REGISTRATION;

	if (type == SS_CONTROL_TYPE_REGISTRATION &&
		!valid_phone_number_format(sia))
		goto error;

	if (strlen(sib) > 0) {
		long service_code;
		char *end;

		service_code = strtoul(sib, &end, 10);

		if (end == sib || *end != '\0')
			goto error;

		cls = mmi_service_code_to_bearer_class(service_code);

		if (cls == 0)
			goto error;
	}

	if (strlen(sic) > 0) {
		char *end;

		if  (type != SS_CONTROL_TYPE_REGISTRATION)
			goto error;

		if (cf_type != CALL_FORWARDING_TYPE_ALL &&
			cf_type != CALL_FORWARDING_TYPE_ALL_CONDITIONAL &&
			cf_type != CALL_FORWARDING_TYPE_NO_REPLY)
			goto error;

		timeout = strtoul(sic, &end, 10);

		if (end == sic || *end != '\0')
			goto error;

		if (timeout < 1 || timeout > 30)
			goto error;
	}

	switch (type) {
	case SS_CONTROL_TYPE_REGISTRATION:
		operation = cf->ops->registration;
		break;
	case SS_CONTROL_TYPE_ACTIVATION:
		operation = cf->ops->activation;
		break;
	case SS_CONTROL_TYPE_DEACTIVATION:
		operation = cf->ops->deactivation;
		break;
	case SS_CONTROL_TYPE_ERASURE:
		operation = cf->ops->erasure;
		break;
	case SS_CONTROL_TYPE_QUERY:
		operation = cf->ops->query;
		break;
	}

	if (!operation) {
		reply = dbus_gsm_not_implemented(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	cf->ss_req = g_try_new0(struct cf_ss_request, 1);

	if (!cf->ss_req) {
		reply = dbus_gsm_failed(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	cf->ss_req->ss_type = type;
	cf->ss_req->cf_type = cf_type;
	cf->ss_req->cls = cls;

	cf->pending = dbus_message_ref(msg);

	switch (cf->ss_req->ss_type) {
	case SS_CONTROL_TYPE_REGISTRATION:
		string_to_phone_number(sia, &number_type, &number);
		cf->ops->registration(modem, cf_type, cls, number, number_type,
					timeout, cf_ss_control_callback,
					modem);
		break;
	case SS_CONTROL_TYPE_ACTIVATION:
		cf->ops->activation(modem, cf_type, cls, cf_ss_control_callback,
					modem);
		break;
	case SS_CONTROL_TYPE_DEACTIVATION:
		cf->ops->deactivation(modem, cf_type, cls,
					cf_ss_control_callback, modem);
		break;
	case SS_CONTROL_TYPE_ERASURE:
		cf->ops->erasure(modem, cf_type, cls, cf_ss_control_callback,
					modem);
		break;
	case SS_CONTROL_TYPE_QUERY:
		cls |= BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS;
		if (cf_type == CALL_FORWARDING_TYPE_ALL)
			cf->ops->query(modem,
					CALL_FORWARDING_TYPE_UNCONDITIONAL,
					cls, cf_unconditional_callback, modem);
		else if (cf_type == CALL_FORWARDING_TYPE_ALL_CONDITIONAL)
			cf->ops->query(modem, CALL_FORWARDING_TYPE_BUSY,
					cls, cf_busy_callback, modem);
		else
			cf->ops->query(modem, cf_type, cls,
					cf_ss_control_query_callback, modem);
		break;
	}

	return TRUE;

error:
	reply = dbus_gsm_invalid_format(msg);
	g_dbus_send_message(conn, reply);
	return TRUE;
}

static void cf_register_ss_controls(struct ofono_modem *modem)
{
	ss_control_register(modem, "21", cf_ss_control);
	ss_control_register(modem, "67", cf_ss_control);
	ss_control_register(modem, "61", cf_ss_control);
	ss_control_register(modem, "62", cf_ss_control);

	ss_control_register(modem, "002", cf_ss_control);
	ss_control_register(modem, "004", cf_ss_control);
}

static void cf_unregister_ss_controls(struct ofono_modem *modem)
{
	ss_control_unregister(modem, "21", cf_ss_control);
	ss_control_unregister(modem, "67", cf_ss_control);
	ss_control_unregister(modem, "61", cf_ss_control);
	ss_control_unregister(modem, "62", cf_ss_control);

	ss_control_unregister(modem, "002", cf_ss_control);
	ss_control_unregister(modem, "004", cf_ss_control);
}

static void cf_not_reachable_callback(const struct ofono_error *error, int total,
				const struct ofono_cf_condition *list, void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	GSList *l = NULL;
	//DBusConnection *conn = dbus_gsm_connection();

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during not reachable CF query");
		goto out;
	}

	l = cf_cond_list_create(total, list);

	set_new_cond_list(modem, CALL_FORWARDING_TYPE_NOT_REACHABLE, l);

	ofono_debug("Not Reachable conditions:");
	cf_cond_list_print(l);

out:

	cf->flags |= CALL_FORWARDING_FLAG_CACHED;

	if (cf->pending) {
		if (cf->ss_req) {
			cf->ss_req->cf_list[CALL_FORWARDING_TYPE_NOT_REACHABLE] = l;
			cf_ss_control_reply(modem, cf->ss_req);
			g_free(cf->ss_req);
			cf->ss_req = NULL;
		} else {
			DBusConnection *conn = dbus_gsm_connection();
			DBusMessage *reply =
				cf_get_properties_reply(cf->pending, cf);

			g_dbus_send_message(conn, reply);
		}

		dbus_message_unref(cf->pending);
		cf->pending = NULL;
	}
}

static void cf_no_reply_callback(const struct ofono_error *error, int total,
				const struct ofono_cf_condition *list, void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	GSList *l = NULL;
	int cls = BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during no reply CF query");
		goto out;
	}

	l = cf_cond_list_create(total, list);

	set_new_cond_list(modem, CALL_FORWARDING_TYPE_NO_REPLY, l);

	ofono_debug("No Reply conditions:");
	cf_cond_list_print(l);

out:
	if (cf->ss_req) {
		cls |= cf->ss_req->cls;
		cf->ss_req->cf_list[CALL_FORWARDING_TYPE_NO_REPLY] = l;
	}

	cf->ops->query(modem, CALL_FORWARDING_TYPE_NOT_REACHABLE,
			cls, cf_not_reachable_callback, modem);
}

static void cf_busy_callback(const struct ofono_error *error, int total,
				const struct ofono_cf_condition *list, void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	GSList *l = NULL;
	int cls = BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during busy CF query");
		goto out;
	}

	l = cf_cond_list_create(total, list);

	set_new_cond_list(modem, CALL_FORWARDING_TYPE_BUSY, l);

	ofono_debug("On Busy conditions:");
	cf_cond_list_print(l);

out:
	if (cf->ss_req) {
		cls |= cf->ss_req->cls;
		cf->ss_req->cf_list[CALL_FORWARDING_TYPE_BUSY] = l;
	}

	cf->ops->query(modem, CALL_FORWARDING_TYPE_NO_REPLY,
			cls, cf_no_reply_callback, modem);
}

static void cf_unconditional_callback(const struct ofono_error *error, int total,
					const struct ofono_cf_condition *list,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *cf = modem->call_forwarding;
	GSList *l = NULL;
	int cls = BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during unconditional CF query");
		goto out;
	}

	l = cf_cond_list_create(total, list);

	set_new_cond_list(modem, CALL_FORWARDING_TYPE_UNCONDITIONAL, l);

	ofono_debug("Unconditional conditions:");
	cf_cond_list_print(l);

out:
	if (cf->ss_req) {
		cls |= cf->ss_req->cls;
		cf->ss_req->cf_list[CALL_FORWARDING_TYPE_UNCONDITIONAL] = l;
	}

	cf->ops->query(modem, CALL_FORWARDING_TYPE_BUSY,
			cls, cf_busy_callback, modem);
}

static gboolean initiate_settings_request(void *data)
{
	struct ofono_modem *modem = data;
	struct call_forwarding_data *call_forwarding = modem->call_forwarding;

	/* We can't get all settings at the same time according to 22.004:
	 * "Interrogation of groups of Supplementary Services is not supported."
	 * so we do it piecemeal, unconditional, busy, no reply, not reachable
	 */

	if (call_forwarding->ops->query)
		call_forwarding->ops->query(modem,
					CALL_FORWARDING_TYPE_UNCONDITIONAL,
					BEARER_CLASS_DEFAULT | BEARER_CLASS_SMS,
					cf_unconditional_callback, modem);

	return FALSE;
}

static void request_settings(struct ofono_modem *modem)
{
	g_timeout_add(0, initiate_settings_request, modem);
}

int ofono_call_forwarding_register(struct ofono_modem *modem,
				struct ofono_call_forwarding_ops *ops)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	if (ops->query == NULL)
		return -1;

	modem->call_forwarding = call_forwarding_create();

	if (modem->call_forwarding == NULL)
		return -1;

	modem->call_forwarding->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					CALL_FORWARDING_INTERFACE,
					cf_methods, cf_signals, NULL,
					modem, call_forwarding_destroy)) {
		ofono_error("Could not register CallForwarding %s", modem->path);
		call_forwarding_destroy(modem);

		return -1;
	}

	ofono_debug("Registered call forwarding interface");

	cf_register_ss_controls(modem);

	modem_add_interface(modem, CALL_FORWARDING_INTERFACE);

	request_settings(modem);

	return 0;
}

void ofono_call_forwarding_unregister(struct ofono_modem *modem)
{
	struct call_forwarding_data *cf = modem->call_forwarding;
	DBusConnection *conn = dbus_gsm_connection();

	if (!cf)
		return;

	modem_remove_interface(modem, CALL_FORWARDING_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path,
					CALL_FORWARDING_INTERFACE);

	modem->call_forwarding = NULL;
}
