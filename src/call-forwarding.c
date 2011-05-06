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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"
#include "simutil.h"

#define uninitialized_var(x) x = x

#define CALL_FORWARDING_FLAG_CACHED 0x1
#define CALL_FORWARDING_FLAG_CPHS_CFF 0x2

/* According to 27.007 Spec */
#define DEFAULT_NO_REPLY_TIMEOUT 20

static GSList *g_drivers = NULL;

enum call_forwarding_type {
	CALL_FORWARDING_TYPE_UNCONDITIONAL =		0,
	CALL_FORWARDING_TYPE_BUSY =			1,
	CALL_FORWARDING_TYPE_NO_REPLY =			2,
	CALL_FORWARDING_TYPE_NOT_REACHABLE =		3,
	CALL_FORWARDING_TYPE_ALL =			4,
	CALL_FORWARDING_TYPE_ALL_CONDITIONAL =		5
};

struct ofono_call_forwarding {
	GSList *cf_conditions[4];
	int flags;
	DBusMessage *pending;
	int query_next;
	int query_end;
	struct cf_ss_request *ss_req;
	struct ofono_sim *sim;
	struct ofono_sim_context *sim_context;
	unsigned char cfis_record_id;
	struct ofono_ussd *ussd;
	unsigned int ussd_watch;
	const struct ofono_call_forwarding_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static void get_query_next_cf_cond(struct ofono_call_forwarding *cf);
static void set_query_next_cf_cond(struct ofono_call_forwarding *cf);
static void ss_set_query_next_cf_cond(struct ofono_call_forwarding *cf);

struct cf_ss_request {
	int ss_type;
	int cf_type;
	int cls;
	GSList *cf_list[4];
};

static gint cf_condition_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call_forwarding_condition *ca = a;
	const struct ofono_call_forwarding_condition *cb = b;

	if (ca->cls < cb->cls)
		return -1;

	if (ca->cls > cb->cls)
		return 1;

	return 0;
}

static gint cf_condition_find_with_cls(gconstpointer a, gconstpointer b)
{
	const struct ofono_call_forwarding_condition *c = a;
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
	struct ofono_call_forwarding_condition *c;

	l = g_slist_find_custom(cf_list, GINT_TO_POINTER(cls),
		cf_condition_find_with_cls);

	if (l == NULL)
		return DEFAULT_NO_REPLY_TIMEOUT;

	c = l->data;

	return c->time;
}

static void cf_cond_list_print(GSList *list)
{
	GSList *l;
	struct ofono_call_forwarding_condition *cond;

	for (l = list; l; l = l->next) {
		cond = l->data;

		DBG("CF Condition status: %d, class: %d, number: %s,"
			" number_type: %d, time: %d",
			cond->status, cond->cls, cond->phone_number.number,
			cond->phone_number.type, cond->time);
	}
}

static GSList *cf_cond_list_create(int total,
			const struct ofono_call_forwarding_condition *list)
{
	GSList *l = NULL;
	int i;
	int j;
	struct ofono_call_forwarding_condition *cond;

	/*
	 * Specification is not really clear how the results are reported,
	 * so assume both multiple list items & compound values of class
	 * are possible
	 */
	for (i = 0; i < total; i++) {
		for (j = 1; j <= BEARER_CLASS_PAD; j = j << 1) {
			if (!(list[i].cls & j))
				continue;

			if (list[i].status == 0)
				continue;

			cond = g_try_new0(struct ofono_call_forwarding_condition, 1);
			if (cond == NULL)
				continue;

			memcpy(cond, &list[i],
				sizeof(struct ofono_call_forwarding_condition));
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

static inline void cf_clear_all(struct ofono_call_forwarding *cf)
{
	int i;

	for (i = 0; i < 4; i++) {
		cf_list_clear(cf->cf_conditions[i]);
		cf->cf_conditions[i] = NULL;
	}
}

static const char *cf_type_lut[] = {
	"Unconditional",
	"Busy",
	"NoReply",
	"NotReachable",
	"All",
	"AllConditional"
};

static void sim_cfis_update_cb(int ok, void *data)
{
	if (!ok)
		ofono_info("Failed to update EFcfis");
}

static void sim_cphs_cff_update_cb(int ok, void *data)
{
	if (!ok)
		ofono_info("Failed to update EFcphs-cff");
}

static gboolean is_cfu_enabled(struct ofono_call_forwarding *cf,
				struct ofono_call_forwarding_condition **out)
{
	GSList *l = cf->cf_conditions[CALL_FORWARDING_TYPE_UNCONDITIONAL];
	struct ofono_call_forwarding_condition *cond;

	/*
	 * For now we only support Voice, although Fax & all Data
	 * basic services are applicable as well.
	 */
	for (; l; l = l->next) {
		cond = l->data;

		if (cond->cls > BEARER_CLASS_VOICE)
			continue;

		if (out)
			*out = cond;

		return TRUE;
	}

	return FALSE;
}

static void sim_set_cf_indicator(struct ofono_call_forwarding *cf)
{
	gboolean cfu_voice;
	struct ofono_call_forwarding_condition *uninitialized_var(cond);

	cfu_voice = is_cfu_enabled(cf, &cond);

	if (cf->cfis_record_id) {
		unsigned char data[16];
		int number_len;

		memset(data, 0xff, sizeof(data));

		/* Profile Identifier */
		data[0] = 0x01;

		if (cfu_voice) {
			number_len = strlen(cond->phone_number.number);

			/* CFU indicator Status - Voice */
			data[1] = 0x01;
			number_len = (number_len + 1) / 2;
			data[2] = number_len + 1;
			data[3] = cond->phone_number.type;

			sim_encode_bcd_number(cond->phone_number.number,
						data + 4);
		} else {
			data[1] = 0x00;
			data[2] = 1;
			data[3] = 128;
		}

		ofono_sim_write(cf->sim_context, SIM_EFCFIS_FILEID,
					sim_cfis_update_cb,
					OFONO_SIM_FILE_STRUCTURE_FIXED,
					cf->cfis_record_id, data,
					sizeof(data), cf);
		return;
	}

	if (cf->flags & CALL_FORWARDING_FLAG_CPHS_CFF) {
		unsigned char cff_voice = cfu_voice ? 0x0A : 0x05;

		ofono_sim_write(cf->sim_context, SIM_EF_CPHS_CFF_FILEID,
					sim_cphs_cff_update_cb,
					OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
					0, &cff_voice, sizeof(cff_voice), cf);
	}
}

static void set_new_cond_list(struct ofono_call_forwarding *cf,
				int type, GSList *list)
{
	GSList *old = cf->cf_conditions[type];
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cf->atom);
	GSList *l;
	GSList *o;
	struct ofono_call_forwarding_condition *lc;
	struct ofono_call_forwarding_condition *oc;
	const char *number;
	dbus_uint16_t timeout;
	char attr[64];
	char tattr[64];
	gboolean update_sim = FALSE;
	gboolean old_cfu;
	gboolean new_cfu;

	if ((cf->flags & CALL_FORWARDING_FLAG_CPHS_CFF) ||
			cf->cfis_record_id > 0)
		old_cfu = is_cfu_enabled(cf, NULL);
	else
		old_cfu = FALSE;

	for (l = list; l; l = l->next) {
		lc = l->data;

		/*
		 * New condition lists might have attributes we don't care about
		 * triggered by e.g. ss control magic strings just skip them
		 * here.  For now we only support Voice, although Fax & all Data
		 * basic services are applicable as well.
		 */
		if (lc->cls > BEARER_CLASS_VOICE)
			continue;

		timeout = lc->time;
		number = phone_number_to_string(&lc->phone_number);

		snprintf(attr, sizeof(attr), "%s%s",
			bearer_class_to_string(lc->cls), cf_type_lut[type]);

		if (type == CALL_FORWARDING_TYPE_NO_REPLY)
			snprintf(tattr, sizeof(tattr), "%sTimeout", attr);

		o = g_slist_find_custom(old, GINT_TO_POINTER(lc->cls),
					cf_condition_find_with_cls);

		if (o) { /* On the old list, must be active */
			oc = o->data;

			if (oc->phone_number.type != lc->phone_number.type ||
				strcmp(oc->phone_number.number,
					lc->phone_number.number)) {
				ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_FORWARDING_INTERFACE,
						attr, DBUS_TYPE_STRING,
						&number);

				if (type == CALL_FORWARDING_TYPE_UNCONDITIONAL)
					update_sim = TRUE;
			}

			if (type == CALL_FORWARDING_TYPE_NO_REPLY &&
				oc->time != lc->time)
				ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_FORWARDING_INTERFACE,
						tattr, DBUS_TYPE_UINT16,
						&timeout);

			/* Remove from the old list */
			g_free(o->data);
			old = g_slist_remove(old, o->data);
		} else {
			number = phone_number_to_string(&lc->phone_number);

			ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_FORWARDING_INTERFACE,
						attr, DBUS_TYPE_STRING,
						&number);

			if (type == CALL_FORWARDING_TYPE_UNCONDITIONAL)
				update_sim = TRUE;

			if (type == CALL_FORWARDING_TYPE_NO_REPLY &&
				lc->time != DEFAULT_NO_REPLY_TIMEOUT)
				ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_FORWARDING_INTERFACE,
						tattr, DBUS_TYPE_UINT16,
						&timeout);
		}
	}

	timeout = DEFAULT_NO_REPLY_TIMEOUT;
	number = "";

	for (o = old; o; o = o->next) {
		oc = o->data;

		/*
		 * For now we only support Voice, although Fax & all Data
		 * basic services are applicable as well.
		 */
		if (oc->cls > BEARER_CLASS_VOICE)
			continue;

		snprintf(attr, sizeof(attr), "%s%s",
			bearer_class_to_string(oc->cls), cf_type_lut[type]);

		if (type == CALL_FORWARDING_TYPE_NO_REPLY)
			snprintf(tattr, sizeof(tattr), "%sTimeout", attr);

		ofono_dbus_signal_property_changed(conn, path,
					OFONO_CALL_FORWARDING_INTERFACE, attr,
					DBUS_TYPE_STRING, &number);

		if (type == CALL_FORWARDING_TYPE_UNCONDITIONAL)
			update_sim = TRUE;

		if (type == CALL_FORWARDING_TYPE_NO_REPLY &&
			oc->time != DEFAULT_NO_REPLY_TIMEOUT)
			ofono_dbus_signal_property_changed(conn, path,
						OFONO_CALL_FORWARDING_INTERFACE,
						tattr, DBUS_TYPE_UINT16,
						&timeout);
	}

	cf_list_clear(old);
	cf->cf_conditions[type] = list;

	if (update_sim == TRUE)
		sim_set_cf_indicator(cf);

	if ((cf->flags & CALL_FORWARDING_FLAG_CPHS_CFF) ||
			cf->cfis_record_id > 0)
		new_cfu = is_cfu_enabled(cf, NULL);
	else
		new_cfu = FALSE;

	if (new_cfu != old_cfu) {
		ofono_bool_t status = new_cfu;

		ofono_dbus_signal_property_changed(conn, path,
					OFONO_CALL_FORWARDING_INTERFACE,
					"ForwardingFlagOnSim",
					DBUS_TYPE_BOOLEAN, &status);
	}
}

static inline void property_append_cf_condition(DBusMessageIter *dict, int cls,
						const char *postfix,
						const char *value,
						dbus_uint16_t timeout)
{
	char attr[64];
	char tattr[64];
	int addt = !strcmp(postfix, "NoReply");

	snprintf(attr, sizeof(attr), "%s%s",
			bearer_class_to_string(cls), postfix);

	if (addt)
		snprintf(tattr, sizeof(tattr), "%s%sTimeout",
				bearer_class_to_string(cls), postfix);

	ofono_dbus_dict_append(dict, attr, DBUS_TYPE_STRING, &value);

	if (addt)
		ofono_dbus_dict_append(dict, tattr, DBUS_TYPE_UINT16, &timeout);
}

static void property_append_cf_conditions(DBusMessageIter *dict,
						GSList *cf_list, int mask,
						const char *postfix)
{
	GSList *l;
	int i;
	struct ofono_call_forwarding_condition *cf;
	const char *number;

	for (i = 1, l = cf_list; i <= BEARER_CLASS_PAD; i = i << 1) {
		if (!(mask & i))
			continue;

		while (l && (cf = l->data) && (cf->cls < i))
				l = l->next;

		if (l == NULL || cf->cls != i) {
			property_append_cf_condition(dict, i, postfix, "",
						DEFAULT_NO_REPLY_TIMEOUT);
			continue;
		}

		number = phone_number_to_string(&cf->phone_number);

		property_append_cf_condition(dict, i, postfix, number,
						cf->time);
	}
}

static DBusMessage *cf_get_properties_reply(DBusMessage *msg,
						struct ofono_call_forwarding *cf)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	int i;
	dbus_bool_t status;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	for (i = 0; i < 4; i++)
		property_append_cf_conditions(&dict, cf->cf_conditions[i],
						BEARER_CLASS_VOICE,
						cf_type_lut[i]);

	if ((cf->flags & CALL_FORWARDING_FLAG_CPHS_CFF) ||
			cf->cfis_record_id > 0)
		status = is_cfu_enabled(cf, NULL);
	else
		status = FALSE;

	ofono_dbus_dict_append(&dict, "ForwardingFlagOnSim", DBUS_TYPE_BOOLEAN,
					&status);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void get_query_cf_callback(const struct ofono_error *error, int total,
			const struct ofono_call_forwarding_condition *list,
			void *data)
{
	struct ofono_call_forwarding *cf = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		GSList *l;
		l = cf_cond_list_create(total, list);
		set_new_cond_list(cf, cf->query_next, l);

		DBG("%s conditions:", cf_type_lut[cf->query_next]);
		cf_cond_list_print(l);

		if (cf->query_next == CALL_FORWARDING_TYPE_NOT_REACHABLE)
			cf->flags |= CALL_FORWARDING_FLAG_CACHED;
	}

	if (cf->query_next == CALL_FORWARDING_TYPE_NOT_REACHABLE) {
		DBusMessage *reply = cf_get_properties_reply(cf->pending, cf);
		__ofono_dbus_pending_reply(&cf->pending, reply);
		return;
	}

	cf->query_next++;
	get_query_next_cf_cond(cf);
}

static void get_query_next_cf_cond(struct ofono_call_forwarding *cf)
{
	cf->driver->query(cf, cf->query_next, BEARER_CLASS_DEFAULT,
			get_query_cf_callback, cf);
}

static DBusMessage *cf_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_forwarding *cf = data;
	struct ofono_modem *modem = __ofono_atom_get_modem(cf->atom);

	if ((cf->flags & CALL_FORWARDING_FLAG_CACHED) ||
			ofono_modem_get_online(modem) == FALSE)
		return cf_get_properties_reply(msg, cf);

	if (cf->driver->query == NULL)
		return __ofono_error_not_implemented(msg);

	if (__ofono_call_forwarding_is_busy(cf) ||
			__ofono_ussd_is_busy(cf->ussd))
		return __ofono_error_busy(msg);

	cf->pending = dbus_message_ref(msg);
	cf->query_next = 0;

	get_query_next_cf_cond(cf);

	return NULL;
}

static gboolean cf_condition_enabled_property(struct ofono_call_forwarding *cf,
			const char *property, int *out_type, int *out_cls)
{
	int i;
	int j;
	int len;
	const char *prefix;

	for (i = 1; i <= BEARER_CLASS_VOICE; i = i << 1) {
		prefix = bearer_class_to_string(i);

		len = strlen(prefix);

		if (strncmp(property, prefix, len))
			continue;

		/*
		 * We check the 4 call forwarding types, e.g.
		 * unconditional, busy, no reply, not reachable
		 */
		for (j = 0; j < 4; j++)
			if (!strcmp(property+len, cf_type_lut[j])) {
				*out_type = j;
				*out_cls = i;
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

	for (i = 1; i <= BEARER_CLASS_VOICE; i = i << 1) {
		prefix = bearer_class_to_string(i);

		len = strlen(prefix);

		if (strncmp(property, prefix, len))
			continue;

		if (!strcmp(property+len, "NoReplyTimeout")) {
			*out_cls = i;
			return TRUE;
		}
	}

	return FALSE;
}

static void set_query_cf_callback(const struct ofono_error *error, int total,
			const struct ofono_call_forwarding_condition *list,
			void *data)
{
	struct ofono_call_forwarding *cf = data;
	GSList *l;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Setting succeeded, but query failed");
		cf->flags &= ~CALL_FORWARDING_FLAG_CACHED;
		reply = __ofono_error_failed(cf->pending);
		__ofono_dbus_pending_reply(&cf->pending, reply);
		return;
	}

	if (cf->query_next == cf->query_end) {
		reply = dbus_message_new_method_return(cf->pending);
		__ofono_dbus_pending_reply(&cf->pending, reply);
	}

	l = cf_cond_list_create(total, list);
	set_new_cond_list(cf, cf->query_next, l);

	DBG("%s conditions:", cf_type_lut[cf->query_next]);
	cf_cond_list_print(l);

	if (cf->query_next != cf->query_end) {
		cf->query_next++;
		set_query_next_cf_cond(cf);
	}
}

static void set_query_next_cf_cond(struct ofono_call_forwarding *cf)
{
	cf->driver->query(cf, cf->query_next, BEARER_CLASS_DEFAULT,
			set_query_cf_callback, cf);
}

static void set_property_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_forwarding *cf = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error occurred during set/erasure");
		__ofono_dbus_pending_reply(&cf->pending,
					__ofono_error_failed(cf->pending));
		return;
	}

	/* Successfully set, query the entire set just in case */
	set_query_next_cf_cond(cf);
}

static DBusMessage *set_property_request(struct ofono_call_forwarding *cf,
						DBusMessage *msg,
						int type, int cls,
						struct ofono_phone_number *ph,
						int timeout)
{
	if (ph->number[0] != '\0' && cf->driver->registration == NULL)
		return __ofono_error_not_implemented(msg);

	if (ph->number[0] == '\0' && cf->driver->erasure == NULL)
		return __ofono_error_not_implemented(msg);

	cf->pending = dbus_message_ref(msg);
	cf->query_next = type;
	cf->query_end = type;

	DBG("Farming off request, will be erasure: %d", ph->number[0] == '\0');

	if (ph->number[0] != '\0')
		cf->driver->registration(cf, type, cls, ph, timeout,
					set_property_callback, cf);
	else
		cf->driver->erasure(cf, type, cls, set_property_callback, cf);

	return NULL;
}

static DBusMessage *cf_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_forwarding *cf = data;
	struct ofono_modem *modem = __ofono_atom_get_modem(cf->atom);
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	int cls;
	int type;

	if (ofono_modem_get_online(modem) == FALSE)
		return __ofono_error_not_available(msg);

	if (__ofono_call_forwarding_is_busy(cf) ||
			__ofono_ussd_is_busy(cf->ussd))
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

	if (cf_condition_timeout_property(property, &cls)) {
		dbus_uint16_t timeout;
		GSList *l;
		struct ofono_call_forwarding_condition *c;

		type = CALL_FORWARDING_TYPE_NO_REPLY;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_UINT16)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &timeout);

		if (timeout < 1 || timeout > 30)
			return __ofono_error_invalid_format(msg);

		l = g_slist_find_custom(cf->cf_conditions[type],
				GINT_TO_POINTER(cls),
				cf_condition_find_with_cls);

		if (l == NULL)
			return __ofono_error_failed(msg);

		c = l->data;

		return set_property_request(cf, msg, type, cls,
						&c->phone_number, timeout);
	} else if (cf_condition_enabled_property(cf, property, &type, &cls)) {
		struct ofono_phone_number ph;
		const char *number;
		int timeout;

		ph.number[0] = '\0';
		ph.type = 129;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &number);

		if (strlen(number) > 0 && !valid_phone_number_format(number))
			return __ofono_error_invalid_format(msg);

		if (number[0] != '\0')
			string_to_phone_number(number, &ph);

		timeout = cf_find_timeout(cf->cf_conditions[type], cls);

		return set_property_request(cf, msg, type, cls, &ph,
						timeout);
	}

	return __ofono_error_invalid_args(msg);
}

static void disable_conditional_callback(const struct ofono_error *error,
						void *data)
{
	struct ofono_call_forwarding *cf = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error occurred during conditional erasure");

		__ofono_dbus_pending_reply(&cf->pending,
					__ofono_error_failed(cf->pending));
		return;
	}

	/* Query the three conditional cf types */
	cf->query_next = CALL_FORWARDING_TYPE_BUSY;
	cf->query_end = CALL_FORWARDING_TYPE_NOT_REACHABLE;
	set_query_next_cf_cond(cf);
}

static void disable_all_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_forwarding *cf = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error occurred during erasure of all");

		__ofono_dbus_pending_reply(&cf->pending,
					__ofono_error_failed(cf->pending));
		return;
	}

	/* Query all cf types */
	cf->query_next = CALL_FORWARDING_TYPE_UNCONDITIONAL;
	cf->query_end = CALL_FORWARDING_TYPE_NOT_REACHABLE;
	set_query_next_cf_cond(cf);
}

static DBusMessage *cf_disable_all(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_call_forwarding *cf = data;
	const char *strtype;
	int type;

	if (cf->driver->erasure == NULL)
		return __ofono_error_not_implemented(msg);

	if (__ofono_call_forwarding_is_busy(cf) ||
			__ofono_ussd_is_busy(cf->ussd))
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &strtype,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!strcmp(strtype, "all") || !strcmp(strtype, ""))
		type = CALL_FORWARDING_TYPE_ALL;
	else if (!strcmp(strtype, "conditional"))
		type = CALL_FORWARDING_TYPE_ALL_CONDITIONAL;
	else
		return __ofono_error_invalid_format(msg);

	cf->pending = dbus_message_ref(msg);

	if (type == CALL_FORWARDING_TYPE_ALL)
		cf->driver->erasure(cf, type, BEARER_CLASS_DEFAULT,
				disable_all_callback, cf);
	else
		cf->driver->erasure(cf, type, BEARER_CLASS_DEFAULT,
				disable_conditional_callback, cf);

	return NULL;
}

static GDBusMethodTable cf_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cf_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
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

static DBusMessage *cf_ss_control_reply(struct ofono_call_forwarding *cf,
					struct cf_ss_request *req)
{
	const char *context = "CallForwarding";
	const char *sig = "(ssa{sv})";
	const char *ss_type = ss_control_type_to_string(req->ss_type);
	const char *cf_type = cf_type_lut[req->cf_type];
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
				OFONO_PROPERTIES_ARRAY_SIGNATURE, &dict);

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

	return reply;
}

static void ss_set_query_cf_callback(const struct ofono_error *error, int total,
			const struct ofono_call_forwarding_condition *list,
			void *data)
{
	struct ofono_call_forwarding *cf = data;
	GSList *l;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Setting succeeded, but query failed");
		cf->flags &= ~CALL_FORWARDING_FLAG_CACHED;
		reply = __ofono_error_failed(cf->pending);
		__ofono_dbus_pending_reply(&cf->pending, reply);
		return;
	}

	l = cf_cond_list_create(total, list);
	DBG("%s conditions:", cf_type_lut[cf->query_next]);
	cf_cond_list_print(l);

	cf->ss_req->cf_list[cf->query_next] = l;

	if (cf->query_next == cf->query_end) {
		reply = cf_ss_control_reply(cf, cf->ss_req);
		__ofono_dbus_pending_reply(&cf->pending, reply);
		g_free(cf->ss_req);
		cf->ss_req = NULL;
	}

	set_new_cond_list(cf, cf->query_next, l);

	if (cf->query_next != cf->query_end) {
		cf->query_next++;
		ss_set_query_next_cf_cond(cf);
	}
}

static void ss_set_query_next_cf_cond(struct ofono_call_forwarding *cf)
{
	cf->driver->query(cf, cf->query_next, BEARER_CLASS_DEFAULT,
			ss_set_query_cf_callback, cf);
}

static void cf_ss_control_callback(const struct ofono_error *error, void *data)
{
	struct ofono_call_forwarding *cf = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error occurred during cf ss control set/erasure");

		__ofono_dbus_pending_reply(&cf->pending,
					__ofono_error_failed(cf->pending));
		g_free(cf->ss_req);
		cf->ss_req = NULL;
		return;
	}

	ss_set_query_next_cf_cond(cf);
}

static gboolean cf_ss_control(int type, const char *sc,
				const char *sia, const char *sib,
				const char *sic, const char *dn,
				DBusMessage *msg, void *data)
{
	struct ofono_call_forwarding *cf = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	int cls = BEARER_CLASS_SS_DEFAULT;
	int timeout = DEFAULT_NO_REPLY_TIMEOUT;
	int cf_type;
	DBusMessage *reply;
	struct ofono_phone_number ph;
	void *operation = NULL;

	/* Before we do anything, make sure we're actually initialized */
	if (cf == NULL)
		return FALSE;

	if (__ofono_call_forwarding_is_busy(cf)) {
		reply = __ofono_error_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	DBG("Received call forwarding ss control request");

	DBG("type: %d, sc: %s, sia: %s, sib: %s, sic: %s, dn: %s",
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

	/*
	 * Activation / Registration is figured context specific according to
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
		operation = cf->driver->registration;
		break;
	case SS_CONTROL_TYPE_ACTIVATION:
		operation = cf->driver->activation;
		break;
	case SS_CONTROL_TYPE_DEACTIVATION:
		operation = cf->driver->deactivation;
		break;
	case SS_CONTROL_TYPE_ERASURE:
		operation = cf->driver->erasure;
		break;
	case SS_CONTROL_TYPE_QUERY:
		operation = cf->driver->query;
		break;
	}

	if (operation == NULL) {
		reply = __ofono_error_not_implemented(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	cf->ss_req = g_try_new0(struct cf_ss_request, 1);

	if (cf->ss_req == NULL) {
		reply = __ofono_error_failed(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	cf->ss_req->ss_type = type;
	cf->ss_req->cf_type = cf_type;
	cf->ss_req->cls = cls;

	cf->pending = dbus_message_ref(msg);

	switch (cf->ss_req->cf_type) {
	case CALL_FORWARDING_TYPE_ALL:
		cf->query_next = CALL_FORWARDING_TYPE_UNCONDITIONAL;
		cf->query_end = CALL_FORWARDING_TYPE_NOT_REACHABLE;
		break;
	case CALL_FORWARDING_TYPE_ALL_CONDITIONAL:
		cf->query_next = CALL_FORWARDING_TYPE_BUSY;
		cf->query_end = CALL_FORWARDING_TYPE_NOT_REACHABLE;
		break;
	default:
		cf->query_next = cf->ss_req->cf_type;
		cf->query_end = cf->ss_req->cf_type;
		break;
	}

	/*
	 * Some modems don't understand all classes very well, particularly
	 * the older models.  So if the bearer class is the default, we
	 * just use the more commonly understood value of 7 since BEARER_SMS
	 * is not applicable to CallForwarding conditions according to 22.004
	 * Annex A
	 */
	if (cls == BEARER_CLASS_SS_DEFAULT)
		cls = BEARER_CLASS_DEFAULT;

	switch (cf->ss_req->ss_type) {
	case SS_CONTROL_TYPE_REGISTRATION:
		string_to_phone_number(sia, &ph);
		cf->driver->registration(cf, cf_type, cls, &ph, timeout,
					cf_ss_control_callback, cf);
		break;
	case SS_CONTROL_TYPE_ACTIVATION:
		cf->driver->activation(cf, cf_type, cls, cf_ss_control_callback,
					cf);
		break;
	case SS_CONTROL_TYPE_DEACTIVATION:
		cf->driver->deactivation(cf, cf_type, cls,
					cf_ss_control_callback, cf);
		break;
	case SS_CONTROL_TYPE_ERASURE:
		cf->driver->erasure(cf, cf_type, cls, cf_ss_control_callback,
					cf);
		break;
	case SS_CONTROL_TYPE_QUERY:
		ss_set_query_next_cf_cond(cf);
		break;
	}

	return TRUE;

error:
	reply = __ofono_error_invalid_format(msg);
	g_dbus_send_message(conn, reply);
	return TRUE;
}

static void cf_register_ss_controls(struct ofono_call_forwarding *cf)
{
	__ofono_ussd_ssc_register(cf->ussd, "21", cf_ss_control, cf, NULL);
	__ofono_ussd_ssc_register(cf->ussd, "67", cf_ss_control, cf, NULL);
	__ofono_ussd_ssc_register(cf->ussd, "61", cf_ss_control, cf, NULL);
	__ofono_ussd_ssc_register(cf->ussd, "62", cf_ss_control, cf, NULL);

	__ofono_ussd_ssc_register(cf->ussd, "002", cf_ss_control, cf, NULL);
	__ofono_ussd_ssc_register(cf->ussd, "004", cf_ss_control, cf, NULL);
}

static void cf_unregister_ss_controls(struct ofono_call_forwarding *cf)
{
	__ofono_ussd_ssc_unregister(cf->ussd, "21");
	__ofono_ussd_ssc_unregister(cf->ussd, "67");
	__ofono_ussd_ssc_unregister(cf->ussd, "61");
	__ofono_ussd_ssc_unregister(cf->ussd, "62");

	__ofono_ussd_ssc_unregister(cf->ussd, "002");
	__ofono_ussd_ssc_unregister(cf->ussd, "004");
}

gboolean __ofono_call_forwarding_is_busy(struct ofono_call_forwarding *cf)
{
	return cf->pending ? TRUE : FALSE;
}

static void sim_cfis_read_cb(int ok, int total_length, int record,
			const unsigned char *data,
			int record_length, void *userdata)
{
	struct ofono_call_forwarding *cf = userdata;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cf->atom);

	if (!ok || record_length < 16 || total_length < record_length) {
		cf->cfis_record_id = 0;
		return;
	}

	/*
	 * Multiple Subscriber Profile number which can have values 1-4.
	 * Profile id 1 is assumed as the current profile.
	 */
	if (data[0] != 1)
		return;

	cf->cfis_record_id = record;

	if (cf->flags & CALL_FORWARDING_FLAG_CACHED)
		return;

	/*
	 * For now we only support Voice, although Fax & all Data
	 * basic services are applicable as well.
	 */
	if (data[1] & 0x01) {
		int ton_npi;
		int number_len;
		const char *number;
		char attr[64];
		struct ofono_call_forwarding_condition *cond;
		dbus_bool_t status;

		number_len = data[2];
		ton_npi = data[3];

		if (number_len > 11 || ton_npi == 0xff)
			return;

		cond = g_try_new0(struct ofono_call_forwarding_condition, 1);
		if (cond == NULL)
			return;

		status = TRUE;
		cond->status = TRUE;
		cond->cls = BEARER_CLASS_VOICE;
		cond->time = 0;
		cond->phone_number.type = ton_npi;

		sim_extract_bcd_number(data + 4, number_len - 1,
					cond->phone_number.number);
		number = phone_number_to_string(&cond->phone_number);

		snprintf(attr, sizeof(attr), "%s%s",
			bearer_class_to_string(BEARER_CLASS_VOICE),
			cf_type_lut[CALL_FORWARDING_TYPE_UNCONDITIONAL]);

		cf->cf_conditions[CALL_FORWARDING_TYPE_UNCONDITIONAL] =
						g_slist_append(NULL, cond);

		ofono_dbus_signal_property_changed(conn, path,
					OFONO_CALL_FORWARDING_INTERFACE,
					attr, DBUS_TYPE_STRING, &number);

		ofono_dbus_signal_property_changed(conn, path,
					OFONO_CALL_FORWARDING_INTERFACE,
					"ForwardingFlagOnSim",
					DBUS_TYPE_BOOLEAN, &status);
	}
}

static void sim_cphs_cff_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_call_forwarding *cf = userdata;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cf->atom);
	dbus_bool_t cfu_voice;

	if (!ok || total_length < 1)
		return;

	cf->flags |= CALL_FORWARDING_FLAG_CPHS_CFF;

	if (cf->flags & CALL_FORWARDING_FLAG_CACHED)
		return;

	/*
	 * For now we only support Voice, although Fax & all Data
	 * basic services are applicable as well.
	 */
	if ((data[0] & 0xf) != 0xA)
		return;

	cfu_voice = TRUE;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CALL_FORWARDING_INTERFACE,
					"ForwardingFlagOnSim",
					DBUS_TYPE_BOOLEAN, &cfu_voice);
}

static void call_forwarding_unregister(struct ofono_atom *atom)
{
	struct ofono_call_forwarding *cf = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(cf->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cf->atom);

	ofono_modem_remove_interface(modem, OFONO_CALL_FORWARDING_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					OFONO_CALL_FORWARDING_INTERFACE);

	if (cf->sim_context) {
		ofono_sim_context_free(cf->sim_context);
		cf->sim_context = NULL;
	}

	if (cf->ussd)
		cf_unregister_ss_controls(cf);

	if (cf->ussd_watch)
		__ofono_modem_remove_atom_watch(modem, cf->ussd_watch);

	cf->flags = 0;
}

static void sim_cfis_changed(int id, void *userdata)
{
	struct ofono_call_forwarding *cf = userdata;

	if (!(cf->flags & CALL_FORWARDING_FLAG_CACHED))
		return;

	/*
	 * If the values are cached it's because at least one client
	 * requested them and we need to notify them about this
	 * change.  However the authoritative source of current
	 * Call-Forwarding settings is the network operator and the
	 * query can take a noticeable amount of time.  Instead of
	 * sending PropertyChanged, we reregister the Call Forwarding
	 * atom.  The client will invoke GetProperties only if it
	 * is still interested.
	 */
	call_forwarding_unregister(cf->atom);
	ofono_call_forwarding_register(cf);
}

static void sim_read_cf_indicator(struct ofono_call_forwarding *cf)
{
	if (__ofono_sim_service_available(cf->sim,
			SIM_UST_SERVICE_CFIS,
			SIM_SST_SERVICE_CFIS) == TRUE) {
		ofono_sim_read(cf->sim_context, SIM_EFCFIS_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				sim_cfis_read_cb, cf);
		ofono_sim_add_file_watch(cf->sim_context, SIM_EFCFIS_FILEID,
						sim_cfis_changed, cf, NULL);
	} else {
		ofono_sim_read(cf->sim_context, SIM_EF_CPHS_CFF_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				sim_cphs_cff_read_cb, cf);
		ofono_sim_add_file_watch(cf->sim_context,
						SIM_EF_CPHS_CFF_FILEID,
						sim_cfis_changed, cf, NULL);
	}
}

int ofono_call_forwarding_driver_register(const struct ofono_call_forwarding_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_call_forwarding_driver_unregister(const struct ofono_call_forwarding_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void call_forwarding_remove(struct ofono_atom *atom)
{
	struct ofono_call_forwarding *cf = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cf == NULL)
		return;

	if (cf->driver && cf->driver->remove)
		cf->driver->remove(cf);

	cf_clear_all(cf);

	g_free(cf);
}

struct ofono_call_forwarding *ofono_call_forwarding_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data)
{
	struct ofono_call_forwarding *cf;
	GSList *l;

	if (driver == NULL)
		return NULL;

	cf = g_try_new0(struct ofono_call_forwarding, 1);

	if (cf == NULL)
		return NULL;

	cf->atom = __ofono_modem_add_atom(modem,
						OFONO_ATOM_TYPE_CALL_FORWARDING,
						call_forwarding_remove, cf);
	for (l = g_drivers; l; l = l->next) {
		const struct ofono_call_forwarding_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cf, vendor, data) < 0)
			continue;

		cf->driver = drv;
		break;
	}

	return cf;
}

static void ussd_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *data)
{
	struct ofono_call_forwarding *cf = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		cf->ussd = NULL;
		return;
	}

	cf->ussd = __ofono_atom_get_data(atom);
	cf_register_ss_controls(cf);
}

void ofono_call_forwarding_register(struct ofono_call_forwarding *cf)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cf->atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(cf->atom);
	struct ofono_atom *sim_atom;

	if (!g_dbus_register_interface(conn, path,
					OFONO_CALL_FORWARDING_INTERFACE,
					cf_methods, cf_signals, NULL, cf,
					NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CALL_FORWARDING_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_CALL_FORWARDING_INTERFACE);

	sim_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SIM);

	if (sim_atom) {
		cf->sim = __ofono_atom_get_data(sim_atom);
		cf->sim_context = ofono_sim_context_create(cf->sim);

		sim_read_cf_indicator(cf);
	}

	cf->ussd_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_USSD,
					ussd_watch, cf, NULL);

	__ofono_atom_register(cf->atom, call_forwarding_unregister);
}

void ofono_call_forwarding_remove(struct ofono_call_forwarding *cf)
{
	__ofono_atom_free(cf->atom);
}

void ofono_call_forwarding_set_data(struct ofono_call_forwarding *cf, void *data)
{
	cf->driver_data = data;
}

void *ofono_call_forwarding_get_data(struct ofono_call_forwarding *cf)
{
	return cf->driver_data;
}
