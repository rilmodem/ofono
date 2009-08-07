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
#include <time.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "cssn.h"

#define VOICECALL_MANAGER_INTERFACE "org.ofono.VoiceCallManager"
#define VOICECALL_INTERFACE "org.ofono.VoiceCall"

#define VOICECALLS_FLAG_PENDING 0x1
#define VOICECALLS_FLAG_MULTI_RELEASE 0x2

#define MAX_VOICE_CALLS 16

struct voicecalls_data {
	GSList *call_list;
	GSList *release_list;
	GSList *multiparty_list;
	struct ofono_voicecall_ops *ops;
	int flags;
	DBusMessage *pending;
	gint emit_calls_source;
	gint emit_multi_source;
};

struct voicecall {
	struct ofono_call *call;
	struct ofono_modem *modem;
	time_t start_time;
	time_t detect_time;
};

static void generic_callback(const struct ofono_error *error, void *data);
static void dial_callback(const struct ofono_error *error, void *data);
static void multirelease_callback(const struct ofono_error *err, void *data);
static void multiparty_create_callback(const struct ofono_error *error,
					void *data);
static void private_chat_callback(const struct ofono_error *error, void *data);

static gint call_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = ((struct voicecall *)a)->call;
	unsigned int id = GPOINTER_TO_UINT(b);

	if (id < call->id)
		return -1;

	if (id > call->id)
		return 1;

	return 0;
}

static gint call_compare(gconstpointer a, gconstpointer b)
{
	const struct voicecall *ca = a;
	const struct voicecall *cb = b;

	if (ca->call->id < cb->call->id)
		return -1;

	if (ca->call->id > cb->call->id)
		return 1;

	return 0;
}

static const char *call_status_to_string(int status)
{
	switch (status) {
	case CALL_STATUS_ACTIVE:
		return "active";
	case CALL_STATUS_HELD:
		return "held";
	case CALL_STATUS_DIALING:
		return "dialing";
	case CALL_STATUS_ALERTING:
	return "alerting";
	case CALL_STATUS_INCOMING:
		return "incoming";
	case CALL_STATUS_WAITING:
		return "waiting";
	default:
		return "disconnected";
	}
}

static const char *phone_and_clip_to_string(const struct ofono_phone_number *n,
						int clip_validity)
{
	if (clip_validity == CLIP_VALIDITY_WITHHELD && !strlen(n->number))
		return "withheld";

	if (clip_validity == CLIP_VALIDITY_NOT_AVAILABLE)
		return "";

	return phone_number_to_string(n);
}

static const char *time_to_str(const time_t *t)
{
	static char buf[128];

	strftime(buf, 127, "%a, %d %b %Y %H:%M:%S %z", localtime(t));
	buf[127] = '\0';

	return buf;
}

static DBusMessage *voicecall_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_call *call = v->call;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *status;
	const char *callerid;
	const char *timestr = "";

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	status = call_status_to_string(call->status);
	callerid = phone_number_to_string(&call->phone_number);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "State", DBUS_TYPE_STRING, &status);

	ofono_dbus_dict_append(&dict, "LineIdentification",
				DBUS_TYPE_STRING, &callerid);

	if (call->status == CALL_STATUS_ACTIVE ||
		(call->status == CALL_STATUS_DISCONNECTED && v->start_time != 0) ||
		call->status == CALL_STATUS_HELD) {
		timestr = time_to_str(&v->start_time);

		ofono_dbus_dict_append(&dict, "StartTime", DBUS_TYPE_STRING,
					&timestr);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *voicecall_busy(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_modem *modem = v->modem;
	struct voicecalls_data *voicecalls = modem->voicecalls;
	struct ofono_call *call = v->call;

	if (call->status != CALL_STATUS_INCOMING &&
		call->status != CALL_STATUS_WAITING)
		return __ofono_error_failed(msg);

	if (!voicecalls->ops->set_udub)
		return __ofono_error_not_implemented(msg);

	if (voicecalls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	voicecalls->flags |= VOICECALLS_FLAG_PENDING;
	voicecalls->pending = dbus_message_ref(msg);

	voicecalls->ops->set_udub(modem, generic_callback, voicecalls);

	return NULL;
}

static DBusMessage *voicecall_deflect(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_modem *modem = v->modem;
	struct voicecalls_data *voicecalls = modem->voicecalls;
	struct ofono_call *call = v->call;

	struct ofono_phone_number ph;
	const char *number;

	if (call->status != CALL_STATUS_INCOMING &&
		call->status != CALL_STATUS_WAITING)
		return __ofono_error_failed(msg);

	if (!voicecalls->ops->deflect)
		return __ofono_error_not_implemented(msg);

	if (voicecalls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &number,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!valid_phone_number_format(number))
		return __ofono_error_invalid_format(msg);

	voicecalls->flags |= VOICECALLS_FLAG_PENDING;
	voicecalls->pending = dbus_message_ref(msg);

	string_to_phone_number(number, &ph);

	voicecalls->ops->deflect(modem, &ph, generic_callback, voicecalls);

	return NULL;
}

static DBusMessage *voicecall_hangup(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_modem *modem = v->modem;
	struct voicecalls_data *voicecalls = modem->voicecalls;
	struct ofono_call *call = v->call;

	if (call->status == CALL_STATUS_DISCONNECTED)
		return __ofono_error_failed(msg);

	if (!voicecalls->ops->release_specific)
		return __ofono_error_not_implemented(msg);

	if (voicecalls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	voicecalls->flags |= VOICECALLS_FLAG_PENDING;
	voicecalls->pending = dbus_message_ref(msg);

	voicecalls->ops->release_specific(modem, call->id,
						generic_callback, voicecalls);

	return NULL;
}

static DBusMessage *voicecall_answer(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_modem *modem = v->modem;
	struct voicecalls_data *voicecalls = modem->voicecalls;
	struct ofono_call *call = v->call;

	if (call->status != CALL_STATUS_INCOMING)
		return __ofono_error_failed(msg);

	if (!voicecalls->ops->answer)
		return __ofono_error_not_implemented(msg);

	if (voicecalls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	voicecalls->flags |= VOICECALLS_FLAG_PENDING;
	voicecalls->pending = dbus_message_ref(msg);

	voicecalls->ops->answer(modem, generic_callback, voicecalls);

	return NULL;
}

static GDBusMethodTable voicecall_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	voicecall_get_properties },
	{ "Busy",		"",	"",		voicecall_busy,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "Deflect",		"s",	"",		voicecall_deflect,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "Hangup",		"",	"",		voicecall_hangup,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "Answer",		"",	"",		voicecall_answer,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable voicecall_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ "DisconnectReason",	"s" },
	{ }
};

static struct voicecall *voicecall_create(struct ofono_modem *modem,
						struct ofono_call *call)
{
	struct voicecall *v;

	v = g_try_new0(struct voicecall, 1);

	if (!v)
		return NULL;

	v->call = call;
	v->modem = modem;

	return v;
}

static void voicecall_destroy(gpointer userdata)
{
	struct voicecall *voicecall = (struct voicecall *)userdata;

	g_free(voicecall->call);

	g_free(voicecall);
}

static const char *voicecall_build_path(struct ofono_modem *modem,
					const struct ofono_call *call)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/voicecall%02d",
			modem->path, call->id);

	return path;
}

static void voicecall_set_call_status(struct ofono_modem *modem,
					struct voicecall *call,
					int status)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *status_str;
	int old_status;

	if (call->call->status == status)
		return;

	old_status = call->call->status;

	call->call->status = status;

	status_str = call_status_to_string(status);
	path = voicecall_build_path(modem, call->call);

	ofono_dbus_signal_property_changed(conn, path, VOICECALL_INTERFACE,
						"State", DBUS_TYPE_STRING,
						&status_str);

	if (status == CALL_STATUS_ACTIVE &&
		(old_status == CALL_STATUS_INCOMING ||
			old_status == CALL_STATUS_DIALING ||
			old_status == CALL_STATUS_ALERTING ||
			old_status == CALL_STATUS_WAITING)) {
		const char *timestr;

		call->start_time = time(NULL);
		timestr = time_to_str(&call->start_time);

		ofono_dbus_signal_property_changed(conn, path,
							VOICECALL_INTERFACE,
							"StartTime",
							DBUS_TYPE_STRING,
							&timestr);
	}
}

static void voicecall_set_call_lineid(struct ofono_modem *modem,
					struct voicecall *v,
					const struct ofono_phone_number *ph,
					int clip_validity)
{
	struct ofono_call *call = v->call;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *lineid_str;

	if (!strcmp(call->phone_number.number, ph->number) &&
		call->phone_number.type == ph->type &&
		call->clip_validity == clip_validity)
		return;

	/* Two cases: We get an incoming call with CLIP factored in, or
	 * CLIP comes in later as a separate event
	 * For COLP only the phone number should be checked, it can come
	 * in with the initial call event or later as a separate event */

	/* For plugins that don't keep state, ignore */
	if (call->clip_validity == CLIP_VALIDITY_VALID &&
		clip_validity == CLIP_VALIDITY_NOT_AVAILABLE)
		return;

	strcpy(call->phone_number.number, ph->number);
	call->clip_validity = clip_validity;
	call->phone_number.type = ph->type;

	path = voicecall_build_path(modem, call);

	if (call->direction == CALL_DIRECTION_MOBILE_TERMINATED)
		lineid_str = phone_and_clip_to_string(ph, clip_validity);
	else
		lineid_str = phone_number_to_string(ph);

	ofono_dbus_signal_property_changed(conn, path, VOICECALL_INTERFACE,
						"LineIdentification",
						DBUS_TYPE_STRING, &lineid_str);
}

static gboolean voicecall_dbus_register(struct voicecall *voicecall)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;

	if (!voicecall)
		return FALSE;

	path = voicecall_build_path(voicecall->modem, voicecall->call);

	if (!g_dbus_register_interface(conn, path, VOICECALL_INTERFACE,
					voicecall_methods,
					voicecall_signals,
					NULL, voicecall,
					voicecall_destroy)) {
		ofono_error("Could not register VoiceCall %s", path);
		voicecall_destroy(voicecall);

		return FALSE;
	}

	return TRUE;
}

static gboolean voicecall_dbus_unregister(struct ofono_modem *modem,
						struct voicecall *call)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = voicecall_build_path(modem, call->call);

	return g_dbus_unregister_interface(conn, path,
					VOICECALL_INTERFACE);
}

static struct voicecalls_data *voicecalls_create()
{
	struct voicecalls_data *calls;

	calls = g_try_new0(struct voicecalls_data, 1);

	return calls;
}

static void voicecalls_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct voicecalls_data *calls = modem->voicecalls;
	GSList *l;

	if (calls->emit_calls_source) {
		g_source_remove(calls->emit_calls_source);
		calls->emit_calls_source = 0;
	}

	if (calls->emit_multi_source) {
		g_source_remove(calls->emit_multi_source);
		calls->emit_multi_source = 0;
	}

	for (l = calls->call_list; l; l = l->next)
		voicecall_dbus_unregister(modem, l->data);

	g_slist_free(calls->call_list);

	g_free(calls);

	modem->voicecalls = 0;
}

static int voicecalls_path_list(struct ofono_modem *modem, GSList *call_list,
				char ***objlist)
{
	GSList *l;
	int i;
	struct voicecall *v;

	*objlist = g_new0(char *, g_slist_length(call_list) + 1);

	if (*objlist == NULL)
		return -1;

	for (i = 0, l = call_list; l; l = l->next, i++) {
		v = l->data;
		(*objlist)[i] = g_strdup(voicecall_build_path(modem, v->call));
	}

	return 0;
}

static gboolean voicecalls_have_active(struct voicecalls_data *calls)
{
	GSList *l;
	struct voicecall *v;

	for (l = calls->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_ACTIVE ||
			v->call->status == CALL_STATUS_INCOMING ||
			v->call->status == CALL_STATUS_DIALING ||
			v->call->status == CALL_STATUS_ALERTING)
			return TRUE;
	}

	return FALSE;
}

static gboolean voicecalls_have_connected(struct voicecalls_data *calls)
{
	GSList *l;
	struct voicecall *v;

	for (l = calls->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_ACTIVE)
			return TRUE;
	}

	return FALSE;
}

static gboolean voicecalls_have_held(struct voicecalls_data *calls)
{
	GSList *l;
	struct voicecall *v;

	for (l = calls->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_HELD)
			return TRUE;
	}

	return FALSE;
}

static int voicecalls_num_with_status(struct voicecalls_data *calls,
					int status)
{
	GSList *l;
	struct voicecall *v;
	int num = 0;

	for (l = calls->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == status)
			num += 1;
	}

	return num;
}

static int voicecalls_num_active(struct voicecalls_data *calls)
{
	return voicecalls_num_with_status(calls, CALL_STATUS_ACTIVE);
}

static int voicecalls_num_held(struct voicecalls_data *calls)
{
	return voicecalls_num_with_status(calls, CALL_STATUS_HELD);
}

static int voicecalls_num_connecting(struct voicecalls_data *calls)
{
	int r = 0;

	r += voicecalls_num_with_status(calls, CALL_STATUS_DIALING);
	r += voicecalls_num_with_status(calls, CALL_STATUS_ALERTING);

	return r;
}

static GSList *voicecalls_held_list(struct voicecalls_data *calls)
{
	GSList *l;
	GSList *r = NULL;
	struct voicecall *v;

	for (l = calls->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_HELD)
			r = g_slist_prepend(r, v);
	}

	if (r)
		r = g_slist_reverse(r);

	return r;
}

/* Intended to be used for multiparty, which cannot be incoming,
 * alerting or dialing */
static GSList *voicecalls_active_list(struct voicecalls_data *calls)
{
	GSList *l;
	GSList *r = NULL;
	struct voicecall *v;

	for (l = calls->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_ACTIVE)
			r = g_slist_prepend(r, v);
	}

	if (r)
		r = g_slist_reverse(r);

	return r;
}

static gboolean voicecalls_have_waiting(struct voicecalls_data *calls)
{
	GSList *l;
	struct voicecall *v;

	for (l = calls->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_WAITING)
			return TRUE;
	}

	return FALSE;
}

static void voicecalls_release_queue(struct ofono_modem *modem, GSList *calls)
{
	struct voicecalls_data *voicecalls = modem->voicecalls;
	GSList *l;

	g_slist_free(voicecalls->release_list);
	voicecalls->release_list = NULL;

	for (l = calls; l; l = l->next) {
		voicecalls->release_list =
			g_slist_prepend(voicecalls->release_list, l->data);
	}
}

static void voicecalls_release_next(struct ofono_modem *modem)
{
	struct voicecalls_data *voicecalls = modem->voicecalls;
	struct voicecall *call;

	if (!voicecalls->release_list)
		return;

	call = voicecalls->release_list->data;

	voicecalls->release_list = g_slist_remove(voicecalls->release_list,
							call);

	voicecalls->ops->release_specific(modem, call->call->id,
						multirelease_callback, modem);
}

static DBusMessage *manager_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	char **callobj_list;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	voicecalls_path_list(modem, calls->call_list, &callobj_list);

	ofono_dbus_dict_append_array(&dict, "Calls", DBUS_TYPE_OBJECT_PATH,
				&callobj_list);

	g_strfreev(callobj_list);

	voicecalls_path_list(modem, calls->multiparty_list, &callobj_list);

	ofono_dbus_dict_append_array(&dict, "MultipartyCalls",
					DBUS_TYPE_OBJECT_PATH, &callobj_list);

	g_strfreev(callobj_list);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *manager_dial(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	const char *number;
	struct ofono_phone_number ph;
	const char *clirstr;
	enum ofono_clir_option clir;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (g_slist_length(calls->call_list) >= MAX_VOICE_CALLS)
		return __ofono_error_failed(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &number,
					DBUS_TYPE_STRING, &clirstr,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!valid_phone_number_format(number))
		return __ofono_error_invalid_format(msg);

	if (strlen(clirstr) == 0 || !strcmp(clirstr, "default"))
		clir = OFONO_CLIR_OPTION_DEFAULT;
	else if (!strcmp(clirstr, "disabled"))
		clir = OFONO_CLIR_OPTION_SUPPRESSION;
	else if (!strcmp(clirstr, "enabled"))
		clir = OFONO_CLIR_OPTION_INVOCATION;
	else
		return __ofono_error_invalid_format(msg);

	if (!calls->ops->dial)
		return __ofono_error_not_implemented(msg);

	if (voicecalls_have_active(calls) &&
		voicecalls_have_held(calls))
		return __ofono_error_failed(msg);

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	string_to_phone_number(number, &ph);

	calls->ops->dial(modem, &ph, clir, OFONO_CUG_OPTION_DEFAULT,
				dial_callback, modem);

	return NULL;
}

static DBusMessage *manager_transfer(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	int numactive;
	int numheld;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	numactive = voicecalls_num_active(calls);

	/* According to 22.091 section 5.8, the network has the option of
	 * implementing the call transfer operation for a call that is
	 * still dialing/alerting.
	 */
	numactive += voicecalls_num_connecting(calls);

	numheld = voicecalls_num_held(calls);

	if ((numactive != 1) && (numheld != 1))
		return __ofono_error_failed(msg);

	if (!calls->ops->transfer)
		return __ofono_error_not_implemented(msg);

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	calls->ops->transfer(modem, generic_callback, calls);

	return NULL;
}

static DBusMessage *manager_swap_calls(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (voicecalls_have_waiting(calls))
		return __ofono_error_failed(msg);

	if (!calls->ops->hold_all_active)
		return __ofono_error_not_implemented(msg);

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	calls->ops->hold_all_active(modem, generic_callback, calls);

	return NULL;
}

static DBusMessage *manager_release_and_answer(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (!voicecalls_have_active(calls) || !voicecalls_have_waiting(calls))
		return __ofono_error_failed(msg);

	if (!calls->ops->release_all_active)
		return __ofono_error_not_implemented(msg);

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	calls->ops->release_all_active(modem, generic_callback, calls);

	return NULL;
}

static DBusMessage *manager_hold_and_answer(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (voicecalls_have_active(calls) && voicecalls_have_held(calls) &&
		voicecalls_have_waiting(calls))
		return __ofono_error_failed(msg);

	if (!calls->ops->hold_all_active)
		return __ofono_error_not_implemented(msg);

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	calls->ops->hold_all_active(modem, generic_callback, calls);

	return NULL;
}

static DBusMessage *manager_hangup_all(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (!calls->ops->release_specific)
		return __ofono_error_not_implemented(msg);

	if (calls->call_list == NULL) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		return reply;
	}

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->flags |= VOICECALLS_FLAG_MULTI_RELEASE;

	calls->pending = dbus_message_ref(msg);

	voicecalls_release_queue(modem, calls->call_list);
	voicecalls_release_next(modem);

	return NULL;
}

static DBusMessage *multiparty_private_chat(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	const char *callpath;
	const char *c;
	unsigned int id;
	GSList *l;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &callpath,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (strlen(callpath) == 0)
		return __ofono_error_invalid_format(msg);

	c = strrchr(callpath, '/');

	if (!c || strncmp(modem->path, callpath, c-callpath))
		return __ofono_error_not_found(msg);

	if (!sscanf(c, "/voicecall%2u", &id))
		return __ofono_error_not_found(msg);

	for (l = calls->multiparty_list; l; l = l->next) {
		struct voicecall *v = l->data;
		if (v->call->id == id)
			break;
	}

	if (!l)
		return __ofono_error_not_found(msg);

	/* If we found id on the list of multiparty calls, then by definition
	 * the multiparty call exists.	Only thing to check is whether we have
	 * held calls
	 */
	if (voicecalls_have_held(calls))
		return __ofono_error_failed(msg);

	if (!calls->ops->private_chat)
		return __ofono_error_not_implemented(msg);

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	calls->ops->private_chat(modem, id, private_chat_callback, modem);

	return NULL;
}

static DBusMessage *multiparty_create(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (!voicecalls_have_held(calls) || !voicecalls_have_active(calls))
		return __ofono_error_failed(msg);

	if (!calls->ops->create_multiparty)
		return __ofono_error_not_implemented(msg);

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	calls->ops->create_multiparty(modem, multiparty_create_callback, modem);

	return NULL;
}

static DBusMessage *multiparty_hangup(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (!calls->ops->release_specific)
		return __ofono_error_not_implemented(msg);

	if (!calls->ops->release_all_held)
		return __ofono_error_not_implemented(msg);

	if (!calls->ops->release_all_active)
		return __ofono_error_not_implemented(msg);

	if (calls->multiparty_list == NULL) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		return reply;
	}

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	/* We don't have waiting calls, as we can't use +CHLD to release */
	if (!voicecalls_have_waiting(calls)) {
		struct voicecall *v = calls->multiparty_list->data;

		if (v->call->status == CALL_STATUS_HELD) {
			calls->ops->release_all_held(modem, generic_callback,
							calls);
			goto out;
		}

		/* Multiparty is currently active, if we have held calls
		 * we shouldn't use release_all_active here since this also
		 * has the side-effect of activating held calls
		 */
		if (!voicecalls_have_held(calls)) {
			calls->ops->release_all_active(modem, generic_callback,
						calls);
			goto out;
		}
	}

	/* Fall back to the old-fashioned way */
	calls->flags |= VOICECALLS_FLAG_MULTI_RELEASE;
	voicecalls_release_queue(modem, calls->multiparty_list);
	voicecalls_release_next(modem);

out:
	return NULL;
}

static DBusMessage *manager_tone(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	const char *in_tones;
	char *tones;
	int i, len;

	if (calls->flags & VOICECALLS_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (!calls->ops->send_tones)
		return __ofono_error_not_implemented(msg);

	/* Send DTMFs only if we have at least one connected call */
	if (!voicecalls_have_connected(calls))
		return __ofono_error_failed(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &in_tones,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	len = strlen(in_tones);

	if (len == 0)
		return __ofono_error_invalid_format(msg);

	tones = g_ascii_strup(in_tones, len);

	/* Tones can be 0-9, *, #, A-D according to 27.007 C.2.11 */
	for (i = 0; i < len; i++) {
		if (g_ascii_isdigit(tones[i]) ||
			tones[i] == '*' || tones[i] == '#' ||
				(tones[i] >= 'A' && tones[i] <= 'D'))
			continue;

		g_free(tones);
		return __ofono_error_invalid_format(msg);
	}

	calls->flags |= VOICECALLS_FLAG_PENDING;
	calls->pending = dbus_message_ref(msg);

	calls->ops->send_tones(modem, tones, generic_callback, calls);

	g_free(tones);

	return NULL;
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	manager_get_properties },
	{ "Dial",		"ss",	"o",		manager_dial,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "Transfer",		"",	"",		manager_transfer,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SwapCalls",		"",	"",		manager_swap_calls,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "ReleaseAndAnswer",	"",	"",		manager_release_and_answer,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "HoldAndAnswer",	"",	"",		manager_hold_and_answer,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "HangupAll",		"",	"",		manager_hangup_all,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "PrivateChat",	"o",	"ao",		multiparty_private_chat,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "CreateMultiparty",	"",	"ao",		multiparty_create,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "HangupMultiparty",	"",	"",		multiparty_hangup,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SendTones",		"s",	"",		manager_tone,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static gboolean real_emit_call_list_changed(void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *voicecalls = modem->voicecalls;
	DBusConnection *conn = ofono_dbus_get_connection();
	char **objpath_list;

	voicecalls_path_list(modem, voicecalls->call_list, &objpath_list);

	ofono_dbus_signal_array_property_changed(conn, modem->path,
				VOICECALL_MANAGER_INTERFACE,
				"Calls",
				DBUS_TYPE_OBJECT_PATH,
				&objpath_list);

	g_strfreev(objpath_list);

	voicecalls->emit_calls_source = 0;

	return FALSE;
}

static void emit_call_list_changed(struct ofono_modem *modem)
{
#ifdef DELAY_EMIT
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->emit_calls_source == 0)
		calls->emit_calls_source = 
			g_timeout_add(0, real_emit_call_list_changed, modem);
#else
	real_emit_call_list_changed(modem);
#endif
}

static gboolean real_emit_multiparty_call_list_changed(void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *voicecalls = modem->voicecalls;
	DBusConnection *conn = ofono_dbus_get_connection();
	char **objpath_list;

	voicecalls_path_list(modem, voicecalls->multiparty_list, &objpath_list);

	ofono_dbus_signal_array_property_changed(conn, modem->path,
				VOICECALL_MANAGER_INTERFACE, "MultipartyCalls",
				DBUS_TYPE_OBJECT_PATH,
				&objpath_list);

	g_strfreev(objpath_list);
	
	voicecalls->emit_multi_source = 0;

	return FALSE;
}

static void emit_multiparty_call_list_changed(struct ofono_modem *modem)
{
#ifdef DELAY_EMIT
	struct voicecalls_data *calls = modem->voicecalls;

	if (calls->emit_multi_source == 0)
		calls->emit_multi_source = g_timeout_add(0, 
				real_emit_multiparty_call_list_changed, modem);
	}
#else
	real_emit_multiparty_call_list_changed(modem);
#endif
}

void ofono_voicecall_disconnected(struct ofono_modem *modem, int id,
				enum ofono_disconnect_reason reason,
				const struct ofono_error *error)
{
	GSList *l;
	struct voicecalls_data *calls = modem->voicecalls;
	struct voicecall *call;
	time_t ts;
	enum call_status prev_status;

	ofono_debug("Got disconnection event for id: %d, reason: %d", id, reason);

	l = g_slist_find_custom(calls->call_list, GINT_TO_POINTER(id),
				call_compare_by_id);

	if (!l) {
		ofono_error("Plugin notified us of call disconnect for"
				" unknown call");
		return;
	}

	call = l->data;

	ts = time(NULL);
	prev_status = call->call->status;

	l = g_slist_find_custom(calls->multiparty_list, GINT_TO_POINTER(id),
				call_compare_by_id);

	if (l) {
		calls->multiparty_list =
			g_slist_remove(calls->multiparty_list, call);

		if (calls->multiparty_list->next == NULL) { /* Size == 1 */
			g_slist_free(calls->multiparty_list);
			calls->multiparty_list = 0;
		}

		emit_multiparty_call_list_changed(modem);
	}

	calls->release_list = g_slist_remove(calls->release_list, call);

	__ofono_modem_release_callid(modem, id);

	/* TODO: Emit disconnect reason */
	voicecall_set_call_status(modem, call, CALL_STATUS_DISCONNECTED);

	if (prev_status == CALL_STATUS_INCOMING)
		__ofono_history_call_missed(modem, call->call, ts);
	else
		__ofono_history_call_ended(modem, call->call,
						call->detect_time, ts);

	voicecall_dbus_unregister(modem, call);

	calls->call_list = g_slist_remove(calls->call_list, call);

	emit_call_list_changed(modem);
}

void ofono_voicecall_notify(struct ofono_modem *modem, const struct ofono_call *call)
{
	GSList *l;
	struct voicecalls_data *calls = modem->voicecalls;
	struct voicecall *v = NULL;
	struct ofono_call *newcall = NULL;

	ofono_debug("Got a voicecall event, status: %d, id: %u, number: %s",
			call->status, call->id, call->phone_number.number);

	l = g_slist_find_custom(calls->call_list, GINT_TO_POINTER(call->id),
				call_compare_by_id);

	if (l) {
		ofono_debug("Found call with id: %d\n", call->id);
		voicecall_set_call_status(modem, l->data, call->status);
		voicecall_set_call_lineid(modem, l->data, &call->phone_number,
						call->clip_validity);

		return;
	}

	ofono_debug("Did not find a call with id: %d\n", call->id);

	newcall = g_try_new0(struct ofono_call, 1);

	if (!call) {
		ofono_error("Unable to allocate call");
		goto err;
	}

	memcpy(newcall, call, sizeof(struct ofono_call));

	if (__ofono_modem_alloc_callid(modem) != call->id) {
		ofono_error("Warning: Call id and internally tracked id"
				" do not correspond");
		goto err;
	}

	v = voicecall_create(modem, newcall);

	if (!v) {
		ofono_error("Unable to allocate voicecall_data");
		goto err;
	}

	v->detect_time = time(NULL);

	if (!voicecall_dbus_register(v)) {
		ofono_error("Unable to register voice call");
		goto err;
	}

	calls->call_list = g_slist_insert_sorted(calls->call_list, v,
							call_compare);

	emit_call_list_changed(modem);

	return;

err:
	if (newcall)
		g_free(newcall);

	if (v)
		g_free(v);
}

static void generic_callback(const struct ofono_error *error, void *data)
{
	struct voicecalls_data *calls = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("command failed with error: %s",
				telephony_error_to_str(error));

	calls->flags &= ~VOICECALLS_FLAG_PENDING;

	if (!calls->pending)
		return;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(calls->pending);
	else
		reply = __ofono_error_failed(calls->pending);

	g_dbus_send_message(conn, reply);

	dbus_message_unref(calls->pending);
	calls->pending = NULL;
}

static void multirelease_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;

	if (calls->release_list != NULL) {
		voicecalls_release_next(modem);
		return;
	}

	calls->flags &= ~VOICECALLS_FLAG_MULTI_RELEASE;
	calls->flags &= ~VOICECALLS_FLAG_PENDING;

	if (!calls->pending)
		return;

	reply = dbus_message_new_method_return(calls->pending);

	g_dbus_send_message(conn, reply);

	dbus_message_unref(calls->pending);
	calls->pending = NULL;
}

static struct ofono_call *synthesize_outgoing_call(struct ofono_modem *modem,
						DBusMessage *msg)
{
	const char *number;
	struct ofono_call *call;

	call = g_try_new0(struct ofono_call, 1);

	if (!call)
		return call;

	call->id = __ofono_modem_alloc_callid(modem);

	if (call->id == 0) {
		ofono_error("Failed to alloc callid, too many calls");
		g_free(call);
		return NULL;
	}

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &number,
				DBUS_TYPE_INVALID) == FALSE)
		number = "";
	else
		string_to_phone_number(number, &call->phone_number);

	call->direction = CALL_DIRECTION_MOBILE_ORIGINATED;
	call->status = CALL_STATUS_DIALING;
	call->clip_validity = CLIP_VALIDITY_VALID;

	return call;
}

static void dial_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	GSList *l;
	struct ofono_call *call;
	const char *path;
	gboolean need_to_emit = FALSE;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("Dial callback returned error: %s",
			telephony_error_to_str(error));

	calls->flags &= ~VOICECALLS_FLAG_PENDING;

	if (!calls->pending)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		reply = __ofono_error_failed(calls->pending);
		g_dbus_send_message(conn, reply);

		goto out;
	}

	reply = dbus_message_new_method_return(calls->pending);
	if (!reply)
		goto out;

	/* Two things can happen, the call notification arrived before dial
	 * callback or dial callback was first.	Handle here */
	for (l = calls->call_list; l; l = l->next) {
		struct voicecall *v = l->data;

		if (v->call->status == CALL_STATUS_DIALING ||
			v->call->status == CALL_STATUS_ALERTING)
			break;
	}

	if (!l) {
		struct voicecall *v;
		call = synthesize_outgoing_call(modem, calls->pending);

		if (!call) {
			reply = __ofono_error_failed(calls->pending);
			g_dbus_send_message(conn, reply);

			goto out;
		}

		v = voicecall_create(modem, call);

		if (!v) {
			reply = __ofono_error_failed(calls->pending);
			g_dbus_send_message(conn, reply);

			goto out;
		}

		v->detect_time = time(NULL);

		ofono_debug("Registering new call: %d", call->id);
		voicecall_dbus_register(v);

		calls->call_list = g_slist_insert_sorted(calls->call_list, v,
					call_compare);

		need_to_emit = TRUE;
	} else {
		struct voicecall *v = l->data;

		call = v->call;
	}

	path = voicecall_build_path(modem, call);

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);

	g_dbus_send_message(conn, reply);

	if (need_to_emit)
		emit_call_list_changed(modem);

out:
	dbus_message_unref(calls->pending);
	calls->pending = NULL;
}


static void multiparty_callback_common(struct ofono_modem *modem,
					DBusMessage *reply)
{
	struct voicecalls_data *voicecalls = modem->voicecalls;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	char **objpath_list;
	int i;

	voicecalls_path_list(modem, voicecalls->multiparty_list, &objpath_list);

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
		DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (i = 0; objpath_list[i]; i++)
		dbus_message_iter_append_basic(&array_iter,
			DBUS_TYPE_OBJECT_PATH, &objpath_list[i]);

	dbus_message_iter_close_container(&iter, &array_iter);
}

static void multiparty_create_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	gboolean need_to_emit = FALSE;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("command failed with error: %s",
				telephony_error_to_str(error));

	calls->flags &= ~VOICECALLS_FLAG_PENDING;

	if (!calls->pending)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		reply = __ofono_error_failed(calls->pending);
		goto out;
	}

	/* We just created a multiparty call, gather all held
	 * active calls and add them to the multiparty list
	 */
	if (calls->multiparty_list) {
		g_slist_free(calls->multiparty_list);
		calls->multiparty_list = 0;
	}

	calls->multiparty_list = g_slist_concat(calls->multiparty_list,
						voicecalls_held_list(calls));

	calls->multiparty_list = g_slist_concat(calls->multiparty_list,
						voicecalls_active_list(calls));

	calls->multiparty_list = g_slist_sort(calls->multiparty_list,
						call_compare);

	if (g_slist_length(calls->multiparty_list) < 2) {
		ofono_error("Created multiparty call, but size is less than 2"
				" panic!");

		reply = __ofono_error_failed(calls->pending);
	} else {
		reply = dbus_message_new_method_return(calls->pending);

		multiparty_callback_common(modem, reply);
		need_to_emit = TRUE;
	}

out:
	g_dbus_send_message(conn, reply);

	if (need_to_emit)
		emit_multiparty_call_list_changed(modem);

	dbus_message_unref(calls->pending);
	calls->pending = NULL;
}

static void private_chat_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct voicecalls_data *calls = modem->voicecalls;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	gboolean need_to_emit = FALSE;
	const char *callpath;
	const char *c;
	int id;
	GSList *l;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("command failed with error: %s",
				telephony_error_to_str(error));

	calls->flags &= ~VOICECALLS_FLAG_PENDING;

	if (!calls->pending)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		reply = __ofono_error_failed(calls->pending);
		goto out;
	}

	dbus_message_get_args(calls->pending, NULL,
				DBUS_TYPE_OBJECT_PATH, &callpath,
				DBUS_TYPE_INVALID);

	c = strrchr(callpath, '/');
	sscanf(c, "/voicecall%2u", &id);

	l = g_slist_find_custom(calls->multiparty_list, GINT_TO_POINTER(id),
				call_compare_by_id);

	if (l) {
		calls->multiparty_list =
			g_slist_remove(calls->multiparty_list, l->data);

		if (g_slist_length(calls->multiparty_list) < 2) {
			g_slist_free(calls->multiparty_list);
			calls->multiparty_list = 0;
		}
	}

	reply = dbus_message_new_method_return(calls->pending);

	multiparty_callback_common(modem, reply);
	need_to_emit = TRUE;

out:
	g_dbus_send_message(conn, reply);

	if (need_to_emit)
		emit_multiparty_call_list_changed(modem);

	dbus_message_unref(calls->pending);
	calls->pending = NULL;
}

int ofono_voicecall_register(struct ofono_modem *modem, struct ofono_voicecall_ops *ops)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->voicecalls = voicecalls_create();

	if (modem->voicecalls == NULL)
		return -1;

	modem->voicecalls->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					VOICECALL_MANAGER_INTERFACE,
					manager_methods, manager_signals, NULL,
					modem, voicecalls_destroy)) {
		ofono_error("Could not create %s interface",
				VOICECALL_MANAGER_INTERFACE);

		voicecalls_destroy(modem->voicecalls);

		return -1;
	}

	ofono_modem_add_interface(modem, VOICECALL_MANAGER_INTERFACE);

	return 0;
}

void ofono_voicecall_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (!modem->voicecalls)
		return;

	ofono_modem_remove_interface(modem, VOICECALL_MANAGER_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path,
					VOICECALL_MANAGER_INTERFACE);

	modem->voicecalls = NULL;
}
