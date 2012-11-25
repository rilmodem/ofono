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
#include <stdint.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"
#include "simutil.h"
#include "smsutil.h"
#include "storage.h"

#define MAX_VOICE_CALLS 16

#define VOICECALL_FLAG_SIM_ECC_READY 0x1
#define VOICECALL_FLAG_STK_MODEM_CALLSETUP 0x2

#define SETTINGS_STORE "voicecall"
#define SETTINGS_GROUP "Settings"

GSList *g_drivers = NULL;

struct ofono_voicecall {
	GSList *call_list;
	GSList *release_list;
	GSList *multiparty_list;
	GHashTable *en_list; /* emergency number list */
	GSList *sim_en_list; /* Emergency numbers already read from SIM */
	GSList *new_sim_en_list; /* Emergency numbers being read from SIM */
	char **nw_en_list; /* Emergency numbers from modem/network */
	DBusMessage *pending;
	uint32_t flags;
	struct ofono_sim *sim;
	struct ofono_sim_context *sim_context;
	unsigned int sim_watch;
	unsigned int sim_state_watch;
	const struct ofono_voicecall_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	struct dial_request *dial_req;
	GQueue *toneq;
	guint tone_source;
	unsigned int hfp_watch;
	GKeyFile *settings;
	char *imsi;
	ofono_voicecall_cb_t release_queue_done_cb;
	struct ofono_emulator *pending_em;
	unsigned int pending_id;
};

struct voicecall {
	struct ofono_call *call;
	struct ofono_voicecall *vc;
	time_t start_time;
	time_t detect_time;
	char *message;
	uint8_t icon_id;
	gboolean untracked;
	gboolean dial_result_handled;
	ofono_bool_t remote_held;
	ofono_bool_t remote_multiparty;
};

struct dial_request {
	struct ofono_voicecall *vc;
	char *message;
	uint8_t icon_id;
	enum ofono_voicecall_interaction interaction;
	ofono_voicecall_dial_cb_t cb;
	void *user_data;
	struct voicecall *call;
	struct ofono_phone_number ph;
};

struct tone_queue_entry {
	char *tone_str;
	char *left;
	ofono_voicecall_tone_cb_t cb;
	void *user_data;
	ofono_destroy_func destroy;
	int id;
};

struct emulator_status {
	struct ofono_voicecall *vc;
	int status;
};

static const char *default_en_list[] = { "911", "112", NULL };
static const char *default_en_list_no_sim[] = { "119", "118", "999", "110",
						"08", "000", NULL };

static void send_ciev_after_swap_callback(const struct ofono_error *error,
								void *data);
static void generic_callback(const struct ofono_error *error, void *data);
static void hangup_all_active(const struct ofono_error *error, void *data);
static void multirelease_callback(const struct ofono_error *err, void *data);
static gboolean tone_request_run(gpointer user_data);

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

static void add_to_en_list(struct ofono_voicecall *vc, char **list)
{
	int i = 0;

	while (list[i])
		g_hash_table_insert(vc->en_list, g_strdup(list[i++]), NULL);
}

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

static const char *cnap_to_string(const char *name, int cnap_validity)
{
	if (cnap_validity == CNAP_VALIDITY_WITHHELD && !strlen(name))
		return "withheld";

	if (cnap_validity == CNAP_VALIDITY_NOT_AVAILABLE)
		return "";

	return name;
}

static const char *time_to_str(const time_t *t)
{
	static char buf[128];
	struct tm tm;

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", localtime_r(t, &tm));
	buf[127] = '\0';

	return buf;
}

static unsigned int voicecalls_num_with_status(struct ofono_voicecall *vc,
						int status)
{
	GSList *l;
	struct voicecall *v;
	int num = 0;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == status)
			num += 1;
	}

	return num;
}

static unsigned int voicecalls_num_active(struct ofono_voicecall *vc)
{
	return voicecalls_num_with_status(vc, CALL_STATUS_ACTIVE);
}

static unsigned int voicecalls_num_held(struct ofono_voicecall *vc)
{
	return voicecalls_num_with_status(vc, CALL_STATUS_HELD);
}

static unsigned int voicecalls_num_connecting(struct ofono_voicecall *vc)
{
	unsigned int r = 0;

	r += voicecalls_num_with_status(vc, CALL_STATUS_DIALING);
	r += voicecalls_num_with_status(vc, CALL_STATUS_ALERTING);

	return r;
}

static gboolean voicecalls_have_active(struct ofono_voicecall *vc)
{
	GSList *l;
	struct voicecall *v;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_ACTIVE ||
				v->call->status == CALL_STATUS_DIALING ||
				v->call->status == CALL_STATUS_ALERTING)
			return TRUE;
	}

	return FALSE;
}

static gboolean voicecalls_have_with_status(struct ofono_voicecall *vc,
						int status)
{
	GSList *l;
	struct voicecall *v;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == status)
			return TRUE;
	}

	return FALSE;
}

static gboolean voicecalls_have_held(struct ofono_voicecall *vc)
{
	return voicecalls_have_with_status(vc, CALL_STATUS_HELD);
}

static gboolean voicecalls_have_waiting(struct ofono_voicecall *vc)
{
	return voicecalls_have_with_status(vc, CALL_STATUS_WAITING);
}

static gboolean voicecalls_have_incoming(struct ofono_voicecall *vc)
{
	return voicecalls_have_with_status(vc, CALL_STATUS_INCOMING);
}

static void dial_request_finish(struct ofono_voicecall *vc)
{
	struct dial_request *dial_req = vc->dial_req;

	if (dial_req->cb)
		dial_req->cb(dial_req->call ? dial_req->call->call : NULL,
				dial_req->user_data);

	g_free(dial_req->message);
	g_free(dial_req);
	vc->dial_req = NULL;
}

static gboolean voicecalls_can_dtmf(struct ofono_voicecall *vc)
{
	GSList *l;
	struct voicecall *v;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_ACTIVE)
			return TRUE;

		/* Connected for 2nd stage dialing */
		if (v->call->status == CALL_STATUS_ALERTING)
			return TRUE;
	}

	return FALSE;
}

static int tone_queue(struct ofono_voicecall *vc, const char *tone_str,
			ofono_voicecall_tone_cb_t cb, void *data,
			ofono_destroy_func destroy)
{
	struct tone_queue_entry *entry;
	int id = 1;
	int n = 0;
	int i;

	/*
	 * Tones can be 0-9, *, #, A-D according to 27.007 C.2.11,
	 * and p for Pause.
	 */
	for (i = 0; tone_str[i]; i++)
		if (!g_ascii_isdigit(tone_str[i]) && tone_str[i] != 'p' &&
				tone_str[i] != 'P' && tone_str[i] != '*' &&
				tone_str[i] != '#' && (tone_str[i] < 'A' ||
				tone_str[i] > 'D'))
			return -EINVAL;

	while ((entry = g_queue_peek_nth(vc->toneq, n++)) != NULL)
		if (entry->id >= id)
			id = entry->id + 1;

	entry = g_try_new0(struct tone_queue_entry, 1);
	if (entry == NULL)
		return -ENOMEM;

	entry->tone_str = g_strdup(tone_str);
	entry->left = entry->tone_str;
	entry->cb = cb;
	entry->user_data = data;
	entry->destroy = destroy;
	entry->id = id;

	g_queue_push_tail(vc->toneq, entry);

	if (g_queue_get_length(vc->toneq) == 1)
		g_timeout_add(0, tone_request_run, vc);

	return id;
}

static void tone_request_finish(struct ofono_voicecall *vc,
				struct tone_queue_entry *entry,
				int error, gboolean callback)
{
	g_queue_remove(vc->toneq, entry);

	if (callback)
		entry->cb(error, entry->user_data);

	if (entry->destroy)
		entry->destroy(entry->user_data);

	g_free(entry->tone_str);
	g_free(entry);
}

static gboolean is_emergency_number(struct ofono_voicecall *vc,
					const char *number)
{
	return g_hash_table_lookup_extended(vc->en_list, number, NULL, NULL);
}

static void append_voicecall_properties(struct voicecall *v,
					DBusMessageIter *dict)
{
	struct ofono_call *call = v->call;
	const char *status;
	const char *callerid;
	const char *timestr;
	const char *name;
	ofono_bool_t mpty;
	dbus_bool_t emergency_call;

	status = call_status_to_string(call->status);

	ofono_dbus_dict_append(dict, "State", DBUS_TYPE_STRING, &status);

	if (call->direction == CALL_DIRECTION_MOBILE_TERMINATED)
		callerid = phone_and_clip_to_string(&call->phone_number,
							call->clip_validity);
	else
		callerid = phone_number_to_string(&call->phone_number);

	ofono_dbus_dict_append(dict, "LineIdentification",
					DBUS_TYPE_STRING, &callerid);

	if (call->called_number.number[0] != '\0') {
		const char *calledid;

		calledid = phone_number_to_string(&call->called_number);

		ofono_dbus_dict_append(dict, "IncomingLine",
						DBUS_TYPE_STRING, &calledid);
	}

	name = cnap_to_string(call->name, call->cnap_validity);

	ofono_dbus_dict_append(dict, "Name", DBUS_TYPE_STRING, &name);

	if (call->status == CALL_STATUS_ACTIVE ||
			call->status == CALL_STATUS_HELD ||
			(call->status == CALL_STATUS_DISCONNECTED &&
				v->start_time != 0)) {
		timestr = time_to_str(&v->start_time);

		ofono_dbus_dict_append(dict, "StartTime", DBUS_TYPE_STRING,
					&timestr);
	}

	if (g_slist_find_custom(v->vc->multiparty_list,
				GINT_TO_POINTER(call->id), call_compare_by_id))
		mpty = TRUE;
	else
		mpty = FALSE;

	ofono_dbus_dict_append(dict, "Multiparty", DBUS_TYPE_BOOLEAN, &mpty);

	ofono_dbus_dict_append(dict, "RemoteHeld", DBUS_TYPE_BOOLEAN,
				&v->remote_held);

	ofono_dbus_dict_append(dict, "RemoteMultiparty", DBUS_TYPE_BOOLEAN,
				&v->remote_multiparty);

	if (v->message)
		ofono_dbus_dict_append(dict, "Information",
						DBUS_TYPE_STRING, &v->message);

	if (v->icon_id)
		ofono_dbus_dict_append(dict, "Icon",
						DBUS_TYPE_BYTE, &v->icon_id);

	if (is_emergency_number(v->vc, callerid) == TRUE)
		emergency_call = TRUE;
	else
		emergency_call = FALSE;

	ofono_dbus_dict_append(dict, "Emergency",
					DBUS_TYPE_BOOLEAN, &emergency_call);

}

static DBusMessage *voicecall_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
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
	append_voicecall_properties(v, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *voicecall_deflect(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_voicecall *vc = v->vc;
	struct ofono_call *call = v->call;

	struct ofono_phone_number ph;
	const char *number;

	if (call->status != CALL_STATUS_INCOMING &&
			call->status != CALL_STATUS_WAITING)
		return __ofono_error_failed(msg);

	if (vc->driver->deflect == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &number,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!valid_phone_number_format(number))
		return __ofono_error_invalid_format(msg);

	vc->pending = dbus_message_ref(msg);

	string_to_phone_number(number, &ph);

	vc->driver->deflect(vc, &ph, generic_callback, vc);

	return NULL;
}

static DBusMessage *voicecall_hangup(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_voicecall *vc = v->vc;
	struct ofono_call *call = v->call;
	gboolean single_call = vc->call_list->next == 0;

	if (vc->pending || vc->pending_em)
		return __ofono_error_busy(msg);

	if (vc->dial_req && vc->dial_req->call != v)
		return __ofono_error_busy(msg);

	switch (call->status) {
	case CALL_STATUS_DISCONNECTED:
		return __ofono_error_failed(msg);

	case CALL_STATUS_INCOMING:
		if (vc->driver->hangup_all == NULL &&
				vc->driver->hangup_active == NULL)
			return __ofono_error_not_implemented(msg);

		vc->pending = dbus_message_ref(msg);

		if (vc->driver->hangup_all)
			vc->driver->hangup_all(vc, generic_callback, vc);
		else
			vc->driver->hangup_active(vc, generic_callback, vc);

		return NULL;

	case CALL_STATUS_WAITING:
		if (vc->driver->set_udub == NULL)
			return __ofono_error_not_implemented(msg);

		vc->pending = dbus_message_ref(msg);
		vc->driver->set_udub(vc, generic_callback, vc);

		return NULL;

	case CALL_STATUS_HELD:
		if (vc->driver->release_all_held &&
				voicecalls_num_held(vc) == 1 &&
				voicecalls_have_waiting(vc) == FALSE) {
			vc->pending = dbus_message_ref(msg);
			vc->driver->release_all_held(vc, generic_callback, vc);

			return NULL;
		}

		break;

	case CALL_STATUS_DIALING:
	case CALL_STATUS_ALERTING:
		if (vc->driver->hangup_active != NULL) {
			vc->pending = dbus_message_ref(msg);
			vc->driver->hangup_active(vc, generic_callback, vc);

			return NULL;
		}

		/*
		 * Fall through, we check if we have a single alerting,
		 * dialing or active call and try to hang it up with
		 * hangup_all or hangup_active
		 */
	case CALL_STATUS_ACTIVE:
		if (single_call == TRUE && vc->driver->hangup_all != NULL) {
			vc->pending = dbus_message_ref(msg);
			vc->driver->hangup_all(vc, generic_callback, vc);

			return NULL;
		}

		if (voicecalls_num_active(vc) == 1 &&
				vc->driver->hangup_active != NULL) {
			vc->pending = dbus_message_ref(msg);
			vc->driver->hangup_active(vc, generic_callback, vc);

			return NULL;
		}

		break;
	}

	if (vc->driver->release_specific == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);
	vc->driver->release_specific(vc, call->id,
					generic_callback, vc);

	return NULL;
}

static DBusMessage *voicecall_answer(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct voicecall *v = data;
	struct ofono_voicecall *vc = v->vc;
	struct ofono_call *call = v->call;

	if (call->status != CALL_STATUS_INCOMING)
		return __ofono_error_failed(msg);

	if (vc->driver->answer == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->answer(vc, generic_callback, vc);

	return NULL;
}

static const GDBusMethodTable voicecall_methods[] = {
	{ GDBUS_METHOD("GetProperties",
				NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
				voicecall_get_properties) },
	{ GDBUS_ASYNC_METHOD("Deflect", GDBUS_ARGS({ "number", "s" }), NULL,
							voicecall_deflect) },
	{ GDBUS_ASYNC_METHOD("Hangup", NULL, NULL, voicecall_hangup) },
	{ GDBUS_ASYNC_METHOD("Answer", NULL, NULL, voicecall_answer) },
	{ }
};

static const GDBusSignalTable voicecall_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ GDBUS_SIGNAL("DisconnectReason",
					GDBUS_ARGS({ "reason", "s" })) },
	{ }
};

static struct voicecall *voicecall_create(struct ofono_voicecall *vc,
						struct ofono_call *call)
{
	struct voicecall *v;

	v = g_try_new0(struct voicecall, 1);
	if (v == NULL)
		return NULL;

	v->call = call;
	v->vc = vc;

	return v;
}

static void voicecall_destroy(gpointer userdata)
{
	struct voicecall *voicecall = (struct voicecall *)userdata;

	g_free(voicecall->call);
	g_free(voicecall->message);

	g_free(voicecall);
}

static const char *voicecall_build_path(struct ofono_voicecall *vc,
					const struct ofono_call *call)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/voicecall%02d",
			__ofono_atom_get_path(vc->atom), call->id);

	return path;
}

static void voicecall_emit_disconnect_reason(struct voicecall *call,
					enum ofono_disconnect_reason reason)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *reason_str;

	reason_str = disconnect_reason_to_string(reason);
	path = voicecall_build_path(call->vc, call->call);

	g_dbus_emit_signal(conn, path, OFONO_VOICECALL_INTERFACE,
				"DisconnectReason",
				DBUS_TYPE_STRING, &reason_str,
				DBUS_TYPE_INVALID);
}

static void voicecall_emit_multiparty(struct voicecall *call, gboolean mpty)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = voicecall_build_path(call->vc, call->call);
	dbus_bool_t val = mpty;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"Multiparty", DBUS_TYPE_BOOLEAN,
						&val);
}

static void emulator_set_indicator_forced(struct ofono_voicecall *vc,
						const char *name, int value)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	struct ofono_emulator *em;

	em = __ofono_atom_find(OFONO_ATOM_TYPE_EMULATOR_HFP, modem);
	if (em)
		__ofono_emulator_set_indicator_forced(em, name, value);
}

static void emulator_call_status_cb(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	struct emulator_status *s = data;

	ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_CALL, s->status);
}

static void emulator_callsetup_status_cb(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	struct emulator_status *s = data;

	ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_CALLSETUP,
					s->status);
}

static void emulator_callheld_status_cb(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	struct emulator_status *s = data;

	ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_CALLHELD,
					s->status);
}

static void notify_emulator_call_status(struct ofono_voicecall *vc)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	gboolean call = FALSE;
	unsigned int non_mpty = 0;
	gboolean multiparty = FALSE;
	gboolean held = FALSE;
	unsigned int non_mpty_held = 0;
	gboolean multiparty_held = FALSE;
	gboolean incoming = FALSE;
	gboolean dialing = FALSE;
	gboolean alerting = FALSE;
	gboolean waiting = FALSE;
	GSList *l;
	struct voicecall *v;
	struct emulator_status data;

	data.vc = vc;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		switch (v->call->status) {
		case CALL_STATUS_ACTIVE:
			call = TRUE;
			if (g_slist_find_custom(vc->multiparty_list,
						GINT_TO_POINTER(v->call->id),
						call_compare_by_id))
				multiparty = TRUE;
			else
				non_mpty++;
			break;

		case CALL_STATUS_HELD:
			held = TRUE;
			if (g_slist_find_custom(vc->multiparty_list,
						GINT_TO_POINTER(v->call->id),
						call_compare_by_id))
				multiparty_held = TRUE;
			else
				non_mpty_held++;
			break;

		case CALL_STATUS_DIALING:
			dialing = TRUE;
			break;

		case CALL_STATUS_ALERTING:
			alerting = TRUE;
			break;

		case CALL_STATUS_INCOMING:
			incoming = TRUE;
			break;

		case CALL_STATUS_WAITING:
			waiting = TRUE;
			break;
		}
	}

	/*
	 * Perform some basic sanity checks for transitionary states;
	 * if a transitionary state is detected, then ignore it.  The call
	 * indicators will be updated properly in the follow-on calls to
	 * this function once the final state has been reached
	 */

	if (incoming && (held || call))
		return;

	if (waiting && (held == FALSE && call == FALSE))
		return;

	if (non_mpty > 1 || (non_mpty && multiparty))
		return;

	if (non_mpty_held > 1 || (non_mpty_held && multiparty_held))
		return;

	if (multiparty && multiparty_held)
		return;

	data.status = call || held ? OFONO_EMULATOR_CALL_ACTIVE :
					OFONO_EMULATOR_CALL_INACTIVE;

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_call_status_cb, &data);

	if (incoming)
		data.status = OFONO_EMULATOR_CALLSETUP_INCOMING;
	else if (dialing)
		data.status = OFONO_EMULATOR_CALLSETUP_OUTGOING;
	else if (alerting)
		data.status = OFONO_EMULATOR_CALLSETUP_ALERTING;
	else if (waiting)
		data.status = OFONO_EMULATOR_CALLSETUP_INCOMING;
	else
		data.status = OFONO_EMULATOR_CALLSETUP_INACTIVE;

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_callsetup_status_cb,
						&data);

	if (held)
		data.status = call ? OFONO_EMULATOR_CALLHELD_MULTIPLE :
					OFONO_EMULATOR_CALLHELD_ON_HOLD;
	else
		data.status = OFONO_EMULATOR_CALLHELD_NONE;

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_callheld_status_cb,
						&data);
}

static void voicecall_set_call_status(struct voicecall *call, int status)
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
	path = voicecall_build_path(call->vc, call->call);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"State", DBUS_TYPE_STRING,
						&status_str);

	notify_emulator_call_status(call->vc);

	if (status == CALL_STATUS_ACTIVE &&
		(old_status == CALL_STATUS_INCOMING ||
			old_status == CALL_STATUS_DIALING ||
			old_status == CALL_STATUS_ALERTING ||
			old_status == CALL_STATUS_WAITING)) {
		const char *timestr;

		call->start_time = time(NULL);
		timestr = time_to_str(&call->start_time);

		ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"StartTime", DBUS_TYPE_STRING,
						&timestr);

		if (call->vc->dial_req && call == call->vc->dial_req->call)
			dial_request_finish(call->vc);
	}

	if (status == CALL_STATUS_DISCONNECTED && call->vc->dial_req &&
			call == call->vc->dial_req->call)
		dial_request_finish(call->vc);

	if (!voicecalls_can_dtmf(call->vc)) {
		struct tone_queue_entry *entry;

		while ((entry = g_queue_peek_head(call->vc->toneq)))
			tone_request_finish(call->vc, entry, ENOENT, TRUE);
	}
}

static void voicecall_set_call_lineid(struct voicecall *v,
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

	/*
	 * Two cases: We get an incoming call with CLIP factored in, or
	 * CLIP comes in later as a separate event
	 * For COLP only the phone number should be checked, it can come
	 * in with the initial call event or later as a separate event
	 */

	/* For plugins that don't keep state, ignore */
	if (call->clip_validity == CLIP_VALIDITY_VALID &&
			clip_validity == CLIP_VALIDITY_NOT_AVAILABLE)
		return;

	strcpy(call->phone_number.number, ph->number);
	call->clip_validity = clip_validity;
	call->phone_number.type = ph->type;

	path = voicecall_build_path(v->vc, call);

	if (call->direction == CALL_DIRECTION_MOBILE_TERMINATED)
		lineid_str = phone_and_clip_to_string(ph, clip_validity);
	else
		lineid_str = phone_number_to_string(ph);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"LineIdentification",
						DBUS_TYPE_STRING, &lineid_str);

	if (is_emergency_number(v->vc, lineid_str)) {
		dbus_bool_t emergency_call = TRUE;

		ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"Emergency",
						DBUS_TYPE_BOOLEAN,
						&emergency_call);
	}
}

static void voicecall_set_call_calledid(struct voicecall *v,
					const struct ofono_phone_number *ph)
{
	struct ofono_call *call = v->call;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *calledid_str;

	if (!strcmp(call->called_number.number, ph->number) &&
					call->called_number.type == ph->type)
		return;

	strcpy(call->called_number.number, ph->number);
	call->called_number.type = ph->type;

	path = voicecall_build_path(v->vc, call);
	calledid_str = phone_number_to_string(ph);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"IncomingLine",
						DBUS_TYPE_STRING,
						&calledid_str);
}


static void voicecall_set_call_name(struct voicecall *v,
					const char *name,
					int cnap_validity)
{
	struct ofono_call *call = v->call;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *name_str;

	if (!strcmp(call->name, name) && call->cnap_validity == cnap_validity)
		return;

	/* For plugins that don't keep state, ignore */
	if (call->cnap_validity == CNAP_VALIDITY_VALID &&
			cnap_validity == CNAP_VALIDITY_NOT_AVAILABLE)
		return;

	strncpy(call->name, name, OFONO_MAX_CALLER_NAME_LENGTH);
	call->name[OFONO_MAX_CALLER_NAME_LENGTH] = '\0';
	call->cnap_validity = cnap_validity;

	path = voicecall_build_path(v->vc, call);

	name_str = cnap_to_string(name, cnap_validity);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"Name",
						DBUS_TYPE_STRING, &name_str);
}

static gboolean voicecall_dbus_register(struct voicecall *v)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;

	if (v == NULL)
		return FALSE;

	path = voicecall_build_path(v->vc, v->call);

	if (!g_dbus_register_interface(conn, path, OFONO_VOICECALL_INTERFACE,
					voicecall_methods,
					voicecall_signals,
					NULL, v, voicecall_destroy)) {
		ofono_error("Could not register VoiceCall %s", path);
		voicecall_destroy(v);

		return FALSE;
	}

	return TRUE;
}

static gboolean voicecall_dbus_unregister(struct ofono_voicecall *vc,
						struct voicecall *v)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = voicecall_build_path(vc, v->call);

	return g_dbus_unregister_interface(conn, path,
						OFONO_VOICECALL_INTERFACE);
}


static int voicecalls_path_list(struct ofono_voicecall *vc, GSList *call_list,
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
		(*objlist)[i] = g_strdup(voicecall_build_path(vc, v->call));
	}

	return 0;
}

static GSList *voicecalls_held_list(struct ofono_voicecall *vc)
{
	GSList *l;
	GSList *r = NULL;
	struct voicecall *v;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_HELD)
			r = g_slist_prepend(r, v);
	}

	if (r)
		r = g_slist_reverse(r);

	return r;
}

/*
 * Intended to be used for multiparty, which cannot be incoming,
 * alerting or dialing
 */
static GSList *voicecalls_active_list(struct ofono_voicecall *vc)
{
	GSList *l;
	GSList *r = NULL;
	struct voicecall *v;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_ACTIVE)
			r = g_slist_prepend(r, v);
	}

	if (r)
		r = g_slist_reverse(r);

	return r;
}

struct ofono_call *__ofono_voicecall_find_call_with_status(
				struct ofono_voicecall *vc, int status)
{
	GSList *l;
	struct voicecall *v;

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == status)
			return v->call;
	}

	return NULL;
}

static void voicecalls_multiparty_changed(GSList *old, GSList *new)
{
	GSList *o, *n;
	struct voicecall *nc, *oc;

	n = new;
	o = old;

	while (n || o) {
		nc = n ? n->data : NULL;
		oc = o ? o->data : NULL;

		if (oc && (nc == NULL || (nc->call->id > oc->call->id))) {
			voicecall_emit_multiparty(oc, FALSE);
			o = o->next;
		} else if (nc && (oc == NULL || (nc->call->id < oc->call->id))) {
			voicecall_emit_multiparty(nc, TRUE);
			n = n->next;
		} else {
			n = n->next;
			o = o->next;
		}
	}
}

static void voicecalls_emit_call_removed(struct ofono_voicecall *vc,
						struct voicecall *v)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *atompath = __ofono_atom_get_path(vc->atom);
	const char *path = voicecall_build_path(vc, v->call);

	g_dbus_emit_signal(conn, atompath, OFONO_VOICECALL_MANAGER_INTERFACE,
				"CallRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
}

static void voicecalls_emit_call_added(struct ofono_voicecall *vc,
					struct voicecall *v)
{
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *path;

	notify_emulator_call_status(vc);

	path = __ofono_atom_get_path(vc->atom);

	signal = dbus_message_new_signal(path,
					OFONO_VOICECALL_MANAGER_INTERFACE,
					"CallAdded");

	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);

	path = voicecall_build_path(vc, v->call);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_voicecall_properties(v, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(ofono_dbus_get_connection(), signal);
}

static void voicecalls_release_queue(struct ofono_voicecall *vc, GSList *calls,
					ofono_voicecall_cb_t cb,
					ofono_bool_t skip_held)
{
	GSList *l;
	struct voicecall *call;

	g_slist_free(vc->release_list);
	vc->release_list = NULL;

	for (l = calls; l; l = l->next) {
		call = l->data;

		if (call->call->status == CALL_STATUS_WAITING)
			continue;

		if (skip_held && call->call->status == CALL_STATUS_HELD)
			continue;

		vc->release_list = g_slist_prepend(vc->release_list, l->data);
	}

	vc->release_queue_done_cb = cb;
}

static void voicecalls_release_next(struct ofono_voicecall *vc)
{
	struct voicecall *call;

	if (vc->release_list == NULL)
		return;

	call = vc->release_list->data;

	vc->release_list = g_slist_remove(vc->release_list, call);

	if (vc->driver->hangup_active == NULL)
		goto fallback;

	if (call->call->status == CALL_STATUS_ACTIVE &&
					voicecalls_num_active(vc) == 1) {
		vc->driver->hangup_active(vc, multirelease_callback, vc);
		return;
	}

	if (call->call->status == CALL_STATUS_ALERTING ||
		call->call->status == CALL_STATUS_DIALING ||
			call->call->status == CALL_STATUS_INCOMING) {
		vc->driver->hangup_active(vc, multirelease_callback, vc);
		return;
	}

fallback:
	vc->driver->release_specific(vc, call->call->id,
					multirelease_callback, vc);
}

static void voicecalls_release_done(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(vc->pending);
	__ofono_dbus_pending_reply(&vc->pending, reply);
}

static DBusMessage *manager_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	int i;
	char **list;
	GHashTableIter ht_iter;
	gpointer key, value;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	/* property EmergencyNumbers */
	list = g_new0(char *, g_hash_table_size(vc->en_list) + 1);
	g_hash_table_iter_init(&ht_iter, vc->en_list);

	for (i = 0; g_hash_table_iter_next(&ht_iter, &key, &value); i++)
		list[i] = key;

	ofono_dbus_dict_append_array(&dict, "EmergencyNumbers",
					DBUS_TYPE_STRING, &list);
	g_free(list);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static ofono_bool_t clir_string_to_clir(const char *clirstr,
					enum ofono_clir_option *clir)
{
	if (strlen(clirstr) == 0 || !strcmp(clirstr, "default")) {
		*clir = OFONO_CLIR_OPTION_DEFAULT;
		return TRUE;
	} else if (!strcmp(clirstr, "disabled")) {
		*clir = OFONO_CLIR_OPTION_SUPPRESSION;
		return TRUE;
	} else if (!strcmp(clirstr, "enabled")) {
		*clir = OFONO_CLIR_OPTION_INVOCATION;
		return TRUE;
	} else {
		return FALSE;
	}
}

static struct ofono_call *synthesize_outgoing_call(struct ofono_voicecall *vc,
							const char *number)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	struct ofono_call *call;

	call = g_try_new0(struct ofono_call, 1);
	if (call == NULL)
		return call;

	call->id = __ofono_modem_callid_next(modem);

	if (call->id == 0) {
		ofono_error("Failed to alloc callid, too many calls");
		g_free(call);
		return NULL;
	}

	__ofono_modem_callid_hold(modem, call->id);

	if (number)
		string_to_phone_number(number, &call->phone_number);

	call->direction = CALL_DIRECTION_MOBILE_ORIGINATED;
	call->status = CALL_STATUS_DIALING;
	call->clip_validity = CLIP_VALIDITY_VALID;

	return call;
}

static struct voicecall *dial_handle_result(struct ofono_voicecall *vc,
						const struct ofono_error *error,
						const char *number,
						gboolean *need_to_emit)
{
	GSList *l;
	struct voicecall *v;
	struct ofono_call *call;

	*need_to_emit = FALSE;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Dial callback returned error: %s",
			telephony_error_to_str(error));

		return NULL;
	}

	/*
	 * Two things can happen, the call notification arrived before dial
	 * callback or dial callback was first.	Handle here
	 */
	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		if (v->call->status == CALL_STATUS_DIALING ||
				v->call->status == CALL_STATUS_ALERTING)
			goto handled;

		/*
		 * Dial request may return before existing active call
		 * is put on hold or after dialed call has got active
		 */
		if (v->call->status == CALL_STATUS_ACTIVE &&
				v->call->direction ==
				CALL_DIRECTION_MOBILE_ORIGINATED &&
				!v->dial_result_handled)
			goto handled;
	}

	call = synthesize_outgoing_call(vc, number);
	if (call == NULL)
		return NULL;

	v = voicecall_create(vc, call);
	if (v == NULL)
		return NULL;

	v->detect_time = time(NULL);

	DBG("Registering new call: %d", call->id);
	voicecall_dbus_register(v);

	vc->call_list = g_slist_insert_sorted(vc->call_list, v,
				call_compare);

	*need_to_emit = TRUE;

handled:
	v->dial_result_handled = TRUE;

	return v;
}

static void manager_dial_callback(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;
	const char *number;
	gboolean need_to_emit;
	struct voicecall *v;

	if (dbus_message_get_args(vc->pending, NULL, DBUS_TYPE_STRING, &number,
					DBUS_TYPE_INVALID) == FALSE)
		number = NULL;

	v = dial_handle_result(vc, error, number, &need_to_emit);

	if (v) {
		const char *path = voicecall_build_path(vc, v->call);

		reply = dbus_message_new_method_return(vc->pending);

		dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_INVALID);
	} else {
		struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);

		if (is_emergency_number(vc, number) == TRUE)
			__ofono_modem_dec_emergency_mode(modem);

		reply = __ofono_error_failed(vc->pending);
	}

	__ofono_dbus_pending_reply(&vc->pending, reply);

	if (need_to_emit)
		voicecalls_emit_call_added(vc, v);
}

static int voicecall_dial(struct ofono_voicecall *vc, const char *number,
					enum ofono_clir_option clir,
					ofono_voicecall_cb_t cb, void *data)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	struct ofono_phone_number ph;

	if (g_slist_length(vc->call_list) >= MAX_VOICE_CALLS)
		return -EPERM;

	if (!valid_long_phone_number_format(number))
		return -EINVAL;

	if (ofono_modem_get_online(modem) == FALSE)
		return -ENETDOWN;

	if (vc->driver->dial == NULL)
		return -ENOTSUP;

	if (voicecalls_have_incoming(vc))
		return -EBUSY;

	/* We can't have two dialing/alerting calls, reject outright */
	if (voicecalls_num_connecting(vc) > 0)
		return -EBUSY;

	if (voicecalls_have_active(vc) && voicecalls_have_held(vc))
		return -EBUSY;

	if (is_emergency_number(vc, number) == TRUE)
		__ofono_modem_inc_emergency_mode(modem);

	string_to_phone_number(number, &ph);

	if (vc->settings) {
		g_key_file_set_string(vc->settings, SETTINGS_GROUP,
					"Number", number);
		storage_sync(vc->imsi, SETTINGS_STORE, vc->settings);
	}

	vc->driver->dial(vc, &ph, clir, cb, vc);

	return 0;
}

static DBusMessage *manager_dial(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	const char *number;
	const char *clirstr;
	enum ofono_clir_option clir;
	int err;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &number,
					DBUS_TYPE_STRING, &clirstr,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (clir_string_to_clir(clirstr, &clir) == FALSE)
		return __ofono_error_invalid_format(msg);

	vc->pending = dbus_message_ref(msg);

	err = voicecall_dial(vc, number, clir, manager_dial_callback, vc);

	if (err >= 0)
		return NULL;

	vc->pending = NULL;
	dbus_message_unref(msg);

	switch (err) {
	case -EINVAL:
		return __ofono_error_invalid_format(msg);

	case -ENETDOWN:
		return __ofono_error_not_available(msg);

	case -ENOTSUP:
		return __ofono_error_not_implemented(msg);
	}

	return __ofono_error_failed(msg);
}

static DBusMessage *manager_transfer(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	int numactive;
	int numheld;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	numactive = voicecalls_num_active(vc);

	/*
	 * According to 22.091 section 5.8, the network has the option of
	 * implementing the call transfer operation for a call that is
	 * still dialing/alerting.
	 */
	numactive += voicecalls_num_connecting(vc);

	numheld = voicecalls_num_held(vc);

	if (numactive != 1 || numheld != 1)
		return __ofono_error_failed(msg);

	if (vc->driver->transfer == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->transfer(vc, generic_callback, vc);

	return NULL;
}

static DBusMessage *manager_swap_without_accept(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	ofono_voicecall_cb_t cb;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	vc->pending = dbus_message_ref(msg);

	if (voicecalls_have_active(vc) && voicecalls_have_held(vc))
		cb = send_ciev_after_swap_callback;
	else
		cb = generic_callback;

	vc->driver->swap_without_accept(vc, cb, vc);

	return NULL;
}


static DBusMessage *manager_swap_calls(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	ofono_voicecall_cb_t cb;

	if (vc->driver->swap_without_accept)
		return manager_swap_without_accept(conn, msg, data);

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (voicecalls_have_waiting(vc))
		return __ofono_error_failed(msg);

	if (vc->driver->hold_all_active == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	if (voicecalls_have_active(vc) && voicecalls_have_held(vc))
		cb = send_ciev_after_swap_callback;
	else
		cb = generic_callback;

	vc->driver->hold_all_active(vc, cb, vc);

	return NULL;
}

static DBusMessage *manager_release_and_answer(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (!voicecalls_have_waiting(vc))
		return __ofono_error_failed(msg);

	if (vc->driver->release_all_active == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->release_all_active(vc, generic_callback, vc);

	return NULL;
}

static DBusMessage *manager_release_and_swap(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (voicecalls_have_waiting(vc))
		return __ofono_error_failed(msg);

	if (vc->driver->release_all_active == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->release_all_active(vc, generic_callback, vc);

	return NULL;
}

static DBusMessage *manager_hold_and_answer(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (voicecalls_have_waiting(vc) == FALSE)
		return __ofono_error_failed(msg);

	/*
	 * We have waiting call and both an active and held call.  According
	 * to 22.030 we cannot use CHLD=2 in this situation.
	 */
	if (voicecalls_have_active(vc) && voicecalls_have_held(vc))
		return __ofono_error_failed(msg);

	if (vc->driver->hold_all_active == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->hold_all_active(vc, generic_callback, vc);

	return NULL;
}

static DBusMessage *manager_hangup_all(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending || vc->pending_em)
		return __ofono_error_busy(msg);

	if (vc->dial_req && vc->dial_req->call == NULL)
		return __ofono_error_busy(msg);

	if (vc->driver->hangup_all == NULL &&
		(vc->driver->release_specific == NULL ||
			vc->driver->hangup_active == NULL))
		return __ofono_error_not_implemented(msg);

	if (vc->call_list == NULL) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		return reply;
	}

	vc->pending = dbus_message_ref(msg);

	if (vc->driver->hangup_all) {
		vc->driver->hangup_all(vc, generic_callback, vc);
		return NULL;
	}

	if (voicecalls_num_held(vc) > 0)
		vc->driver->hangup_active(vc, hangup_all_active, vc);
	else
		vc->driver->hangup_active(vc, generic_callback, vc);

	return NULL;
}

static void multiparty_callback_common(struct ofono_voicecall *vc,
					DBusMessage *reply)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	char **objpath_list;
	int i;

	voicecalls_path_list(vc, vc->multiparty_list, &objpath_list);

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
		DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (i = 0; objpath_list[i]; i++)
		dbus_message_iter_append_basic(&array_iter,
			DBUS_TYPE_OBJECT_PATH, &objpath_list[i]);

	dbus_message_iter_close_container(&iter, &array_iter);

	g_strfreev(objpath_list);
}

static void private_chat_callback(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;
	const char *callpath;
	const char *c;
	int id;
	GSList *l;
	GSList *old;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("command failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&vc->pending,
					__ofono_error_failed(vc->pending));
		return;
	}

	dbus_message_get_args(vc->pending, NULL,
				DBUS_TYPE_OBJECT_PATH, &callpath,
				DBUS_TYPE_INVALID);

	c = strrchr(callpath, '/');
	sscanf(c, "/voicecall%2u", &id);

	old = g_slist_copy(vc->multiparty_list);

	l = g_slist_find_custom(vc->multiparty_list, GINT_TO_POINTER(id),
				call_compare_by_id);

	if (l) {
		vc->multiparty_list =
			g_slist_remove(vc->multiparty_list, l->data);

		if (vc->multiparty_list->next == NULL) {
			g_slist_free(vc->multiparty_list);
			vc->multiparty_list = NULL;
		}
	}

	reply = dbus_message_new_method_return(vc->pending);
	multiparty_callback_common(vc, reply);
	__ofono_dbus_pending_reply(&vc->pending, reply);

	voicecalls_multiparty_changed(old, vc->multiparty_list);
	g_slist_free(old);
}

static DBusMessage *multiparty_private_chat(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	const char *path = __ofono_atom_get_path(vc->atom);
	const char *callpath;
	const char *c;
	unsigned int id;
	GSList *l;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &callpath,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (strlen(callpath) == 0)
		return __ofono_error_invalid_format(msg);

	c = strrchr(callpath, '/');

	if (c == NULL || strncmp(path, callpath, c-callpath))
		return __ofono_error_not_found(msg);

	if (!sscanf(c, "/voicecall%2u", &id))
		return __ofono_error_not_found(msg);

	for (l = vc->multiparty_list; l; l = l->next) {
		struct voicecall *v = l->data;
		if (v->call->id == id)
			break;
	}

	if (l == NULL)
		return __ofono_error_not_found(msg);

	/*
	 * If we found id on the list of multiparty calls, then by definition
	 * the multiparty call exists.	Only thing to check is whether we have
	 * held calls
	 */
	if (voicecalls_have_held(vc))
		return __ofono_error_failed(msg);

	if (vc->driver->private_chat == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->private_chat(vc, id, private_chat_callback, vc);

	return NULL;
}

static void multiparty_create_callback(const struct ofono_error *error,
					void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;
	GSList *old;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("command failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&vc->pending,
					__ofono_error_failed(vc->pending));
		return;
	}

	/*
	 * We just created a multiparty call, gather all held
	 * active calls and add them to the multiparty list
	 */
	old = vc->multiparty_list;
	vc->multiparty_list = NULL;

	vc->multiparty_list = g_slist_concat(vc->multiparty_list,
						voicecalls_held_list(vc));

	vc->multiparty_list = g_slist_concat(vc->multiparty_list,
						voicecalls_active_list(vc));

	vc->multiparty_list = g_slist_sort(vc->multiparty_list,
						call_compare);

	if (g_slist_length(vc->multiparty_list) < 2) {
		ofono_error("Created multiparty call, but size is less than 2"
				" panic!");

		__ofono_dbus_pending_reply(&vc->pending,
					__ofono_error_failed(vc->pending));
		return;
	}

	reply = dbus_message_new_method_return(vc->pending);
	multiparty_callback_common(vc, reply);
	__ofono_dbus_pending_reply(&vc->pending, reply);

	voicecalls_multiparty_changed(old, vc->multiparty_list);
	g_slist_free(old);
}

static DBusMessage *multiparty_create(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (!voicecalls_have_held(vc) || !voicecalls_have_active(vc))
		return __ofono_error_failed(msg);

	if (vc->driver->create_multiparty == NULL)
		return __ofono_error_not_implemented(msg);

	vc->pending = dbus_message_ref(msg);

	vc->driver->create_multiparty(vc, multiparty_create_callback, vc);

	return NULL;
}

static DBusMessage *multiparty_hangup(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending || vc->dial_req || vc->pending_em)
		return __ofono_error_busy(msg);

	if (vc->driver->release_specific == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->driver->release_all_held == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->driver->release_all_active == NULL)
		return __ofono_error_not_implemented(msg);

	if (vc->multiparty_list == NULL) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		return reply;
	}

	vc->pending = dbus_message_ref(msg);

	/* We don't have waiting calls, as we can't use +CHLD to release */
	if (!voicecalls_have_waiting(vc)) {
		struct voicecall *v = vc->multiparty_list->data;

		if (v->call->status == CALL_STATUS_HELD) {
			vc->driver->release_all_held(vc, generic_callback,
							vc);
			goto out;
		}

		/*
		 * Multiparty is currently active, if we have held calls
		 * we shouldn't use release_all_active here since this also
		 * has the side-effect of activating held calls
		 */
		if (!voicecalls_have_held(vc)) {
			vc->driver->release_all_active(vc, generic_callback,
								vc);
			goto out;
		}
	}

	/* Fall back to the old-fashioned way */
	voicecalls_release_queue(vc, vc->multiparty_list,
					voicecalls_release_done, FALSE);
	voicecalls_release_next(vc);

out:
	return NULL;
}

static void tone_callback(int error, void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;

	if (error)
		reply = __ofono_error_failed(vc->pending);
	else
		reply = dbus_message_new_method_return(vc->pending);

	__ofono_dbus_pending_reply(&vc->pending, reply);
}

static DBusMessage *manager_tone(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	const char *in_tones;
	char *tones;
	int err, len;

	if (vc->pending)
		return __ofono_error_busy(msg);

	if (vc->driver->send_tones == NULL)
		return __ofono_error_not_implemented(msg);

	/* Send DTMFs only if we have at least one connected call */
	if (!voicecalls_can_dtmf(vc))
		return __ofono_error_failed(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &in_tones,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	len = strlen(in_tones);

	if (len == 0)
		return __ofono_error_invalid_format(msg);

	tones = g_ascii_strup(in_tones, len);

	err = tone_queue(vc, tones, tone_callback, vc, NULL);

	g_free(tones);

	if (err < 0)
		return __ofono_error_invalid_format(msg);

	vc->pending = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *manager_get_calls(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;
	DBusMessageIter entry, dict;
	const char *path;
	GSList *l;
	struct voicecall *v;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_OBJECT_PATH_AS_STRING
					DBUS_TYPE_ARRAY_AS_STRING
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING
					DBUS_STRUCT_END_CHAR_AS_STRING,
					&array);

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;

		path = voicecall_build_path(vc, v->call);

		dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT,
							NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
						&path);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

		append_voicecall_properties(v, &dict);
		dbus_message_iter_close_container(&entry, &dict);
		dbus_message_iter_close_container(&array, &entry);
	}

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static const GDBusMethodTable manager_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			manager_get_properties) },
	{ GDBUS_ASYNC_METHOD("Dial",
		GDBUS_ARGS({ "number", "s" }, { "hide_callerid", "s" }),
		GDBUS_ARGS({ "path", "o" }),
		manager_dial) },
	{ GDBUS_ASYNC_METHOD("Transfer", NULL, NULL, manager_transfer) },
	{ GDBUS_ASYNC_METHOD("SwapCalls",  NULL, NULL, manager_swap_calls) },
	{ GDBUS_ASYNC_METHOD("ReleaseAndAnswer", NULL, NULL,
						manager_release_and_answer) },
	{ GDBUS_ASYNC_METHOD("ReleaseAndSwap", NULL, NULL,
						manager_release_and_swap) },
	{ GDBUS_ASYNC_METHOD("HoldAndAnswer", NULL, NULL,
						manager_hold_and_answer) },
	{ GDBUS_ASYNC_METHOD("HangupAll", NULL, NULL,
						manager_hangup_all) },
	{ GDBUS_ASYNC_METHOD("PrivateChat", GDBUS_ARGS({ "call", "o" }),
						GDBUS_ARGS({ "calls", "ao" }),
						multiparty_private_chat) },
	{ GDBUS_ASYNC_METHOD("CreateMultiparty",
					NULL, GDBUS_ARGS({ "calls", "o" }),
					multiparty_create) },
	{ GDBUS_ASYNC_METHOD("HangupMultiparty", NULL, NULL,
							multiparty_hangup) },
	{ GDBUS_ASYNC_METHOD("SendTones",
				GDBUS_ARGS({ "SendTones", "s" }), NULL,
				manager_tone) },
	{ GDBUS_METHOD("GetCalls",
		NULL, GDBUS_ARGS({ "calls_with_properties", "a(oa{sv})" }),
		manager_get_calls) },
	{ }
};

static const GDBusSignalTable manager_signals[] = {
	{ GDBUS_SIGNAL("Forwarded", GDBUS_ARGS({ "type", "s" })) },
	{ GDBUS_SIGNAL("BarringActive", GDBUS_ARGS({ "type", "s" })) },
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ GDBUS_SIGNAL("CallAdded",
		GDBUS_ARGS({ "path", "o" }, { "properties", "a{sv}" })) },
	{ GDBUS_SIGNAL("CallRemoved", GDBUS_ARGS({ "path", "o"})) },
	{ }
};

void ofono_voicecall_disconnected(struct ofono_voicecall *vc, int id,
				enum ofono_disconnect_reason reason,
				const struct ofono_error *error)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	GSList *l;
	struct voicecall *call;
	time_t ts;
	enum call_status prev_status;
	const char *number;

	DBG("Got disconnection event for id: %d, reason: %d", id, reason);

	__ofono_modem_callid_release(modem, id);

	l = g_slist_find_custom(vc->call_list, GUINT_TO_POINTER(id),
				call_compare_by_id);

	if (l == NULL) {
		ofono_error("Plugin notified us of call disconnect for"
				" unknown call");
		return;
	}

	call = l->data;

	ts = time(NULL);
	prev_status = call->call->status;

	l = g_slist_find_custom(vc->multiparty_list, GUINT_TO_POINTER(id),
				call_compare_by_id);

	if (l) {
		vc->multiparty_list =
			g_slist_remove(vc->multiparty_list, call);

		if (vc->multiparty_list->next == NULL) { /* Size == 1 */
			struct voicecall *v = vc->multiparty_list->data;

			voicecall_emit_multiparty(v, FALSE);
			g_slist_free(vc->multiparty_list);
			vc->multiparty_list = NULL;
		}
	}

	vc->release_list = g_slist_remove(vc->release_list, call);

	if (reason != OFONO_DISCONNECT_REASON_UNKNOWN)
		voicecall_emit_disconnect_reason(call, reason);

	number = phone_number_to_string(&call->call->phone_number);
	if (is_emergency_number(vc, number) == TRUE)
		__ofono_modem_dec_emergency_mode(modem);

	voicecall_set_call_status(call, CALL_STATUS_DISCONNECTED);

	if (!call->untracked) {
		if (prev_status == CALL_STATUS_INCOMING ||
				prev_status == CALL_STATUS_WAITING)
			__ofono_history_call_missed(modem, call->call, ts);
		else
			__ofono_history_call_ended(modem, call->call,
							call->detect_time, ts);
	}

	voicecalls_emit_call_removed(vc, call);

	voicecall_dbus_unregister(vc, call);

	vc->call_list = g_slist_remove(vc->call_list, call);
}

void ofono_voicecall_notify(struct ofono_voicecall *vc,
				const struct ofono_call *call)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	GSList *l;
	struct voicecall *v = NULL;
	struct ofono_call *newcall;

	DBG("Got a voicecall event, status: %d, id: %u, number: %s"
			" called_number: %s, called_name %s", call->status,
			call->id, call->phone_number.number,
			call->called_number.number, call->name);

	l = g_slist_find_custom(vc->call_list, GUINT_TO_POINTER(call->id),
				call_compare_by_id);

	if (l) {
		DBG("Found call with id: %d", call->id);
		voicecall_set_call_status(l->data, call->status);
		voicecall_set_call_lineid(l->data, &call->phone_number,
						call->clip_validity);
		voicecall_set_call_calledid(l->data, &call->called_number);
		voicecall_set_call_name(l->data, call->name,
						call->cnap_validity);

		return;
	}

	DBG("Did not find a call with id: %d", call->id);

	__ofono_modem_callid_hold(modem, call->id);

	newcall = g_memdup(call, sizeof(struct ofono_call));
	if (newcall == NULL) {
		ofono_error("Unable to allocate call");
		goto error;
	}

	v = voicecall_create(vc, newcall);
	if (v == NULL) {
		ofono_error("Unable to allocate voicecall_data");
		goto error;
	}

	if (vc->flags & VOICECALL_FLAG_STK_MODEM_CALLSETUP) {
		struct dial_request *req = vc->dial_req;
		const char *phone_number = phone_number_to_string(&req->ph);

		if (!strcmp(phone_number, "112"))
			__ofono_modem_inc_emergency_mode(modem);

		if (v->call->clip_validity == CLIP_VALIDITY_NOT_AVAILABLE) {
			char *number = v->call->phone_number.number;

			v->call->phone_number.type = req->ph.type;
			strncpy(number, req->ph.number,
					OFONO_MAX_PHONE_NUMBER_LENGTH);
			v->call->clip_validity = CLIP_VALIDITY_VALID;
			number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
		}

		v->message = req->message;
		v->icon_id = req->icon_id;

		req->message = NULL;
		req->call = v;

		/*
		 * TS 102 223 Section 6.4.13: The terminal shall not store
		 * in the UICC the call set-up details (called party number
		 * and associated parameters)
		 */
		v->untracked = TRUE;
		vc->flags &= ~VOICECALL_FLAG_STK_MODEM_CALLSETUP;
	}

	v->detect_time = time(NULL);

	if (!voicecall_dbus_register(v)) {
		ofono_error("Unable to register voice call");
		goto error;
	}

	vc->call_list = g_slist_insert_sorted(vc->call_list, v, call_compare);

	voicecalls_emit_call_added(vc, v);

	return;

error:
	if (newcall)
		g_free(newcall);

	if (v)
		g_free(v);
}

static void send_ciev_after_swap_callback(const struct ofono_error *error,
								void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		DBG("command failed with error: %s",
				telephony_error_to_str(error));

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		reply = dbus_message_new_method_return(vc->pending);
		emulator_set_indicator_forced(vc, OFONO_EMULATOR_IND_CALLHELD,
					OFONO_EMULATOR_CALLHELD_MULTIPLE);
	} else
		reply = __ofono_error_failed(vc->pending);

	__ofono_dbus_pending_reply(&vc->pending, reply);
}

static void generic_callback(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		DBG("command failed with error: %s",
				telephony_error_to_str(error));

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(vc->pending);
	else
		reply = __ofono_error_failed(vc->pending);

	__ofono_dbus_pending_reply(&vc->pending, reply);
}

static void hangup_all_active(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&vc->pending,
					__ofono_error_failed(vc->pending));
		return;
	}

	/*
	 * If we have waiting call, we cannot use CHLD=0 due to side effects
	 * to that call.  Instead we try to hangup all calls one by one,
	 * which might fail if the modem / AG does not support release_specific
	 * for held calls.  In that case the waiting call and held calls will
	 * remain.
	 */
	if (vc->driver->release_all_held == NULL ||
			voicecalls_have_waiting(vc)) {
		GSList *held = voicecalls_held_list(vc);

		voicecalls_release_queue(vc, held,
						voicecalls_release_done, FALSE);
		voicecalls_release_next(vc);

		g_slist_free(held);
	} else
		vc->driver->release_all_held(vc, generic_callback, vc);
}

static void multirelease_callback(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->release_list != NULL) {
		voicecalls_release_next(vc);
		return;
	}

	vc->release_queue_done_cb(error, vc);
}

static void emit_en_list_changed(struct ofono_voicecall *vc)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(vc->atom);
	char **list;
	int i;
	GHashTableIter iter;
	gpointer key, value;

	list = g_new0(char *, g_hash_table_size(vc->en_list) + 1);
	g_hash_table_iter_init(&iter, vc->en_list);

	for (i = 0; g_hash_table_iter_next(&iter, &key, &value); i++)
		list[i] = key;

	ofono_dbus_signal_array_property_changed(conn, path,
				OFONO_VOICECALL_MANAGER_INTERFACE,
				"EmergencyNumbers", DBUS_TYPE_STRING, &list);

	g_free(list);
}

static void set_new_ecc(struct ofono_voicecall *vc)
{
	g_hash_table_destroy(vc->en_list);

	vc->en_list = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, NULL);

	/* Emergency numbers from modem/network */
	if (vc->nw_en_list)
		add_to_en_list(vc, vc->nw_en_list);

	/* Emergency numbers read from SIM */
	if (vc->flags & VOICECALL_FLAG_SIM_ECC_READY) {
		GSList *l;

		for (l = vc->sim_en_list; l; l = l->next)
			g_hash_table_insert(vc->en_list, g_strdup(l->data),
							NULL);
	} else
		add_to_en_list(vc, (char **) default_en_list_no_sim);

	/* Default emergency numbers */
	add_to_en_list(vc, (char **) default_en_list);

	emit_en_list_changed(vc);
}

static void free_sim_ecc_numbers(struct ofono_voicecall *vc, gboolean old_only)
{
	/*
	 * Free the currently being read EN list, just in case the
	 * we're still reading them
	 */
	if (old_only == FALSE) {
		if (vc->new_sim_en_list) {
			g_slist_foreach(vc->new_sim_en_list, (GFunc) g_free,
					NULL);
			g_slist_free(vc->new_sim_en_list);
			vc->new_sim_en_list = NULL;
		}

		vc->flags &= ~VOICECALL_FLAG_SIM_ECC_READY;
	}

	if (vc->sim_en_list) {
		g_slist_foreach(vc->sim_en_list, (GFunc) g_free, NULL);
		g_slist_free(vc->sim_en_list);
		vc->sim_en_list = NULL;
	}
}

static void ecc_g2_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	char en[7];

	DBG("%d", ok);

	if (!ok)
		return;

	if (total_length < 3) {
		ofono_error("Unable to read emergency numbers from SIM");
		return;
	}

	free_sim_ecc_numbers(vc, TRUE);

	total_length /= 3;
	while (total_length--) {
		extract_bcd_number(data, 3, en);
		data += 3;

		if (en[0] != '\0')
			vc->sim_en_list = g_slist_prepend(vc->sim_en_list,
								g_strdup(en));
	}

	vc->flags |= VOICECALL_FLAG_SIM_ECC_READY;

	set_new_ecc(vc);
}

static void ecc_g3_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	int total;
	char en[7];

	DBG("%d", ok);

	if (!ok)
		goto check;

	if (record_length < 4 || total_length < record_length) {
		ofono_error("Unable to read emergency numbers from SIM");
		return;
	}

	total = total_length / record_length;
	extract_bcd_number(data, 3, en);

	if (en[0] != '\0')
		vc->new_sim_en_list = g_slist_prepend(vc->new_sim_en_list,
							g_strdup(en));

	if (record != total)
		return;

check:
	if (!ok && vc->new_sim_en_list == NULL)
		return;

	free_sim_ecc_numbers(vc, TRUE);
	vc->sim_en_list = vc->new_sim_en_list;
	vc->new_sim_en_list = NULL;

	vc->flags |= VOICECALL_FLAG_SIM_ECC_READY;

	set_new_ecc(vc);
}

void ofono_voicecall_en_list_notify(struct ofono_voicecall *vc,
						char **nw_en_list)
{
	g_strfreev(vc->nw_en_list);

	vc->nw_en_list = g_strdupv(nw_en_list);
	set_new_ecc(vc);
}

int ofono_voicecall_driver_register(const struct ofono_voicecall_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_voicecall_driver_unregister(const struct ofono_voicecall_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void emulator_remove_handler(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	ofono_emulator_remove_handler(em, data);
}

static void emulator_hfp_unregister(struct ofono_atom *atom)
{
	struct ofono_voicecall *vc = __ofono_atom_get_data(atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_call_status_cb, 0);
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_callsetup_status_cb,
						0);
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_callheld_status_cb, 0);

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"A");
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"+CHUP");
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"+CLCC");
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"+CHLD");
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"+VTS");
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"D");
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"+BLDN");

	__ofono_modem_remove_atom_watch(modem, vc->hfp_watch);
}

static void voicecall_load_settings(struct ofono_voicecall *vc)
{
	const char *imsi;

	imsi = ofono_sim_get_imsi(vc->sim);
	if (imsi == NULL)
		return;

	vc->settings = storage_open(imsi, SETTINGS_STORE);

	if (vc->settings == NULL)
		return;

	vc->imsi = g_strdup(imsi);
}

static void voicecall_close_settings(struct ofono_voicecall *vc)
{
	if (vc->settings) {
		storage_close(vc->imsi, SETTINGS_STORE, vc->settings, TRUE);

		g_free(vc->imsi);
		vc->imsi = NULL;
		vc->settings = NULL;
	}
}

static void voicecall_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_voicecall *vc = __ofono_atom_get_data(atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);
	GSList *l;

	emulator_hfp_unregister(atom);

	voicecall_close_settings(vc);

	if (vc->sim_state_watch) {
		ofono_sim_remove_state_watch(vc->sim, vc->sim_state_watch);
		vc->sim_state_watch = 0;
	}

	if (vc->sim_watch) {
		__ofono_modem_remove_atom_watch(modem, vc->sim_watch);
		vc->sim_watch = 0;
	}

	vc->sim = NULL;

	free_sim_ecc_numbers(vc, FALSE);

	if (vc->nw_en_list) {
		g_strfreev(vc->nw_en_list);
		vc->nw_en_list = NULL;
	}

	g_hash_table_destroy(vc->en_list);
	vc->en_list = NULL;

	if (vc->dial_req)
		dial_request_finish(vc);

	for (l = vc->call_list; l; l = l->next)
		voicecall_dbus_unregister(vc, l->data);

	g_slist_free(vc->call_list);
	vc->call_list = NULL;

	ofono_modem_remove_interface(modem, OFONO_VOICECALL_MANAGER_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					OFONO_VOICECALL_MANAGER_INTERFACE);
}

static void voicecall_remove(struct ofono_atom *atom)
{
	struct ofono_voicecall *vc = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (vc == NULL)
		return;

	if (vc->driver && vc->driver->remove)
		vc->driver->remove(vc);

	if (vc->tone_source) {
		g_source_remove(vc->tone_source);
		vc->tone_source = 0;
	}

	if (vc->toneq) {
		struct tone_queue_entry *entry;

		while ((entry = g_queue_peek_head(vc->toneq)))
			tone_request_finish(vc, entry, ESHUTDOWN, TRUE);

		g_queue_free(vc->toneq);
	}

	g_free(vc);
}

struct ofono_voicecall *ofono_voicecall_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data)
{
	struct ofono_voicecall *vc;
	GSList *l;

	if (driver == NULL)
		return NULL;

	vc = g_try_new0(struct ofono_voicecall, 1);

	if (vc == NULL)
		return NULL;

	vc->toneq = g_queue_new();

	vc->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_VOICECALL,
						voicecall_remove, vc);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_voicecall_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(vc, vendor, data) < 0)
			continue;

		vc->driver = drv;
		break;
	}

	return vc;
}

static void read_sim_ecc_numbers(int id, void *userdata)
{
	struct ofono_voicecall *vc = userdata;

	/* Try both formats, only one or none will work */
	ofono_sim_read(vc->sim_context, SIM_EFECC_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			ecc_g2_read_cb, vc);
	ofono_sim_read(vc->sim_context, SIM_EFECC_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED,
			ecc_g3_read_cb, vc);
}

static void sim_state_watch(enum ofono_sim_state new_state, void *user)
{
	struct ofono_voicecall *vc = user;

	switch (new_state) {
	case OFONO_SIM_STATE_INSERTED:
		if (vc->sim_context == NULL)
			vc->sim_context = ofono_sim_context_create(vc->sim);

		read_sim_ecc_numbers(SIM_EFECC_FILEID, vc);

		ofono_sim_add_file_watch(vc->sim_context, SIM_EFECC_FILEID,
						read_sim_ecc_numbers, vc, NULL);
		break;
	case OFONO_SIM_STATE_NOT_PRESENT:
	case OFONO_SIM_STATE_RESETTING:
		/* TODO: Must release all non-emergency calls */

		if (vc->sim_context) {
			ofono_sim_context_free(vc->sim_context);
			vc->sim_context = NULL;
		}

		free_sim_ecc_numbers(vc, FALSE);
		set_new_ecc(vc);

		voicecall_close_settings(vc);
		break;
	case OFONO_SIM_STATE_READY:
		voicecall_load_settings(vc);
		break;
	case OFONO_SIM_STATE_LOCKED_OUT:
		voicecall_close_settings(vc);
		break;
	}
}

static void sim_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *data)
{
	struct ofono_voicecall *vc = data;
	struct ofono_sim *sim = __ofono_atom_get_data(atom);

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		voicecall_close_settings(vc);
		vc->sim_state_watch = 0;
		vc->sim = NULL;
		return;
	}

	vc->sim = sim;
	vc->sim_state_watch = ofono_sim_add_state_watch(sim,
							sim_state_watch,
							vc, NULL);

	sim_state_watch(ofono_sim_get_state(sim), vc);
}

static void emulator_send_ciev_after_swap_cb(const struct ofono_error *error,
								void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending_em == NULL)
		return;

	ofono_emulator_send_final(vc->pending_em, error);

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		emulator_set_indicator_forced(vc, OFONO_EMULATOR_IND_CALLHELD,
					OFONO_EMULATOR_CALLHELD_MULTIPLE);

	vc->pending_em = NULL;
}

static void emulator_generic_cb(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;

	if (vc->pending_em == NULL)
		return;

	ofono_emulator_send_final(vc->pending_em, error);
	vc->pending_em = NULL;
}

static void emulator_mpty_join_cb(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	GSList *old;

	if (vc->pending_em != NULL)
		ofono_emulator_send_final(vc->pending_em, error);

	vc->pending_em = NULL;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		return;

	/*
	 * We just created a multiparty call, gather all held
	 * active calls and add them to the multiparty list
	 */
	old = vc->multiparty_list;
	vc->multiparty_list = NULL;

	vc->multiparty_list = g_slist_concat(vc->multiparty_list,
						voicecalls_held_list(vc));

	vc->multiparty_list = g_slist_concat(vc->multiparty_list,
						voicecalls_active_list(vc));

	vc->multiparty_list = g_slist_sort(vc->multiparty_list,
						call_compare);

	if (g_slist_length(vc->multiparty_list) < 2) {
		ofono_error("Created multiparty call, but size is less than 2"
				" panic!");
		g_slist_free(old);
		return;
	}

	voicecalls_multiparty_changed(old, vc->multiparty_list);
	g_slist_free(old);
}

static void emulator_mpty_private_chat_cb(const struct ofono_error *error,
							void *data)
{
	struct ofono_voicecall *vc = data;
	GSList *old;
	GSList *l;

	if (vc->pending_em != NULL)
		ofono_emulator_send_final(vc->pending_em, error);

	vc->pending_em = NULL;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		return;

	old = g_slist_copy(vc->multiparty_list);

	l = g_slist_find_custom(vc->multiparty_list,
			GINT_TO_POINTER(vc->pending_id), call_compare_by_id);

	if (l) {
		vc->multiparty_list =
			g_slist_remove(vc->multiparty_list, l->data);

		if (vc->multiparty_list->next == NULL) {
			g_slist_free(vc->multiparty_list);
			vc->multiparty_list = NULL;
		}
	}

	voicecalls_multiparty_changed(old, vc->multiparty_list);
	g_slist_free(old);
}

#define CHECK_BUSY(vc, em, result)				\
	if (vc->pending || vc->dial_req || vc->pending_em) {	\
		result.error = 126;				\
		result.type = OFONO_ERROR_TYPE_CME;		\
		ofono_emulator_send_final(em, &result);		\
	}							\

static void emulator_ata_cb(struct ofono_emulator *em,
				struct ofono_emulator_request *req,
				void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	struct ofono_error result;

	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY:
		CHECK_BUSY(vc, em, result)

		if (!voicecalls_have_incoming(vc))
			goto fail;

		if (vc->driver->answer == NULL)
			goto fail;

		vc->pending_em = em;
		vc->driver->answer(vc, emulator_generic_cb, vc);
		break;

	default:
fail:
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
	};
}

static void emulator_chup_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	struct ofono_error result;

	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY:
		if (vc->pending || vc->pending_em)
			goto fail;

		if (vc->dial_req && vc->dial_req->call == NULL)
			goto fail;

		if (vc->driver->release_specific == NULL &&
				vc->driver->hangup_active == NULL)
			goto fail;

		if (vc->driver->hangup_active) {
			vc->pending_em = em;
			vc->driver->hangup_active(vc, emulator_generic_cb, vc);
			goto done;
		}

		if (voicecalls_have_active(vc) == FALSE &&
				voicecalls_have_incoming(vc) == FALSE)
			goto fail;

		vc->pending_em = em;
		voicecalls_release_queue(vc, vc->call_list,
						emulator_generic_cb, TRUE);
		voicecalls_release_next(vc);

done:
		break;

	default:
fail:
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
	};
}

static void emulator_clcc_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	struct ofono_error result;
	GSList *l;
	/*
	 *          idx   dir  stat  mode  mpty
	 * '+CLCC: <0-7>,<0-1>,<0-5>,<0-9>,<0-1>,"",' +
	 * phone number + phone type on 3 digits + terminating null
	 */
	char buf[20 + OFONO_MAX_PHONE_NUMBER_LENGTH + 3 + 1];

	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY:
		for (l = vc->call_list; l; l = l->next) {
			struct voicecall *v = l->data;
			const char *number = "";
			int type = 128;
			gboolean mpty;

			if (g_slist_find_custom(vc->multiparty_list,
						GINT_TO_POINTER(v->call->id),
						call_compare_by_id))
				mpty = TRUE;
			else
				mpty = FALSE;

			if (v->call->clip_validity == CLIP_VALIDITY_VALID) {
				number = v->call->phone_number.number;
				type = v->call->phone_number.type;
			}

			sprintf(buf, "+CLCC: %d,%d,%d,0,%d,\"%s\",%d",
					v->call->id, v->call->direction,
					v->call->status, mpty, number, type);
			ofono_emulator_send_info(em, buf, l->next == NULL ?
							TRUE : FALSE);
		}

		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		break;

	default:
		result.type = OFONO_ERROR_TYPE_FAILURE;
	}

	ofono_emulator_send_final(em, &result);
}

#define ADD_CHLD_SUPPORT(cond, x)			\
	if (cond) {					\
		if (info[-1] != '(')			\
			*info++ = ',';			\
							\
		*info++ = x[0];				\
							\
		if (x[1])				\
			*info++ = x[1];			\
	}						\

static void emulator_chld_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	struct ofono_error result;
	char buf[64];
	char *info;
	int chld;
	ofono_voicecall_cb_t cb;

	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_SET:
		if (!ofono_emulator_request_next_number(req, &chld))
			goto fail;

		CHECK_BUSY(vc, em, result)

		switch (chld) {
		case 0:
			if (vc->driver->set_udub == NULL)
				goto fail;

			if (vc->driver->release_all_held == NULL)
				goto fail;

			vc->pending_em = em;

			if (voicecalls_have_waiting(vc)) {
				vc->driver->set_udub(vc,
						emulator_generic_cb, vc);
				return;
			}

			vc->driver->release_all_held(vc,
					emulator_generic_cb, vc);
			return;
		case 1:
			if (vc->driver->release_all_active == NULL)
				goto fail;

			vc->pending_em = em;
			vc->driver->release_all_active(vc,
					emulator_generic_cb, vc);
			return;
		case 2:
			if (vc->driver->hold_all_active == NULL)
				goto fail;

			if (voicecalls_have_active(vc) &&
					voicecalls_have_held(vc))
				cb = emulator_send_ciev_after_swap_cb;
			else
				cb = emulator_generic_cb;

			vc->pending_em = em;
			vc->driver->hold_all_active(vc, cb, vc);
			return;
		case 3:
			if (vc->driver->create_multiparty == NULL)
				goto fail;

			if (!voicecalls_have_held(vc) ||
					!voicecalls_have_active(vc))
				goto fail;

			vc->pending_em = em;
			vc->driver->create_multiparty(vc,
					emulator_mpty_join_cb, vc);
			return;
		case 4:
			if (vc->driver->transfer == NULL)
				goto fail;

			vc->pending_em = em;
			vc->driver->transfer(vc,
					emulator_generic_cb, vc);
			return;
		default:
			break;
		}

		if (chld >= 11 && chld <= 17) {
			if (vc->driver->release_specific == NULL)
				goto fail;

			vc->pending_em = em;
			vc->driver->release_specific(vc, chld - 10,
						emulator_generic_cb, vc);
			return;
		}

		if (chld >= 21 && chld <= 27) {
			GSList *l;
			unsigned int id = chld - 20;

			if (vc->driver->private_chat == NULL)
				goto fail;

			for (l = vc->multiparty_list; l; l = l->next) {
				struct voicecall *v = l->data;
				if (v->call->id == id)
					break;
			}

			if (l == NULL)
				goto fail;

			if (voicecalls_have_held(vc))
				goto fail;

			vc->pending_em = em;
			vc->pending_id = id;

			vc->driver->private_chat(vc, id,
					emulator_mpty_private_chat_cb, vc);
			return;
		}

		goto fail;

	case OFONO_EMULATOR_REQUEST_TYPE_SUPPORT:
		memcpy(buf, "+CHLD: (", 8);
		info = buf + 8;

		ADD_CHLD_SUPPORT(vc->driver->release_all_held &&
					vc->driver->set_udub, "0")
		ADD_CHLD_SUPPORT(vc->driver->release_all_active, "1")
		ADD_CHLD_SUPPORT(vc->driver->release_specific, "1x")
		ADD_CHLD_SUPPORT(vc->driver->hold_all_active, "2")
		ADD_CHLD_SUPPORT(vc->driver->private_chat, "2x")
		ADD_CHLD_SUPPORT(vc->driver->create_multiparty, "3")
		ADD_CHLD_SUPPORT(vc->driver->transfer, "4")

		*info++ = ')';
		*info++ = '\0';

		ofono_emulator_send_info(em, buf, TRUE);
		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		break;

	case OFONO_EMULATOR_REQUEST_TYPE_QUERY:
	case OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY:
fail:
		result.type = OFONO_ERROR_TYPE_FAILURE;
	}

	ofono_emulator_send_final(em, &result);
}

static void vts_tone_cb(int error, void *data)
{
	struct ofono_emulator *em = data;
	struct ofono_error result;

	result.error = 0;
	result.type = error ? OFONO_ERROR_TYPE_FAILURE :
						OFONO_ERROR_TYPE_NO_ERROR;

	ofono_emulator_send_final(em, &result);
}

static void emulator_vts_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	struct ofono_error result;
	const char *str;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_SET:
		str = ofono_emulator_request_get_raw(req);
		if (str == NULL)
			break;

		if (!g_ascii_isdigit(str[0]) && str[0] != '*' &&
				str[0] != '#' && (str[0] < 'A' || str[0] > 'D'))
			break;

		if (str[1] != '\0')
			break;

		if (__ofono_voicecall_tone_send(vc, str, vts_tone_cb, em) >= 0)
			return;

		break;

	default:
		break;
	}

	result.error = 0;
	result.type = OFONO_ERROR_TYPE_FAILURE;

	ofono_emulator_send_final(em, &result);
}

static void emulator_dial_callback(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	gboolean need_to_emit;
	struct voicecall *v;
	const char *number;
	GError *err = NULL;

	number = g_key_file_get_string(vc->settings, SETTINGS_GROUP,
					"Number", &err);

	v = dial_handle_result(vc, error, number, &need_to_emit);

	if (v == NULL) {
		struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);

		if (is_emergency_number(vc, number) == TRUE)
			__ofono_modem_dec_emergency_mode(modem);
	}

	if (vc->pending_em)
		ofono_emulator_send_final(vc->pending_em, error);

	vc->pending_em = NULL;

	if (need_to_emit)
		voicecalls_emit_call_added(vc, v);
}

static void emulator_dial(struct ofono_emulator *em, struct ofono_voicecall *vc,
				const char *number)
{
	struct ofono_error result;
	int err;

	result.error = 0;

	if (vc->pending || vc->dial_req || vc->pending_em) {
		result.type = OFONO_ERROR_TYPE_FAILURE;
		goto send;
	}

	vc->pending_em = em;

	err = voicecall_dial(vc, number, OFONO_CLIR_OPTION_DEFAULT,
					emulator_dial_callback, vc);

	if (err >= 0)
		return;

	vc->pending_em = NULL;

	switch (err) {
	case -ENETDOWN:
		result.error = 30;
		result.type = OFONO_ERROR_TYPE_CME;
		break;

	default:
		result.type = OFONO_ERROR_TYPE_FAILURE;
	}

send:
	ofono_emulator_send_final(em, &result);
}

static void emulator_atd_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	const char *str;
	size_t len;
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 1];
	struct ofono_error result;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_SET:
		str = ofono_emulator_request_get_raw(req);

		if (str == NULL || str[0] == '\0')
			goto fail;

		len = strlen(str);

		if (len > OFONO_MAX_PHONE_NUMBER_LENGTH + 1 ||
				str[len - 1] != ';')
			goto fail;

		if (len == 3 && str[0] == '>' && str[1] == '1') {
			struct ofono_message_waiting *mw;
			const struct ofono_phone_number *ph;
			const char *num;

			mw = __ofono_atom_find(OFONO_ATOM_TYPE_MESSAGE_WAITING,
						modem);
			if (mw == NULL)
				goto fail;

			ph = __ofono_message_waiting_get_mbdn(mw, 0);

			if (ph == NULL)
				goto fail;

			num = phone_number_to_string(ph);

			emulator_dial(em, vc, num);
		} else {
			strncpy(number, str, len - 1);
			number[len - 1] = '\0';

			emulator_dial(em, vc, number);
		}

		break;

	default:
fail:
		result.error = 0;
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
	};
}

static void emulator_bldn_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_voicecall *vc = userdata;
	const char *number;
	struct ofono_error result;
	GError *error = NULL;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY:
		if (vc->settings == NULL)
			goto fail;

		number = g_key_file_get_string(vc->settings, SETTINGS_GROUP,
						"Number", &error);
		if (number == NULL || number[0] == '\0')
			goto fail;

		emulator_dial(em, vc, number);
		break;

	default:
fail:
		result.error = 0;
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
	};
}

static void emulator_hfp_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	struct ofono_voicecall *vc = data;

	switch (cond) {
	case OFONO_ATOM_WATCH_CONDITION_UNREGISTERED:
		if (vc->pending_em == em)
			vc->pending_em = NULL;

		return;
	case OFONO_ATOM_WATCH_CONDITION_REGISTERED:
		break;
	}

	notify_emulator_call_status(vc);

	ofono_emulator_add_handler(em, "A", emulator_ata_cb, vc, NULL);
	ofono_emulator_add_handler(em, "+CHUP", emulator_chup_cb, vc, NULL);
	ofono_emulator_add_handler(em, "+CLCC", emulator_clcc_cb, vc, NULL);
	ofono_emulator_add_handler(em, "+CHLD", emulator_chld_cb, vc, NULL);
	ofono_emulator_add_handler(em, "+VTS", emulator_vts_cb, vc, NULL);
	ofono_emulator_add_handler(em, "D", emulator_atd_cb, vc, NULL);
	ofono_emulator_add_handler(em, "+BLDN", emulator_bldn_cb, vc, NULL);
}

void ofono_voicecall_register(struct ofono_voicecall *vc)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);
	const char *path = __ofono_atom_get_path(vc->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_VOICECALL_MANAGER_INTERFACE,
					manager_methods, manager_signals, NULL,
					vc, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_VOICECALL_MANAGER_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_VOICECALL_MANAGER_INTERFACE);

	vc->en_list = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, NULL);

	/*
	 * Start out with the 22.101 mandated numbers, if we have a SIM and
	 * the SIM contains EFecc, then we update the list once we've read them
	 */
	add_to_en_list(vc, (char **) default_en_list_no_sim);
	add_to_en_list(vc, (char **) default_en_list);

	vc->sim_watch = __ofono_modem_add_atom_watch(modem,
						OFONO_ATOM_TYPE_SIM,
						sim_watch, vc, NULL);

	__ofono_atom_register(vc->atom, voicecall_unregister);

	vc->hfp_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_EMULATOR_HFP,
					emulator_hfp_watch, vc, NULL);
}

void ofono_voicecall_remove(struct ofono_voicecall *vc)
{
	__ofono_atom_free(vc->atom);
}

void ofono_voicecall_set_data(struct ofono_voicecall *vc, void *data)
{
	vc->driver_data = data;
}

void *ofono_voicecall_get_data(struct ofono_voicecall *vc)
{
	return vc->driver_data;
}

int ofono_voicecall_get_next_callid(struct ofono_voicecall *vc)
{
	struct ofono_modem *modem;
	if (vc == NULL || vc->atom == NULL)
		return 0;

	modem = __ofono_atom_get_modem(vc->atom);

	return __ofono_modem_callid_next(modem);
}

ofono_bool_t __ofono_voicecall_is_busy(struct ofono_voicecall *vc,
					enum ofono_voicecall_interaction type)
{
	if (vc->pending || vc->dial_req || vc->pending_em)
		return TRUE;

	switch (type) {
	case OFONO_VOICECALL_INTERACTION_NONE:
		return vc->call_list != NULL;
	case OFONO_VOICECALL_INTERACTION_DISCONNECT:
		/* Only support releasing active calls */
		if (voicecalls_num_active(vc) == g_slist_length(vc->call_list))
			return FALSE;

		return TRUE;
	case OFONO_VOICECALL_INTERACTION_PUT_ON_HOLD:
		if (voicecalls_num_active(vc) == g_slist_length(vc->call_list))
			return FALSE;

		if (voicecalls_num_held(vc) == g_slist_length(vc->call_list))
			return FALSE;

		return TRUE;
	}

	return TRUE;
}

static void dial_request_cb(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	const char *number = phone_number_to_string(&vc->dial_req->ph);
	gboolean need_to_emit;
	struct voicecall *v;

	v = dial_handle_result(vc, error, number, &need_to_emit);

	if (v == NULL) {
		if (is_emergency_number(vc, number) == TRUE) {
			struct ofono_modem *modem =
				__ofono_atom_get_modem(vc->atom);

			__ofono_modem_dec_emergency_mode(modem);
		}

		dial_request_finish(vc);
		return;
	}

	v->message = vc->dial_req->message;
	v->icon_id = vc->dial_req->icon_id;

	vc->dial_req->message = NULL;
	vc->dial_req->call = v;

	/*
	 * TS 102 223 Section 6.4.13: The terminal shall not store
	 * in the UICC the call set-up details (called party number
	 * and associated parameters)
	 */
	v->untracked = TRUE;

	if (v->call->status == CALL_STATUS_ACTIVE)
		dial_request_finish(vc);

	if (need_to_emit)
		voicecalls_emit_call_added(vc, v);
}

static void dial_request(struct ofono_voicecall *vc)
{
	const char *number = phone_number_to_string(&vc->dial_req->ph);

	if (is_emergency_number(vc, number) == TRUE) {
		struct ofono_modem *modem = __ofono_atom_get_modem(vc->atom);

		__ofono_modem_inc_emergency_mode(modem);
	}

	vc->driver->dial(vc, &vc->dial_req->ph, OFONO_CLIR_OPTION_DEFAULT,
				dial_request_cb, vc);
}

static void dial_req_disconnect_cb(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		dial_request_finish(vc);
		return;
	}

	/*
	 * Note that the callback might come back fore we receive call
	 * disconnection notifications.  So it makes no sense to recheck
	 * whether we can dial here.  We simply dial and hope for the best.
	 */
	dial_request(vc);
}

int __ofono_voicecall_dial(struct ofono_voicecall *vc,
				const char *addr, int addr_type,
				const char *message, unsigned char icon_id,
				enum ofono_voicecall_interaction interaction,
				ofono_voicecall_dial_cb_t cb, void *user_data)
{
	struct dial_request *req;

	if (!valid_long_phone_number_format(addr))
		return -EINVAL;

	if (vc->driver->dial == NULL)
		return -ENOSYS;

	if (interaction == OFONO_VOICECALL_INTERACTION_DISCONNECT &&
			vc->driver->release_all_active == NULL)
		return -ENOSYS;

	if (__ofono_voicecall_is_busy(vc, interaction) == TRUE)
		return -EBUSY;

	/*
	 * TODO: if addr starts with "112", possibly translate into the
	 * technology-specific emergency number.
	 */

	req = g_try_new0(struct dial_request, 1);
	if (req == NULL)
		return -ENOMEM;

	req->message = g_strdup(message);
	req->icon_id = icon_id;
	req->interaction = interaction;
	req->cb = cb;
	req->user_data = user_data;

	/* TODO: parse the tones to dial after call connected */
	req->ph.type = addr_type;
	strncpy(req->ph.number, addr, OFONO_MAX_PHONE_NUMBER_LENGTH);

	vc->dial_req = req;

	switch (interaction) {
	case OFONO_VOICECALL_INTERACTION_NONE:
		dial_request(vc);
		break;

	case OFONO_VOICECALL_INTERACTION_PUT_ON_HOLD:
		/* Note: dialling automatically puts active calls on hold */
		dial_request(vc);
		break;

	case OFONO_VOICECALL_INTERACTION_DISCONNECT:
		if (voicecalls_have_active(vc))
			vc->driver->release_all_active(vc,
						dial_req_disconnect_cb, vc);
		else
			dial_request(vc);

		break;
	}

	return 0;
}

void __ofono_voicecall_dial_cancel(struct ofono_voicecall *vc)
{
	if (vc->dial_req == NULL || vc->dial_req->cb == NULL)
		return;

	vc->dial_req->cb = NULL;
}

static void tone_request_cb(const struct ofono_error *error, void *data)
{
	struct ofono_voicecall *vc = data;
	struct tone_queue_entry *entry = g_queue_peek_head(vc->toneq);
	int len = 0;

	if (entry == NULL)
		return;

	/*
	 * Call back with error only if the error is related to the
	 * current entry.  If the error corresponds to a cancelled
	 * request, do nothing.
	 */
	if (error && error->type != OFONO_ERROR_TYPE_NO_ERROR &&
			entry->left > entry->tone_str) {
		DBG("command failed with error: %s",
				telephony_error_to_str(error));

		tone_request_finish(vc, entry, EIO, TRUE);

		goto done;
	}

	if (*entry->left == '\0') {
		tone_request_finish(vc, entry, 0, TRUE);

		goto done;
	}

	len = strspn(entry->left, "pP");
	entry->left += len;

done:
	/*
	 * Wait 3 seconds per PAUSE, same as for DTMF separator characters
	 * passed in a telephone number according to TS 22.101 A.21,
	 * although 27.007 claims this delay can be set using S8 and
	 * defaults to 2 seconds.
	 */
	vc->tone_source = g_timeout_add_seconds(len * 3, tone_request_run, vc);
}

static gboolean tone_request_run(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct tone_queue_entry *entry = g_queue_peek_head(vc->toneq);
	char final;
	unsigned len;

	vc->tone_source = 0;

	if (entry == NULL)
		return FALSE;

	len = strcspn(entry->left, "pP");

	if (len) {
		if (len > 8) /* Arbitrary length limit per request */
			len = 8;

		/* Temporarily move the end of the string */
		final = entry->left[len];
		entry->left[len] = '\0';

		vc->driver->send_tones(vc, entry->left, tone_request_cb, vc);

		entry->left += len;
		entry->left[0] = final;
	} else
		tone_request_cb(NULL, vc);

	return FALSE;
}

int __ofono_voicecall_tone_send(struct ofono_voicecall *vc,
				const char *tone_str,
				ofono_voicecall_tone_cb_t cb, void *user_data)
{
	if (vc->driver->send_tones == NULL)
		return -ENOSYS;

	/* Send DTMFs only if we have at least one connected call */
	if (!voicecalls_can_dtmf(vc))
		return -ENOENT;

	return tone_queue(vc, tone_str, cb, user_data, NULL);
}

void __ofono_voicecall_tone_cancel(struct ofono_voicecall *vc, int id)
{
	struct tone_queue_entry *entry;
	int n = 0;

	while ((entry = g_queue_peek_nth(vc->toneq, n++)) != NULL)
		if (entry->id == id)
			break;

	tone_request_finish(vc, entry, 0, FALSE);

	/*
	 * If we were in the middle of a PAUSE, wake queue up
	 * now, else wake up when current tone finishes.
	 */
	if (n == 1 && vc->tone_source) {
		g_source_remove(vc->tone_source);
		tone_request_run(vc);
	}
}

void __ofono_voicecall_set_alpha_and_icon_id(struct ofono_voicecall *vc,
						const char *addr, int addr_type,
						const char *message,
						unsigned char icon_id)
{
	struct dial_request *req;

	req = g_new0(struct dial_request, 1);

	req->message = g_strdup(message);
	req->icon_id = icon_id;

	req->ph.type = addr_type;
	strncpy(req->ph.number, addr, OFONO_MAX_PHONE_NUMBER_LENGTH);

	vc->dial_req = req;

	vc->flags |= VOICECALL_FLAG_STK_MODEM_CALLSETUP;

	DBG("%p, %p", vc, vc->dial_req);
}

void __ofono_voicecall_clear_alpha_and_icon_id(struct ofono_voicecall *vc)
{
	DBG("%p, %p", vc, vc->dial_req);

	if (vc->dial_req) {
		g_free(vc->dial_req->message);
		vc->dial_req->message = NULL;

		g_free(vc->dial_req);
		vc->dial_req = NULL;
	}

	vc->flags &= ~VOICECALL_FLAG_STK_MODEM_CALLSETUP;
}

static void ssn_mt_forwarded_notify(struct ofono_voicecall *vc,
					unsigned int id, int code,
					const struct ofono_phone_number *ph)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(vc->atom);
	char *info = "incoming";

	g_dbus_emit_signal(conn, path, OFONO_VOICECALL_MANAGER_INTERFACE,
				"Forwarded",
				DBUS_TYPE_STRING, &info,
				DBUS_TYPE_INVALID);
}

static struct voicecall *voicecall_select(struct ofono_voicecall *vc,
						unsigned int id)
{
	if (id != 0) {
		GSList *l = g_slist_find_custom(vc->call_list,
						GUINT_TO_POINTER(id),
						call_compare_by_id);

		if (l == NULL)
			return NULL;

		return l->data;
	}

	if (g_slist_length(vc->call_list) == 1)
		return vc->call_list->data;

	return NULL;
}

static void ssn_mt_remote_held_notify(struct ofono_voicecall *vc,
					unsigned int id, gboolean held,
					const struct ofono_phone_number *ph)
{
	struct voicecall *v = voicecall_select(vc, id);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;

	if (v == NULL)
		return;

	if (v->remote_held == held)
		return;

	v->remote_held = held;
	path = voicecall_build_path(vc, v->call);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_VOICECALL_INTERFACE,
						"RemoteHeld", DBUS_TYPE_BOOLEAN,
						&v->remote_held);
}

static void ssn_mt_remote_multiparty_notify(struct ofono_voicecall *vc,
					unsigned int id,
					const struct ofono_phone_number *ph)
{
	struct voicecall *v = voicecall_select(vc, id);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;

	if (v == NULL)
		return;

	if (v->remote_multiparty == TRUE)
		return;

	v->remote_multiparty = TRUE;

	path = voicecall_build_path(vc, v->call);

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_VOICECALL_INTERFACE,
					"RemoteMultiparty", DBUS_TYPE_BOOLEAN,
					&v->remote_multiparty);
}

void ofono_voicecall_ssn_mt_notify(struct ofono_voicecall *vc,
					unsigned int id, int code, int index,
					const struct ofono_phone_number *ph)
{
	switch (code) {
	case SS_MT_CALL_FORWARDED:
		ssn_mt_forwarded_notify(vc, id, code, ph);
		break;
	case SS_MT_VOICECALL_ON_HOLD:
		ssn_mt_remote_held_notify(vc, id, TRUE, ph);
		break;
	case SS_MT_VOICECALL_RETRIEVED:
		ssn_mt_remote_held_notify(vc, id, FALSE, ph);
		break;
	case SS_MT_MULTIPARTY_VOICECALL:
		ssn_mt_remote_multiparty_notify(vc, id, ph);
		break;
	}
}

static void ssn_mo_call_barred_notify(struct ofono_voicecall *vc,
					unsigned int id, int code)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(vc->atom);
	const char *info;

	if (code == SS_MO_INCOMING_BARRING)
		info = "remote";
	else
		info = "local";

	g_dbus_emit_signal(conn, path, OFONO_VOICECALL_MANAGER_INTERFACE,
				"BarringActive",
				DBUS_TYPE_STRING, &info,
				DBUS_TYPE_INVALID);
}

static void ssn_mo_forwarded_notify(struct ofono_voicecall *vc,
					unsigned int id, int code)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(vc->atom);
	char *info = "outgoing";

	g_dbus_emit_signal(conn, path, OFONO_VOICECALL_MANAGER_INTERFACE,
				"Forwarded",
				DBUS_TYPE_STRING, &info,
				DBUS_TYPE_INVALID);
}

void ofono_voicecall_ssn_mo_notify(struct ofono_voicecall *vc,
					unsigned int id, int code, int index)
{
	switch (code) {
	case SS_MO_OUTGOING_BARRING:
	case SS_MO_INCOMING_BARRING:
		ssn_mo_call_barred_notify(vc, id, code);
		break;
	case SS_MO_CALL_FORWARDED:
		ssn_mo_forwarded_notify(vc, id, code);
		break;
	}
}
