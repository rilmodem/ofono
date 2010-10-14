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
#include <errno.h>

#include <glib.h>
#include <gdbus.h>
#include <sys/time.h>

#include "ofono.h"

#include "common.h"
#include "util.h"
#include "smsutil.h"
#include "storage.h"

#define uninitialized_var(x) x = x

#define MESSAGE_MANAGER_FLAG_CACHED 0x1

#define SETTINGS_STORE "sms"
#define SETTINGS_GROUP "Settings"

#define TXQ_MAX_RETRIES 4

static gboolean tx_next(gpointer user_data);

static GSList *g_drivers = NULL;

enum message_state {
	MESSAGE_STATE_PENDING =		0,
	MESSAGE_STATE_SENT,
	MESSAGE_STATE_FAILED
};

struct message {
	struct ofono_uuid uuid;
	enum message_state state;
};

struct sms_handler {
	struct ofono_watchlist_item item;
	int dst;
	int src;
};

struct ofono_sms {
	int flags;
	DBusMessage *pending;
	struct ofono_phone_number sca;
	struct sms_assembly *assembly;
	guint ref;
	GQueue *txq;
	guint tx_source;
	struct ofono_message_waiting *mw;
	unsigned int mw_watch;
	struct ofono_sim *sim;
	GKeyFile *settings;
	char *imsi;
	int bearer;
	const struct ofono_sms_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	ofono_bool_t use_delivery_reports;
	struct status_report_assembly *sr_assembly;
	GHashTable *messages;
	struct ofono_watchlist *text_handlers;
	struct ofono_watchlist *datagram_handlers;
};

struct pending_pdu {
	unsigned char pdu[176];
	int tpdu_len;
	int pdu_len;
};

struct tx_queue_entry {
	struct pending_pdu *pdus;
	unsigned char num_pdus;
	unsigned char cur_pdu;
	struct sms_address receiver;
	struct ofono_uuid uuid;
	unsigned int retry;
	unsigned int flags;
	ofono_sms_txq_submit_cb_t cb;
	void *data;
	ofono_destroy_func destroy;
};

static gboolean uuid_equal(gconstpointer v1, gconstpointer v2)
{
	return memcmp(v1, v2, OFONO_SHA1_UUID_LEN) == 0;
}

static gboolean port_equal(int received, int expected)
{
	return expected == -1 || received == expected;
}

static guint uuid_hash(gconstpointer v)
{
	const struct ofono_uuid *uuid = v;
	guint h;

	memcpy(&h, uuid->uuid, sizeof(h));

	return h;
}

static const char *sms_bearer_to_string(int bearer)
{
	switch (bearer) {
	case 0:
		return "ps-only";
	case 1:
		return "cs-only";
	case 2:
		return "ps-preferred";
	case 3:
		return "cs-preferred";
	};

	return "unknown";
}

static int sms_bearer_from_string(const char *str)
{
	if (g_str_equal(str, "ps-only"))
		return 0;
	else if (g_str_equal(str, "cs-only"))
		return 1;
	else if (g_str_equal(str, "ps-preferred"))
		return 2;
	else if (g_str_equal(str, "cs-preferred"))
		return 3;

	return -1;
}

static const char *message_state_to_string(enum message_state s)
{
	switch (s) {
	case MESSAGE_STATE_PENDING:
		return "pending";
	case MESSAGE_STATE_SENT:
		return "sent";
	case MESSAGE_STATE_FAILED:
		return "failed";
	}

	return "invalid";
}

static void append_message_properties(struct message *m, DBusMessageIter *dict)
{
	const char *state;

	state = message_state_to_string(m->state);
	ofono_dbus_dict_append(dict, "State", DBUS_TYPE_STRING, &state);
}

static unsigned int add_sms_handler(struct ofono_watchlist *watchlist,
					int dst, int src, void *notify,
					void *data, ofono_destroy_func destroy)
{
	struct sms_handler *handler;

	if (!notify)
		return 0;

	handler = g_try_new0(struct sms_handler, 1);
	if (!handler)
		return 0;

	handler->dst = dst;
	handler->src = src;
	handler->item.notify = notify;
	handler->item.notify_data = data;
	handler->item.destroy = destroy;

	return __ofono_watchlist_add_item(watchlist,
				(struct ofono_watchlist_item *)handler);
}

unsigned int __ofono_sms_text_watch_add(struct ofono_sms *sms,
					ofono_sms_text_notify_cb_t cb,
					void *data, ofono_destroy_func destroy)
{
	if (!sms)
		return 0;

	DBG("%p", sms);

	return add_sms_handler(sms->text_handlers, -1, -1, cb, data, destroy);
}

gboolean __ofono_sms_text_watch_remove(struct ofono_sms *sms,
					unsigned int id)
{
	if (!sms)
		return FALSE;

	DBG("%p", sms);

	return __ofono_watchlist_remove_item(sms->text_handlers, id);
}

unsigned int __ofono_sms_datagram_watch_add(struct ofono_sms *sms,
					ofono_sms_datagram_notify_cb_t cb,
					int dst, int src, void *data,
					ofono_destroy_func destroy)
{
	if (!sms)
		return 0;

	DBG("%p: dst %d, src %d", sms, dst, src);

	return add_sms_handler(sms->datagram_handlers, dst, src, cb, data,
				destroy);
}

gboolean __ofono_sms_datagram_watch_remove(struct ofono_sms *sms,
					unsigned int id)
{
	if (!sms)
		return FALSE;

	DBG("%p", sms);

	return __ofono_watchlist_remove_item(sms->datagram_handlers, id);
}

static DBusMessage *message_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct message *m = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_message_properties(m, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable message_methods[] = {
	{ "GetProperties",  "",    "a{sv}",   message_get_properties },
	{ }
};

static GDBusSignalTable message_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static struct message *message_create(const struct ofono_uuid *uuid)
{
	struct message *v;

	if (uuid == NULL)
		return NULL;

	v = g_try_new0(struct message, 1);
	if (v == NULL)
		return NULL;

	memcpy(&v->uuid, uuid, sizeof(*uuid));

	return v;
}

static void message_destroy(gpointer userdata)
{
	struct message *m = userdata;

	g_free(m);
}

static const char *message_build_path(struct ofono_sms *sms,
						struct message *m)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/message_%s",
			__ofono_atom_get_path(sms->atom),
			ofono_uuid_to_str(&m->uuid));

	return path;
}

static gboolean message_dbus_register(struct ofono_sms *sms, struct message *m)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;

	if (!m)
		return FALSE;

	path = message_build_path(sms, m);

	if (!g_dbus_register_interface(conn, path, OFONO_MESSAGE_INTERFACE,
					message_methods, message_signals,
					NULL, m, message_destroy)) {
		ofono_error("Could not register Message %s", path);
		message_destroy(m);

		return FALSE;
	}

	return TRUE;
}

static gboolean message_dbus_unregister(struct ofono_sms *sms,
						struct message *m)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = message_build_path(sms, m);

	return g_dbus_unregister_interface(conn, path,
						OFONO_MESSAGE_INTERFACE);
}

static void emit_message_added(struct ofono_sms *sms, struct message *m)
{
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *path;

	path = __ofono_atom_get_path(sms->atom);

	signal = dbus_message_new_signal(path,
					OFONO_MESSAGE_MANAGER_INTERFACE,
					"MessageAdded");

	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);

	path = message_build_path(sms, m);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_message_properties(m, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(ofono_dbus_get_connection(), signal);
}

static void emit_message_removed(struct ofono_sms *sms, struct message *m)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *atompath = __ofono_atom_get_path(sms->atom);
	const char *path = message_build_path(sms, m);

	g_dbus_emit_signal(conn, atompath, OFONO_MESSAGE_MANAGER_INTERFACE,
				"MessageRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
}

static void message_set_state(struct ofono_sms *sms,
					const struct ofono_uuid *uuid,
					enum message_state new_state)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *state;
	struct message *m;

	m = g_hash_table_lookup(sms->messages, uuid);

	if (m == NULL)
		return;

	if (m->state == new_state)
		return;

	m->state = new_state;
	path = message_build_path(sms, m);
	state = message_state_to_string(m->state);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_MESSAGE_INTERFACE,
						"State", DBUS_TYPE_STRING,
						&state);

	if (m->state == MESSAGE_STATE_SENT ||
			m->state == MESSAGE_STATE_FAILED) {
		g_hash_table_remove(sms->messages, uuid);
		emit_message_removed(sms, m);
		message_dbus_unregister(sms, m);
	}
}

static void set_bearer(struct ofono_sms *sms, int bearer)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sms->atom);
	const char *value;

	if (sms->bearer == bearer)
		return;

	sms->bearer = bearer;

	value = sms_bearer_to_string(sms->bearer);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_MESSAGE_MANAGER_INTERFACE,
						"Bearer",
						DBUS_TYPE_STRING, &value);
}

static void set_sca(struct ofono_sms *sms,
			const struct ofono_phone_number *sca)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sms->atom);
	const char *value;

	if (sms->sca.type == sca->type &&
			!strcmp(sms->sca.number, sca->number))
		return;

	sms->sca.type = sca->type;
	strncpy(sms->sca.number, sca->number, OFONO_MAX_PHONE_NUMBER_LENGTH);
	sms->sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';

	value = phone_number_to_string(&sms->sca);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_MESSAGE_MANAGER_INTERFACE,
						"ServiceCenterAddress",
						DBUS_TYPE_STRING, &value);
}

static DBusMessage *generate_get_properties_reply(struct ofono_sms *sms,
							DBusMessage *msg)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *sca;
	const char *bearer;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	sca = phone_number_to_string(&sms->sca);

	ofono_dbus_dict_append(&dict, "ServiceCenterAddress", DBUS_TYPE_STRING,
				&sca);

	ofono_dbus_dict_append(&dict, "UseDeliveryReports", DBUS_TYPE_BOOLEAN,
				&sms->use_delivery_reports);

	bearer = sms_bearer_to_string(sms->bearer);
	ofono_dbus_dict_append(&dict, "Bearer", DBUS_TYPE_STRING, &bearer);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void sms_sca_query_cb(const struct ofono_error *error,
				const struct ofono_phone_number *sca,
				void *data)
{
	struct ofono_sms *sms = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto out;

	set_sca(sms, sca);

	sms->flags |= MESSAGE_MANAGER_FLAG_CACHED;

out:
	if (sms->pending) {
		DBusMessage *reply = generate_get_properties_reply(sms,
								sms->pending);
		__ofono_dbus_pending_reply(&sms->pending, reply);
	}
}

static DBusMessage *sms_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_sms *sms = data;

	if (sms->pending)
		return __ofono_error_busy(msg);

	if (!sms->driver->sca_query)
		return __ofono_error_not_implemented(msg);

	if (sms->flags & MESSAGE_MANAGER_FLAG_CACHED)
		return generate_get_properties_reply(sms, msg);

	sms->pending = dbus_message_ref(msg);

	sms->driver->sca_query(sms, sms_sca_query_cb, sms);

	return NULL;
}

static void bearer_set_query_callback(const struct ofono_error *error,
					int bearer, void *data)
{
	struct ofono_sms *sms = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Set Bearer succeeded, but query failed");
		reply = __ofono_error_failed(sms->pending);
		__ofono_dbus_pending_reply(&sms->pending, reply);
		return;
	}

	reply = dbus_message_new_method_return(sms->pending);
	__ofono_dbus_pending_reply(&sms->pending, reply);

	set_bearer(sms, bearer);
}

static void bearer_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_sms *sms = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Setting Bearer failed");
		__ofono_dbus_pending_reply(&sms->pending,
					__ofono_error_failed(sms->pending));
		return;
	}

	sms->driver->bearer_query(sms, bearer_set_query_callback, sms);
}

static void sca_set_query_callback(const struct ofono_error *error,
					const struct ofono_phone_number *sca,
					void *data)
{
	struct ofono_sms *sms = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Set SCA succeeded, but query failed");
		sms->flags &= ~MESSAGE_MANAGER_FLAG_CACHED;
		reply = __ofono_error_failed(sms->pending);
		__ofono_dbus_pending_reply(&sms->pending, reply);
		return;
	}

	set_sca(sms, sca);

	reply = dbus_message_new_method_return(sms->pending);
	__ofono_dbus_pending_reply(&sms->pending, reply);
}

static void sca_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_sms *sms = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Setting SCA failed");
		__ofono_dbus_pending_reply(&sms->pending,
					__ofono_error_failed(sms->pending));
		return;
	}

	sms->driver->sca_query(sms, sca_set_query_callback, sms);
}

static DBusMessage *sms_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sms *sms = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (sms->pending)
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

	if (!strcmp(property, "ServiceCenterAddress")) {
		const char *value;
		struct ofono_phone_number sca;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (strlen(value) == 0 || !valid_phone_number_format(value))
			return __ofono_error_invalid_format(msg);

		if (sms->driver->sca_set == NULL ||
				sms->driver->sca_query == NULL)
			return __ofono_error_not_implemented(msg);

		string_to_phone_number(value, &sca);

		sms->pending = dbus_message_ref(msg);

		sms->driver->sca_set(sms, &sca, sca_set_callback, sms);
		return NULL;
	}

	if (!strcmp(property, "Bearer")) {
		const char *value;
		int bearer;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		bearer = sms_bearer_from_string(value);
		if (bearer < 0)
			return __ofono_error_invalid_format(msg);

		if (sms->driver->bearer_set == NULL ||
				sms->driver->bearer_query == NULL)
			return __ofono_error_not_implemented(msg);

		sms->pending = dbus_message_ref(msg);

		sms->driver->bearer_set(sms, bearer, bearer_set_callback, sms);
		return NULL;
	}

	if (!strcmp(property, "UseDeliveryReports")) {
		const char *path = __ofono_atom_get_path(sms->atom);
		dbus_bool_t value;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

		if (sms->use_delivery_reports != (ofono_bool_t) value) {
			sms->use_delivery_reports = value;
			ofono_dbus_signal_property_changed(conn, path,
						OFONO_MESSAGE_MANAGER_INTERFACE,
						"UseDeliveryReports",
						DBUS_TYPE_BOOLEAN, &value);
		}

		return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

/*
 * Destroy/release the contents of a 'struct tx_queue_entry'
 *
 * This releases resources allocated *inside* @entry and @entry
 * itself.
 */
static void tx_queue_entry_destroy(struct tx_queue_entry *entry)
{
	if (entry->destroy)
		entry->destroy(entry->data);

	g_free(entry->pdus);
	g_free(entry);
}

static void tx_queue_entry_destroy_foreach(gpointer _entry, gpointer unused)
{
	tx_queue_entry_destroy(_entry);
}

static void tx_finished(const struct ofono_error *error, int mr, void *data)
{
	struct ofono_sms *sms = data;
	struct ofono_modem *modem = __ofono_atom_get_modem(sms->atom);
	struct tx_queue_entry *entry = g_queue_peek_head(sms->txq);
	gboolean ok = error->type == OFONO_ERROR_TYPE_NO_ERROR;

	DBG("tx_finished");

	if (ok == FALSE) {
		if (!(entry->flags & OFONO_SMS_SUBMIT_FLAG_RETRY))
			goto next_q;

		entry->retry += 1;

		if (entry->retry < TXQ_MAX_RETRIES) {
			DBG("Sending failed, retry in %d secs",
					entry->retry * 5);
			sms->tx_source = g_timeout_add_seconds(entry->retry * 5,
								tx_next, sms);
			return;
		}

		DBG("Max retries reached, giving up");
		goto next_q;
	}

	entry->cur_pdu += 1;
	entry->retry = 0;

	if (entry->flags & OFONO_SMS_SUBMIT_FLAG_REQUEST_SR)
		status_report_assembly_add_fragment(sms->sr_assembly,
							entry->uuid.uuid,
							&entry->receiver,
							mr, time(NULL),
							entry->num_pdus);

	if (entry->cur_pdu < entry->num_pdus) {
		sms->tx_source = g_timeout_add(0, tx_next, sms);
		return;
	}

next_q:
	entry = g_queue_pop_head(sms->txq);

	if (entry->cb)
		entry->cb(ok, entry->data);

	if (entry->flags & OFONO_SMS_SUBMIT_FLAG_RECORD_HISTORY) {
		enum ofono_history_sms_status hs;
		enum message_state ms;

		if (ok) {
			hs = OFONO_HISTORY_SMS_STATUS_SUBMITTED;
			ms = MESSAGE_STATE_SENT;
		} else {
			hs = OFONO_HISTORY_SMS_STATUS_SUBMIT_FAILED;
			ms = MESSAGE_STATE_FAILED;
		}

		__ofono_history_sms_send_status(modem, &entry->uuid,
						time(NULL), hs);
		message_set_state(sms, &entry->uuid, ms);
	}

	tx_queue_entry_destroy(entry);

	if (g_queue_peek_head(sms->txq)) {
		DBG("Scheduling next");
		sms->tx_source = g_timeout_add(0, tx_next, sms);
	}
}

static gboolean tx_next(gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	int send_mms = 0;
	struct tx_queue_entry *entry = g_queue_peek_head(sms->txq);
	struct pending_pdu *pdu = &entry->pdus[entry->cur_pdu];
	struct ofono_error error;

	error.type = OFONO_ERROR_TYPE_NO_ERROR;

	DBG("tx_next: %p", entry);

	sms->tx_source = 0;

	if (!entry)
		return FALSE;

	if (g_queue_get_length(sms->txq) > 1
			|| (entry->num_pdus - entry->cur_pdu) > 1)
		send_mms = 1;

	sms->driver->submit(sms, pdu->pdu, pdu->pdu_len, pdu->tpdu_len,
				send_mms, tx_finished, sms);

	return FALSE;
}

static void set_ref_and_to(GSList *msg_list, guint16 ref, int offset,
				gboolean use_16bit, const char *to)
{
	GSList *l;
	struct sms *sms;

	for (l = msg_list; l; l = l->next) {
		sms = l->data;

		sms_address_from_string(&sms->submit.daddr, to);

		if (offset == 0)
			continue;

		if (use_16bit) {
			sms->submit.ud[offset] = (ref & 0xf0) >> 8;
			sms->submit.ud[offset+1] = ref & 0x0f;
		} else {
			sms->submit.ud[offset] = ref & 0x0f;
		}
	}
}

/**
 * Generate a UUID from an SMS PDU List
 *
 * @param pdu Pointer to array of PDUs data to generate the ID from
 * @param pdus Number of entries in the \e pdu array
 * @return 0 in error (no memory or serious code inconsistency in the
 *     input data structures), otherwise the SMS UUID.
 *
 * @internal
 *
 * The current time is added to avoid the UUID being the same when the
 * same message is sent to the same destination repeatedly. Note we
 * need a high resolution time (not just seconds), otherwise resending
 * in the same second (not that rare) could yield the same UUID.
 */
static gboolean sms_uuid_from_pdus(const struct pending_pdu *pdu,
					unsigned char pdus,
					struct ofono_uuid *uuid)

{
	GChecksum *checksum;
	gsize uuid_size = sizeof(uuid->uuid);
	unsigned int cnt;
	struct timeval now;

	checksum = g_checksum_new(G_CHECKSUM_SHA1);
	if (checksum == NULL)
		return FALSE;

	for (cnt = 0; cnt < pdus; cnt++)
		g_checksum_update(checksum, pdu[cnt].pdu, pdu[cnt].pdu_len);

	gettimeofday(&now, NULL);
	g_checksum_update(checksum, (void *) &now, sizeof(now));

	g_checksum_get_digest(checksum, uuid->uuid, &uuid_size);
	g_checksum_free(checksum);

	return TRUE;
}

static struct tx_queue_entry *tx_queue_entry_new(GSList *msg_list,
						unsigned int flags,
						ofono_sms_txq_submit_cb_t cb,
						void *data,
						ofono_destroy_func destroy)
{
	struct tx_queue_entry *entry;
	int i = 0;
	GSList *l;

	entry = g_try_new0(struct tx_queue_entry, 1);
	if (entry == NULL)
		return NULL;

	entry->num_pdus = g_slist_length(msg_list);

	entry->pdus = g_try_new0(struct pending_pdu, entry->num_pdus);
	if (entry->pdus == NULL)
		goto error;

	if (flags & OFONO_SMS_SUBMIT_FLAG_REQUEST_SR) {
		struct sms *head = msg_list->data;

		memcpy(&entry->receiver, &head->submit.daddr,
				sizeof(entry->receiver));
	}

	entry->flags = flags;
	entry->cb = cb;
	entry->data = data;
	entry->destroy = destroy;

	for (l = msg_list; l; l = l->next) {
		struct pending_pdu *pdu = &entry->pdus[i++];
		struct sms *s = l->data;

		sms_encode(s, &pdu->pdu_len, &pdu->tpdu_len, pdu->pdu);

		DBG("pdu_len: %d, tpdu_len: %d",
				pdu->pdu_len, pdu->tpdu_len);
	}

	if (sms_uuid_from_pdus(entry->pdus, entry->num_pdus, &entry->uuid))
		return entry;

error:
	g_free(entry->pdus);
	g_free(entry);

	return NULL;
}

/*
 * Pre-process a SMS text message and deliver it [D-Bus SendMessage()]
 *
 * @conn: D-Bus connection
 * @msg: message data (telephone number and text)
 * @data: SMS object to use for transmision
 *
 * An alphabet is chosen for the text and it (might be) segmented in
 * fragments by sms_text_prepare() into @msg_list. A queue list @entry
 * is created by tx_queue_entry_new() and g_queue_push_tail()
 * appends that entry to the SMS transmit queue. Then the tx_next()
 * function is scheduled to run to process the queue.
 */
static DBusMessage *sms_send_message(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sms *sms = data;
	const char *to;
	const char *text;
	GSList *msg_list;
	int ref_offset;
	struct ofono_modem *modem;
	unsigned int flags;
	gboolean use_16bit_ref = FALSE;
	struct tx_queue_entry *entry;
	struct message *m;
	const char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &to,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (valid_phone_number_format(to) == FALSE)
		return __ofono_error_invalid_format(msg);

	msg_list = sms_text_prepare(text, 0, use_16bit_ref, &ref_offset,
					sms->use_delivery_reports);

	if (!msg_list)
		return __ofono_error_invalid_format(msg);

	set_ref_and_to(msg_list, sms->ref, ref_offset, use_16bit_ref, to);
	DBG("ref: %d, offset: %d", sms->ref, ref_offset);

	if (ref_offset != 0) {
		if (sms->ref == 65536)
			sms->ref = 1;
		else
			sms->ref = sms->ref + 1;
	}

	flags = OFONO_SMS_SUBMIT_FLAG_RECORD_HISTORY;
	flags |= OFONO_SMS_SUBMIT_FLAG_RETRY;
	if (sms->use_delivery_reports)
		flags |= OFONO_SMS_SUBMIT_FLAG_REQUEST_SR;

	entry = tx_queue_entry_new(msg_list, flags, NULL, NULL, NULL);

	g_slist_foreach(msg_list, (GFunc)g_free, NULL);
	g_slist_free(msg_list);

	if (entry == NULL)
		goto err;

	m = message_create(&entry->uuid);
	if (m == NULL)
		goto err;

	if (message_dbus_register(sms, m) == FALSE)
		goto err;

	g_hash_table_insert(sms->messages, &m->uuid, m);

	g_queue_push_tail(sms->txq, entry);

	if (g_queue_get_length(sms->txq) == 1)
		sms->tx_source = g_timeout_add(0, tx_next, sms);

	path = message_build_path(sms, m);
	g_dbus_send_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);

	modem = __ofono_atom_get_modem(sms->atom);
	__ofono_history_sms_send_pending(modem, &entry->uuid,
						to, time(NULL), text);

	emit_message_added(sms, m);

	return NULL;

err:
	return __ofono_error_failed(msg);
}

static DBusMessage *sms_get_messages(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sms *sms = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;
	DBusMessageIter entry, dict;
	const char *path;
	GHashTableIter hashiter;
	gpointer key, value;
	struct message *m;

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

	g_hash_table_iter_init(&hashiter, sms->messages);

	while (g_hash_table_iter_next(&hashiter, &key, &value)) {
		m = value;

		path = message_build_path(sms, m);

		dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT,
							NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
						&path);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

		append_message_properties(m, &dict);
		dbus_message_iter_close_container(&entry, &dict);
		dbus_message_iter_close_container(&array, &entry);
	}

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static GDBusMethodTable sms_manager_methods[] = {
	{ "GetProperties",    "",    "a{sv}",        sms_get_properties,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "SetProperty",      "sv",  "",             sms_set_property,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "SendMessage",      "ss",  "o",             sms_send_message,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetMessages",       "",    "a(oa{sv})",    sms_get_messages },
	{ }
};

static GDBusSignalTable sms_manager_signals[] = {
	{ "PropertyChanged",	"sv"		},
	{ "IncomingMessage",	"sa{sv}"	},
	{ "ImmediateMessage",	"sa{sv}"	},
	{ "MessageAdded",	"oa{sv}"	},
	{ "MessageRemoved",	"o"		},
	{ }
};

static gboolean compute_incoming_msgid(GSList *sms_list,
						struct ofono_uuid *uuid)
{
	GChecksum *checksum;
	GSList *l;
	const struct sms *s;
	unsigned char buf[176];
	gsize uuid_size = sizeof(uuid->uuid);
	int len;

	checksum = g_checksum_new(G_CHECKSUM_SHA1);
	if (checksum == NULL)
		return FALSE;

	for (l = sms_list; l; l = l->next) {
		s = l->data;

		sms_encode(s, &len, NULL, buf);
		g_checksum_update(checksum, buf, len);
	}

	g_checksum_get_digest(checksum, uuid->uuid, &uuid_size);
	g_checksum_free(checksum);

	return TRUE;
}

static void dispatch_app_datagram(struct ofono_sms *sms,
					const struct ofono_uuid *uuid,
					int dst, int src,
					unsigned char *buf, unsigned len,
					const struct sms_address *addr,
					const struct sms_scts *scts)
{
	const char *sender = sms_address_to_string(addr);
	time_t ts;
	struct tm remote;
	struct tm local;

	ofono_sms_datagram_notify_cb_t notify;
	struct sms_handler *h;
	GSList *l;

	ts = sms_scts_to_time(scts, &remote);
	localtime_r(&ts, &local);

	for (l = sms->datagram_handlers->items; l; l = l->next) {
		h = l->data;
		notify = h->item.notify;

		if (!port_equal(dst, h->dst) || !port_equal(src, h->src))
			continue;

		notify(sender, &remote, &local, dst, src, buf, len,
			h->item.notify_data);
	}
}

static void dispatch_text_message(struct ofono_sms *sms,
					const struct ofono_uuid *uuid,
					const char *message,
					enum sms_class cls,
					const struct sms_address *addr,
					const struct sms_scts *scts)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(sms->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sms->atom);
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char buf[128];
	const char *signal_name;
	time_t ts;
	struct tm remote;
	struct tm local;
	const char *str = buf;
	ofono_sms_text_notify_cb_t notify;
	struct sms_handler *h;
	GSList *l;

	if (!message)
		return;

	if (cls == SMS_CLASS_0)
		signal_name = "ImmediateMessage";
	else
		signal_name = "IncomingMessage";

	signal = dbus_message_new_signal(path, OFONO_MESSAGE_MANAGER_INTERFACE,
						signal_name);

	if (!signal)
		return;

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &message);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	ts = sms_scts_to_time(scts, &remote);
	localtime_r(&ts, &local);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", &local);
	buf[127] = '\0';
	ofono_dbus_dict_append(&dict, "LocalSentTime", DBUS_TYPE_STRING, &str);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", &remote);
	buf[127] = '\0';
	ofono_dbus_dict_append(&dict, "SentTime", DBUS_TYPE_STRING, &str);

	str = sms_address_to_string(addr);
	ofono_dbus_dict_append(&dict, "Sender", DBUS_TYPE_STRING, &str);

	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(conn, signal);

	if (cls == SMS_CLASS_0)
		return;

	for (l = sms->text_handlers->items; l; l = l->next) {
		h = l->data;
		notify = h->item.notify;

		notify(str, &remote, &local, message, h->item.notify_data);
	}

	__ofono_history_sms_received(modem, uuid, str, &remote, &local,
					message);
}

static void sms_dispatch(struct ofono_sms *sms, GSList *sms_list)
{
	GSList *l;
	const struct sms *s;
	struct ofono_uuid uuid;
	enum sms_charset uninitialized_var(old_charset);
	enum sms_class cls;
	int srcport = -1;
	int dstport = -1;

	DBG("");

	if (sms_list == NULL)
		return;

	/*
	 * Qutoting 23.040: The TP elements in the SMS‑SUBMIT PDU, apart from
	 * TP‑MR, TP-SRR, TP‑UDL and TP‑UD, should remain unchanged for each
	 * SM which forms part of a concatenated SM, otherwise this may lead
	 * to irrational behaviour
	 *
	 * This means that we assume that at least the charset is the same
	 * across all parts of the SMS in the case of 8-bit data.  Other
	 * cases can be handled by converting to UTF8.
	 *
	 * We also check that if 8-bit or 16-bit application addressing is
	 * used, the addresses are the same across all segments.
	 */

	for (l = sms_list; l; l = l->next) {
		guint8 dcs;
		gboolean comp = FALSE;
		enum sms_charset charset;
		int cdst = -1;
		int csrc = -1;
		gboolean is_8bit;

		s = l->data;
		dcs = s->deliver.dcs;

		if (sms_mwi_dcs_decode(dcs, NULL, &charset, NULL, NULL))
			cls = SMS_CLASS_UNSPECIFIED;
		else if (!sms_dcs_decode(dcs, &cls, &charset, &comp, NULL)) {
			ofono_error("The deliver DCS is not recognized");
			return;
		}

		if (comp) {
			ofono_error("Compressed data not supported");
			return;
		}

		if (l == sms_list)
			old_charset = charset;

		if (charset == SMS_CHARSET_8BIT && charset != old_charset) {
			ofono_error("Can't concatenate disparate charsets");
			return;
		}

		if (sms_extract_app_port(s, &cdst, &csrc, &is_8bit)) {
			csrc = is_8bit ? (csrc << 8) : csrc;
			cdst = is_8bit ? (cdst << 8) : cdst;

			if (l == sms_list) {
				srcport = csrc;
				dstport = cdst;
			}
		}

		if (srcport != csrc || dstport != cdst) {
			ofono_error("Source / Destination ports across "
					"concatenated message are not the "
					"same, ignoring");
			return;
		}
	}

	if (!compute_incoming_msgid(sms_list, &uuid))
		return;

	s = sms_list->data;

	/* Handle datagram */
	if (old_charset == SMS_CHARSET_8BIT) {
		unsigned char *buf;
		long len;

		if (srcport == -1 || dstport == -1) {
			ofono_error("Got an 8-bit encoded message, however "
					"no valid src/address port, ignore");
			return;
		}

		buf = sms_decode_datagram(sms_list, &len);

		if (!buf)
			return;

		dispatch_app_datagram(sms, &uuid, dstport, srcport, buf, len,
					&s->deliver.oaddr, &s->deliver.scts);

		g_free(buf);
	} else {
		char *message = sms_decode_text(sms_list);

		if (!message)
			return;

		dispatch_text_message(sms, &uuid, message, cls,
					&s->deliver.oaddr, &s->deliver.scts);

		g_free(message);
	}
}

static void handle_deliver(struct ofono_sms *sms, const struct sms *incoming)
{
	GSList *l;
	guint16 ref;
	guint8 max;
	guint8 seq;

	DBG("");

	if (sms_extract_concatenation(incoming, &ref, &max, &seq)) {
		GSList *sms_list;

		if (!sms->assembly)
			return;

		sms_list = sms_assembly_add_fragment(sms->assembly,
							incoming, time(NULL),
							&incoming->deliver.oaddr,
							ref, max, seq);

		if (!sms_list)
			return;

		sms_dispatch(sms, sms_list);
		g_slist_foreach(sms_list, (GFunc)g_free, NULL);
		g_slist_free(sms_list);

		return;
	}

	l = g_slist_append(NULL, (void *)incoming);
	sms_dispatch(sms, l);
	g_slist_free(l);
}

static void handle_sms_status_report(struct ofono_sms *sms,
						const struct sms *incoming)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(sms->atom);
	gboolean delivered;
	struct ofono_uuid uuid;

	DBG("");

	if (status_report_assembly_report(sms->sr_assembly, incoming, uuid.uuid,
						&delivered) == FALSE)
		return;

	__ofono_history_sms_send_status(modem, &uuid, time(NULL),
			delivered ? OFONO_HISTORY_SMS_STATUS_DELIVERED :
			OFONO_HISTORY_SMS_STATUS_DELIVER_FAILED);
}


static inline gboolean handle_mwi(struct ofono_sms *sms, struct sms *s)
{
	gboolean discard;

	DBG("");

	if (sms->mw == NULL)
		return FALSE;

	__ofono_message_waiting_mwi(sms->mw, s, &discard);

	return discard;
}

void ofono_sms_deliver_notify(struct ofono_sms *sms, unsigned char *pdu,
				int len, int tpdu_len)
{
	struct sms s;
	enum sms_class cls;

	DBG("len %d tpdu len %d", len, tpdu_len);

	if (!sms_decode(pdu, len, FALSE, tpdu_len, &s)) {
		ofono_error("Unable to decode PDU");
		return;
	}

	if (s.type != SMS_TYPE_DELIVER) {
		ofono_error("Expecting a DELIVER pdu");
		return;
	}

	if (s.deliver.pid == SMS_PID_TYPE_SM_TYPE_0) {
		DBG("Explicitly ignoring type 0 SMS");
		return;
	}

	/*
	 * This is an older style MWI notification, process MWI
	 * headers and handle it like any other message
	 */
	if (s.deliver.pid == SMS_PID_TYPE_RETURN_CALL) {
		if (handle_mwi(sms, &s))
			return;

		goto out;
	}

	/*
	 * The DCS indicates this is an MWI notification, process it
	 * and then handle the User-Data as any other message
	 */
	if (sms_mwi_dcs_decode(s.deliver.dcs, NULL, NULL, NULL, NULL)) {
		if (handle_mwi(sms, &s))
			return;

		goto out;
	}

	if (!sms_dcs_decode(s.deliver.dcs, &cls, NULL, NULL, NULL)) {
		ofono_error("Unknown / Reserved DCS.  Ignoring");
		return;
	}

	switch (s.deliver.pid) {
	case SMS_PID_TYPE_ME_DOWNLOAD:
		if (cls == SMS_CLASS_1) {
			ofono_error("ME Download message ignored");
			return;
		}

		break;
	case SMS_PID_TYPE_ME_DEPERSONALIZATION:
		if (s.deliver.dcs == 0x11) {
			ofono_error("ME Depersonalization message ignored");
			return;
		}

		break;
	case SMS_PID_TYPE_USIM_DOWNLOAD:
	case SMS_PID_TYPE_ANSI136:
		if (cls == SMS_CLASS_2) {
			ofono_error("(U)SIM Download messages not supported");
			return;
		}

		/* Otherwise handle in a "normal" way */
		break;
	default:
		break;
	}

	/*
	 * Check to see if the SMS has any other MWI related headers,
	 * as sometimes they are "tacked on" by the SMSC.
	 * While we're doing this we also check for messages containing
	 * WCMP headers or headers that can't possibly be in a normal
	 * message.  If we find messages like that, we ignore them.
	 */
	if (s.deliver.udhi) {
		struct sms_udh_iter iter;
		enum sms_iei iei;

		if (!sms_udh_iter_init(&s, &iter))
			goto out;

		while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
				SMS_IEI_INVALID) {
			if (iei > 0x25) {
				ofono_error("Reserved / Unknown / USAT"
						"header in use, ignore");
				return;
			}

			switch (iei) {
			case SMS_IEI_SPECIAL_MESSAGE_INDICATION:
			case SMS_IEI_ENHANCED_VOICE_MAIL_INFORMATION:
				/*
				 * TODO: ignore if not in the very first
				 * segment of a concatenated SM so as not
				 * to repeat the indication.
				 */
				if (handle_mwi(sms, &s))
					return;

				goto out;
			case SMS_IEI_WCMP:
				ofono_error("No support for WCMP, ignoring");
				return;
			default:
				sms_udh_iter_next(&iter);
			}
		}
	}

out:
	handle_deliver(sms, &s);
}

void ofono_sms_status_notify(struct ofono_sms *sms, unsigned char *pdu,
				int len, int tpdu_len)
{
	struct sms s;
	enum sms_class cls;

	DBG("len %d tpdu len %d", len, tpdu_len);

	if (!sms_decode(pdu, len, FALSE, tpdu_len, &s)) {
		ofono_error("Unable to decode PDU");
		return;
	}

	if (s.type != SMS_TYPE_STATUS_REPORT) {
		ofono_error("Expecting a STATUS REPORT pdu");
		return;
	}

	if (s.status_report.srq) {
		ofono_error("Waiting an answer to SMS-SUBMIT, not SMS-COMMAND");
		return;
	}

	if (!sms_dcs_decode(s.deliver.dcs, &cls, NULL, NULL, NULL)) {
		ofono_error("Unknown / Reserved DCS.  Ignoring");
		return;
	}

	handle_sms_status_report(sms, &s);
}

int ofono_sms_driver_register(const struct ofono_sms_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_sms_driver_unregister(const struct ofono_sms_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void sms_unregister(struct ofono_atom *atom)
{
	struct ofono_sms *sms = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	g_dbus_unregister_interface(conn, path,
					OFONO_MESSAGE_MANAGER_INTERFACE);
	ofono_modem_remove_interface(modem, OFONO_MESSAGE_MANAGER_INTERFACE);

	if (sms->mw_watch) {
		__ofono_modem_remove_atom_watch(modem, sms->mw_watch);
		sms->mw_watch = 0;
		sms->mw = NULL;
	}

	if (sms->messages) {
		GHashTableIter iter;
		struct message *m;
		gpointer key, value;

		g_hash_table_iter_init(&iter, sms->messages);

		while (g_hash_table_iter_next(&iter, &key, &value)) {
			m = value;
			message_dbus_unregister(sms, m);
		}

		g_hash_table_destroy(sms->messages);
		sms->messages = NULL;
	}

	__ofono_watchlist_free(sms->text_handlers);
	sms->text_handlers = NULL;

	__ofono_watchlist_free(sms->datagram_handlers);
	sms->datagram_handlers = NULL;
}

static void sms_remove(struct ofono_atom *atom)
{
	struct ofono_sms *sms = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (sms == NULL)
		return;

	if (sms->driver && sms->driver->remove)
		sms->driver->remove(sms);

	if (sms->tx_source) {
		g_source_remove(sms->tx_source);
		sms->tx_source = 0;
	}

	if (sms->assembly) {
		sms_assembly_free(sms->assembly);
		sms->assembly = NULL;
	}

	if (sms->txq) {
		g_queue_foreach(sms->txq, tx_queue_entry_destroy_foreach, NULL);
		g_queue_free(sms->txq);
		sms->txq = NULL;
	}

	if (sms->settings) {
		g_key_file_set_integer(sms->settings, SETTINGS_GROUP,
					"NextReference", sms->ref);
		g_key_file_set_boolean(sms->settings, SETTINGS_GROUP,
					"UseDeliveryReports",
					sms->use_delivery_reports);
		g_key_file_set_integer(sms->settings, SETTINGS_GROUP,
					"Bearer", sms->bearer);

		storage_close(sms->imsi, SETTINGS_STORE, sms->settings, TRUE);

		g_free(sms->imsi);
		sms->imsi = NULL;
		sms->settings = NULL;
	}

	if (sms->sr_assembly) {
		status_report_assembly_free(sms->sr_assembly);
		sms->sr_assembly = NULL;
	}

	g_free(sms);
}


/*
 * Create a SMS driver
 *
 * This creates a SMS driver that is hung off a @modem
 * object. However, for the driver to be used by the system, it has to
 * be registered with the oFono core using ofono_sms_register().
 *
 * This is done once the modem driver determines that SMS is properly
 * supported by the hardware.
 */
struct ofono_sms *ofono_sms_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_sms *sms;
	GSList *l;

	if (driver == NULL)
		return NULL;

	sms = g_try_new0(struct ofono_sms, 1);

	if (sms == NULL)
		return NULL;

	sms->sca.type = 129;
	sms->ref = 1;
	sms->txq = g_queue_new();
	sms->messages = g_hash_table_new(uuid_hash, uuid_equal);

	sms->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_SMS,
						sms_remove, sms);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_sms_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(sms, vendor, data) < 0)
			continue;

		sms->driver = drv;
		break;
	}

	return sms;
}

static void mw_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *data)
{
	struct ofono_sms *sms = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		sms->mw = NULL;
		return;
	}

	sms->mw = __ofono_atom_get_data(atom);
}

static void sms_load_settings(struct ofono_sms *sms, const char *imsi)
{
	GError *error = NULL;

	sms->settings = storage_open(imsi, SETTINGS_STORE);

	if (sms->settings == NULL)
		return;

	sms->imsi = g_strdup(imsi);

	sms->ref = g_key_file_get_integer(sms->settings, SETTINGS_GROUP,
							"NextReference", NULL);
	if (sms->ref >= 65536)
		sms->ref = 1;

	sms->use_delivery_reports =
		g_key_file_get_boolean(sms->settings, SETTINGS_GROUP,
					"UseDeliveryReports", NULL);

	sms->bearer = g_key_file_get_integer(sms->settings, SETTINGS_GROUP,
							"Bearer", &error);
	if (error)
		sms->bearer = 3; /* Default to CS then PS */
}

static void bearer_init_callback(const struct ofono_error *error, void *data)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_error("Error bootstrapping SMS Bearer Preference");
}

/*
 * Indicate oFono that a SMS driver is ready for operation
 *
 * This is called after ofono_sms_create() was done and the modem
 * driver determined that a modem supports SMS correctly. Once this
 * call succeeds, the D-BUS interface for SMS goes live.
 */
void ofono_sms_register(struct ofono_sms *sms)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(sms->atom);
	const char *path = __ofono_atom_get_path(sms->atom);
	struct ofono_atom *mw_atom;
	struct ofono_atom *sim_atom;

	if (!g_dbus_register_interface(conn, path,
					OFONO_MESSAGE_MANAGER_INTERFACE,
					sms_manager_methods,
					sms_manager_signals,
					NULL, sms, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_MESSAGE_MANAGER_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_MESSAGE_MANAGER_INTERFACE);

	sms->mw_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_MESSAGE_WAITING,
					mw_watch, sms, NULL);

	mw_atom = __ofono_modem_find_atom(modem,
					OFONO_ATOM_TYPE_MESSAGE_WAITING);

	if (mw_atom && __ofono_atom_get_registered(mw_atom))
		mw_watch(mw_atom, OFONO_ATOM_WATCH_CONDITION_REGISTERED, sms);

	sim_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SIM);

	/*
	 * If we have a sim atom, we can uniquely identify the SIM,
	 * otherwise create an sms assembly which doesn't backup the fragment
	 * store.
	 */
	if (sim_atom) {
		const char *imsi;

		sms->sim = __ofono_atom_get_data(sim_atom);
		imsi = ofono_sim_get_imsi(sms->sim);
		sms->assembly = sms_assembly_new(imsi);

		sms->sr_assembly = status_report_assembly_new(imsi);

		sms_load_settings(sms, imsi);
	} else {
		sms->assembly = sms_assembly_new(NULL);
		sms->sr_assembly = status_report_assembly_new(NULL);
		sms->bearer = 3; /* Default to CS then PS */
	}

	if (sms->driver->bearer_set)
		sms->driver->bearer_set(sms, sms->bearer,
						bearer_init_callback, sms);

	sms->text_handlers = __ofono_watchlist_new(g_free);
	sms->datagram_handlers = __ofono_watchlist_new(g_free);

	__ofono_atom_register(sms->atom, sms_unregister);
}

void ofono_sms_remove(struct ofono_sms *sms)
{
	__ofono_atom_free(sms->atom);
}

void ofono_sms_set_data(struct ofono_sms *sms, void *data)
{
	sms->driver_data = data;
}

void *ofono_sms_get_data(struct ofono_sms *sms)
{
	return sms->driver_data;
}

int __ofono_sms_txq_submit(struct ofono_sms *sms, GSList *list,
				unsigned int flags,
				struct ofono_uuid *uuid,
				ofono_sms_txq_submit_cb_t cb,
				void *data, ofono_destroy_func destroy)
{
	struct tx_queue_entry *entry;

	entry = tx_queue_entry_new(list, flags, cb, data, destroy);
	if (entry == NULL)
		return -ENOMEM;

	g_queue_push_tail(sms->txq, entry);

	if (g_queue_get_length(sms->txq) == 1)
		sms->tx_source = g_timeout_add(0, tx_next, sms);

	if (uuid)
		memcpy(uuid, &entry->uuid, sizeof(*uuid));

	if (flags & OFONO_SMS_SUBMIT_FLAG_RECORD_HISTORY) {
		struct message *m;

		m = message_create(&entry->uuid);
		if (m == NULL)
			goto out;

		if (message_dbus_register(sms, m) == FALSE)
			goto out;

		g_hash_table_insert(sms->messages, &m->uuid, m);
		emit_message_added(sms, m);
	}

out:
	return 0;
}
