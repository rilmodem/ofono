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

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <glib.h>
#include <gdbus.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "ofono.h"

#include "common.h"
#include "smsutil.h"
#include "stkutil.h"
#include "stkagent.h"

static GSList *g_drivers = NULL;

struct stk_timer {
	time_t expiry;
	time_t start;
};

struct ofono_stk {
	const struct ofono_stk_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	struct stk_command *pending_cmd;
	void (*cancel_cmd)(struct ofono_stk *stk);
	GQueue *envelope_q;
	DBusMessage *pending;

	struct stk_timer timers[8];
	guint timers_source;

	int timeout;
	int short_timeout;
	struct stk_agent *session_agent;
	struct stk_agent *default_agent;
	struct stk_agent *current_agent; /* Always equals one of the above */
	struct stk_menu *main_menu, *select_item_menu;
	gboolean respond_on_exit;
	ofono_bool_t immediate_response;
	guint remove_agent_source;
	struct sms_submit_req *sms_submit_req;
	char *idle_mode_text;
	struct timeval get_inkey_start_ts;
};

struct envelope_op {
	uint8_t tlv[256];
	unsigned int tlv_len;
	int retries;
	void (*cb)(struct ofono_stk *stk, gboolean ok,
			const unsigned char *data, int length);
};

struct sms_submit_req {
	struct ofono_stk *stk;
	gboolean cancelled;
};

#define ENVELOPE_RETRIES_DEFAULT 5

static void envelope_queue_run(struct ofono_stk *stk);
static void timers_update(struct ofono_stk *stk);

static int stk_respond(struct ofono_stk *stk, struct stk_response *rsp,
			ofono_stk_generic_cb_t cb)
{
	const guint8 *tlv;
	unsigned int tlv_len;

	DBG("");

	if (stk->driver->terminal_response == NULL)
		return -ENOSYS;

	rsp->src = STK_DEVICE_IDENTITY_TYPE_TERMINAL;
	rsp->dst = STK_DEVICE_IDENTITY_TYPE_UICC;
	rsp->number = stk->pending_cmd->number;
	rsp->type = stk->pending_cmd->type;
	rsp->qualifier = stk->pending_cmd->qualifier;

	tlv = stk_pdu_from_response(rsp, &tlv_len);
	if (!tlv)
		return -EINVAL;

	stk_command_free(stk->pending_cmd);
	stk->pending_cmd = NULL;
	stk->cancel_cmd = NULL;

	stk->driver->terminal_response(stk, tlv_len, tlv, cb, stk);

	return 0;
}

static void stk_command_cb(const struct ofono_error *error, void *data)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("TERMINAL RESPONSE to a UICC command failed");
		return;
	}

	DBG("TERMINAL RESPONSE to a command reported no errors");
}

static void send_simple_response(struct ofono_stk *stk,
					enum stk_result_type result)
{
	struct stk_response rsp;
	static struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };

	DBG("result %d", result);

	memset(&rsp, 0, sizeof(rsp));
	rsp.result.type = result;

	if (stk_respond(stk, &rsp, stk_command_cb))
		stk_command_cb(&error, stk);
}

static void envelope_cb(const struct ofono_error *error, const uint8_t *data,
			int length, void *user_data)
{
	struct ofono_stk *stk = user_data;
	struct envelope_op *op = g_queue_peek_head(stk->envelope_q);
	gboolean result = TRUE;

	DBG("length %d", length);

	if (op->retries > 0 && error->type == OFONO_ERROR_TYPE_SIM &&
			error->error == 0x9300) {
		op->retries--;
		goto out;
	}

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		result = FALSE;

	g_queue_pop_head(stk->envelope_q);

	if (op->cb)
		op->cb(stk, result, data, length);

	g_free(op);

out:
	envelope_queue_run(stk);
}

static void envelope_queue_run(struct ofono_stk *stk)
{
	if (g_queue_get_length(stk->envelope_q) > 0) {
		struct envelope_op *op = g_queue_peek_head(stk->envelope_q);

		stk->driver->envelope(stk, op->tlv_len, op->tlv,
					envelope_cb, stk);
	}
}

static int stk_send_envelope(struct ofono_stk *stk, struct stk_envelope *e,
				void (*cb)(struct ofono_stk *stk, gboolean ok,
						const uint8_t *data,
						int length), int retries)
{
	const uint8_t *tlv;
	unsigned int tlv_len;
	struct envelope_op *op;

	DBG("");

	if (stk->driver->envelope == NULL)
		return -ENOSYS;

	e->dst = STK_DEVICE_IDENTITY_TYPE_UICC;
	tlv = stk_pdu_from_envelope(e, &tlv_len);
	if (!tlv)
		return -EINVAL;

	op = g_new0(struct envelope_op, 1);

	op->cb = cb;
	op->retries = retries;
	memcpy(op->tlv, tlv, tlv_len);
	op->tlv_len = tlv_len;

	g_queue_push_tail(stk->envelope_q, op);

	if (g_queue_get_length(stk->envelope_q) == 1)
		envelope_queue_run(stk);

	return 0;
}

static void stk_cbs_download_cb(struct ofono_stk *stk, gboolean ok,
				const unsigned char *data, int len)
{
	if (!ok) {
		ofono_error("CellBroadcast download to UICC failed");
		return;
	}

	if (len)
		ofono_error("CellBroadcast download returned %i bytes of data",
				len);

	DBG("CellBroadcast download to UICC reported no error");
}

void __ofono_cbs_sim_download(struct ofono_stk *stk, const struct cbs *msg)
{
	struct stk_envelope e;
	int err;

	DBG("");

	memset(&e, 0, sizeof(e));

	e.type = STK_ENVELOPE_TYPE_CBS_PP_DOWNLOAD;
	e.src = STK_DEVICE_IDENTITY_TYPE_NETWORK;
	memcpy(&e.cbs_pp_download.page, msg, sizeof(msg));

	err = stk_send_envelope(stk, &e, stk_cbs_download_cb,
				ENVELOPE_RETRIES_DEFAULT);
	if (err)
		stk_cbs_download_cb(stk, FALSE, NULL, -1);
}

static struct stk_menu *stk_menu_create(const char *title,
		const struct stk_text_attribute *title_attr, GSList *items,
		const struct stk_item_text_attribute_list *item_attrs,
		int default_id, gboolean soft_key, gboolean has_help)
{
	struct stk_menu *ret = g_new(struct stk_menu, 1);
	GSList *l;
	int i;

	DBG("");

	ret->title = g_strdup(title ? title : "");
	ret->icon_id = 0;
	ret->items = g_new0(struct stk_menu_item, g_slist_length(items) + 1);
	ret->default_item = -1;
	ret->soft_key = soft_key;
	ret->has_help = has_help;

	for (l = items, i = 0; l; l = l->next, i++) {
		struct stk_item *item = l->data;

		ret->items[i].text = g_strdup(item->text);
		ret->items[i].item_id = item->id;

		if (item->id == default_id)
			ret->default_item = i;
	}

	return ret;
}

static struct stk_menu *stk_menu_create_from_set_up_menu(
						const struct stk_command *cmd)
{
	gboolean soft_key = (cmd->qualifier & (1 << 0)) != 0;
	gboolean has_help = (cmd->qualifier & (1 << 7)) != 0;

	return stk_menu_create(cmd->setup_menu.alpha_id,
				&cmd->setup_menu.text_attr,
				cmd->setup_menu.items,
				&cmd->setup_menu.item_text_attr_list,
				0, soft_key, has_help);
}

static struct stk_menu *stk_menu_create_from_select_item(
						const struct stk_command *cmd)
{
	gboolean soft_key = (cmd->qualifier & (1 << 2)) != 0;
	gboolean has_help = (cmd->qualifier & (1 << 7)) != 0;

	return stk_menu_create(cmd->select_item.alpha_id,
				&cmd->select_item.text_attr,
				cmd->select_item.items,
				&cmd->select_item.item_text_attr_list,
				cmd->select_item.item_id, soft_key, has_help);
}

static void stk_menu_free(struct stk_menu *menu)
{
	struct stk_menu_item *i;

	for (i = menu->items; i->text; i++)
		g_free(i->text);

	g_free(menu->items);
	g_free(menu->title);
	g_free(menu);
}

static void emit_menu_changed(struct ofono_stk *stk)
{
	static struct stk_menu_item end_item = {};
	static struct stk_menu no_menu = {
		.title = "",
		.items = &end_item,
		.has_help = FALSE,
		.default_item = -1,
	};
	static char *name = "MainMenu";
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(stk->atom);
	struct stk_menu *menu = stk->main_menu ? stk->main_menu : &no_menu;
	DBusMessage *signal;
	DBusMessageIter iter;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_STK_INTERFACE,
						"MainMenuTitle",
						DBUS_TYPE_STRING, &menu->title);

	signal = dbus_message_new_signal(path, OFONO_STK_INTERFACE,
						"PropertyChanged");
	if (!signal) {
		ofono_error("Unable to allocate new %s.PropertyChanged signal",
				OFONO_SIM_APP_INTERFACE);

		return;
	}

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

	append_menu_items_variant(&iter, menu->items);

	g_dbus_send_message(conn, signal);
}

static void dict_append_menu(DBusMessageIter *dict, struct stk_menu *menu)
{
	DBusMessageIter entry;
	const char *key = "MainMenu";

	ofono_dbus_dict_append(dict, "MainMenuTitle",
				DBUS_TYPE_STRING, &menu->title);

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	append_menu_items_variant(&entry, menu->items);

	dbus_message_iter_close_container(dict, &entry);
}

static void stk_alpha_id_set(struct ofono_stk *stk, const char *text)
{
	/* TODO */
}

static void stk_alpha_id_unset(struct ofono_stk *stk)
{
	/* TODO */
}

static DBusMessage *stk_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_stk *stk = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *idle_mode_text;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	idle_mode_text = stk->idle_mode_text ? stk->idle_mode_text : "";
	ofono_dbus_dict_append(&dict, "IdleModeText",
				DBUS_TYPE_STRING, &idle_mode_text);

	if (stk->main_menu)
		dict_append_menu(&dict, stk->main_menu);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void stk_request_cancel(struct ofono_stk *stk)
{
	if (stk->session_agent)
		stk_agent_request_cancel(stk->session_agent);

	if (stk->default_agent)
		stk_agent_request_cancel(stk->default_agent);
}

static void default_agent_notify(gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	if (stk->current_agent == stk->default_agent && stk->respond_on_exit)
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);

	stk->default_agent = NULL;
	stk->current_agent = stk->session_agent;
	stk->respond_on_exit = FALSE;
}

static void session_agent_notify(gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	DBG("Session Agent removed");

	if (stk->current_agent == stk->session_agent && stk->respond_on_exit) {
		DBG("Sending Terminate response for session agent");
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
	}

	stk->session_agent = NULL;
	stk->current_agent = stk->default_agent;
	stk->respond_on_exit = FALSE;

	if (stk->remove_agent_source) {
		g_source_remove(stk->remove_agent_source);
		stk->remove_agent_source = 0;
	}
}

static gboolean session_agent_remove_cb(gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	stk->remove_agent_source = 0;

	stk_agent_free(stk->session_agent);

	return FALSE;
}

/* Safely remove the agent even inside a callback */
static void session_agent_remove(struct ofono_stk *stk)
{
	if (!stk->remove_agent_source)
		stk->remove_agent_source =
			g_timeout_add(0, session_agent_remove_cb, stk);
}

static DBusMessage *stk_register_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_stk *stk = data;
	const char *agent_path;

	if (stk->default_agent)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_dbus_valid_object_path(agent_path))
		return __ofono_error_invalid_format(msg);

	stk->default_agent = stk_agent_new(agent_path,
						dbus_message_get_sender(msg),
						FALSE);
	if (!stk->default_agent)
		return __ofono_error_failed(msg);

	stk_agent_set_removed_notify(stk->default_agent,
					default_agent_notify, stk);

	if (!stk->session_agent)
		stk->current_agent = stk->default_agent;

	return dbus_message_new_method_return(msg);
}

static DBusMessage *stk_unregister_agent(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_stk *stk = data;
	const char *agent_path;
	const char *agent_bus = dbus_message_get_sender(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!stk->default_agent)
		return __ofono_error_failed(msg);

	if (!stk_agent_matches(stk->default_agent, agent_path, agent_bus))
		return __ofono_error_failed(msg);

	stk_agent_free(stk->default_agent);

	return dbus_message_new_method_return(msg);
}

static void menu_selection_envelope_cb(struct ofono_stk *stk, gboolean ok,
					const unsigned char *data, int len)
{
	unsigned char selection;
	const char *agent_path;
	DBusMessage *reply;

	DBG("");

	if (!ok) {
		ofono_error("Sending Menu Selection to UICC failed");

		reply = __ofono_error_failed(stk->pending);

		goto out;
	}

	if (len)
		ofono_error("Menu Selection returned %i bytes of unwanted data",
				len);

	DBG("Menu Selection envelope submission gave no error");

	dbus_message_get_args(stk->pending, NULL,
				DBUS_TYPE_BYTE, &selection,
				DBUS_TYPE_OBJECT_PATH, &agent_path,
				DBUS_TYPE_INVALID);

	stk->session_agent = stk_agent_new(agent_path,
					dbus_message_get_sender(stk->pending),
					TRUE);
	if (!stk->session_agent) {
		reply = __ofono_error_failed(stk->pending);

		goto out;
	}

	stk_agent_set_removed_notify(stk->session_agent,
					session_agent_notify, stk);

	stk->current_agent = stk->session_agent;

	reply = dbus_message_new_method_return(stk->pending);

out:
	__ofono_dbus_pending_reply(&stk->pending, reply);
}

static DBusMessage *stk_select_item(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_stk *stk = data;
	const char *agent_path;
	unsigned char selection, i;
	struct stk_envelope e;
	struct stk_menu *menu = stk->main_menu;

	DBG("");

	if (stk->pending || stk->session_agent)
		return __ofono_error_busy(msg);

	if (!menu)
		return __ofono_error_not_supported(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_BYTE, &selection,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_dbus_valid_object_path(agent_path))
		return __ofono_error_invalid_format(msg);

	for (i = 0; i < selection && menu->items[i].text; i++);

	if (i != selection)
		return __ofono_error_invalid_format(msg);

	memset(&e, 0, sizeof(e));
	e.type = STK_ENVELOPE_TYPE_MENU_SELECTION;
	e.src = STK_DEVICE_IDENTITY_TYPE_KEYPAD,
	e.menu_selection.item_id = menu->items[selection].item_id;
	e.menu_selection.help_request = FALSE;

	DBG("");

	if (stk_send_envelope(stk, &e, menu_selection_envelope_cb, 0))
		return __ofono_error_failed(msg);

	stk->pending = dbus_message_ref(msg);

	return NULL;
}

static GDBusMethodTable stk_methods[] = {
	{ "GetProperties",		"",	"a{sv}",stk_get_properties },
	{ "SelectItem",			"yo",	"",	stk_select_item,
					G_DBUS_METHOD_FLAG_ASYNC },
	{ "RegisterAgent",		"o",	"",	stk_register_agent },
	{ "UnregisterAgent",		"o",	"",	stk_unregister_agent },

	{ }
};

static GDBusSignalTable stk_signals[] = {
	{ "PropertyChanged",	"sv" },

	{ }
};

static gboolean handle_command_more_time(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	/* Do nothing */

	return TRUE;
}

static void send_sms_cancel(struct ofono_stk *stk)
{
	stk->sms_submit_req->cancelled = TRUE;

	if (!stk->pending_cmd->send_sms.alpha_id ||
			!stk->pending_cmd->send_sms.alpha_id[0])
		return;

	stk_alpha_id_unset(stk);
}

static void send_sms_submit_cb(gboolean ok, void *data)
{
	struct sms_submit_req *req = data;
	struct ofono_stk *stk = req->stk;
	struct ofono_error failure = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;

	DBG("SMS submission %s", ok ? "successful" : "failed");

	if (req->cancelled) {
		DBG("Received an SMS submitted callback after the "
				"proactive command was cancelled");
		return;
	}

	if (stk->pending_cmd->send_sms.alpha_id &&
			stk->pending_cmd->send_sms.alpha_id[0])
		stk_alpha_id_unset(stk);

	memset(&rsp, 0, sizeof(rsp));

	if (ok == FALSE)
		rsp.result.type = STK_RESULT_TYPE_NETWORK_UNAVAILABLE;

	if (stk_respond(stk, &rsp, stk_command_cb))
		stk_command_cb(&failure, stk);
}

static gboolean handle_command_send_sms(const struct stk_command *cmd,
					struct stk_response *rsp,
					struct ofono_stk *stk)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	struct ofono_atom *sms_atom;
	struct ofono_sms *sms;
	GSList msg_list;

	sms_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SMS);

	if (!sms_atom || !__ofono_atom_get_registered(sms_atom)) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	sms = __ofono_atom_get_data(sms_atom);

	stk->sms_submit_req = g_new0(struct sms_submit_req, 1);
	stk->sms_submit_req->stk = stk;

	msg_list.data = (void *) &cmd->send_sms.gsm_sms;
	msg_list.next = NULL;

	__ofono_sms_txq_submit(sms, &msg_list, 0, send_sms_submit_cb,
				stk->sms_submit_req, g_free);

	stk->cancel_cmd = send_sms_cancel;

	if (cmd->send_sms.alpha_id && cmd->send_sms.alpha_id[0])
		stk_alpha_id_set(stk, cmd->send_sms.alpha_id);

	return FALSE;
}

static gboolean handle_command_set_idle_text(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(stk->atom);
	const char *idle_mode_text;

	if (stk->idle_mode_text) {
		g_free(stk->idle_mode_text);
		stk->idle_mode_text = NULL;
	}

	if (cmd->setup_idle_mode_text.text)
		stk->idle_mode_text = g_strdup(cmd->setup_idle_mode_text.text);

	idle_mode_text = stk->idle_mode_text ? stk->idle_mode_text : "";
	ofono_dbus_signal_property_changed(conn, path, OFONO_STK_INTERFACE,
						"IdleModeText",
						DBUS_TYPE_STRING,
						&idle_mode_text);

	return TRUE;
}

static void timer_expiration_cb(struct ofono_stk *stk, gboolean ok,
				const unsigned char *data, int len)
{
	if (!ok) {
		ofono_error("Timer Expiration reporting failed");
		return;
	}

	if (len)
		ofono_error("Timer Expiration returned %i bytes of data",
				len);

	DBG("Timer Expiration reporting to UICC reported no error");
}

static gboolean timers_cb(gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	stk->timers_source = 0;

	timers_update(stk);

	return FALSE;
}

static void timer_value_from_seconds(struct stk_timer_value *val, int seconds)
{
	val->has_value = TRUE;
	val->hour = seconds / 3600;
	seconds -= val->hour * 3600;
	val->minute = seconds / 60;
	seconds -= val->minute * 60;
	val->second = seconds;
}

static void timers_update(struct ofono_stk *stk)
{
	time_t min = 0, now = time(NULL);
	int i;

	if (stk->timers_source) {
		g_source_remove(stk->timers_source);
		stk->timers_source = 0;
	}

	for (i = 0; i < 8; i++) {
		if (!stk->timers[i].expiry)
			continue;

		if (stk->timers[i].expiry <= now) {
			struct stk_envelope e;
			int seconds = now - stk->timers[i].start;

			stk->timers[i].expiry = 0;

			memset(&e, 0, sizeof(e));

			e.type = STK_ENVELOPE_TYPE_TIMER_EXPIRATION;
			e.src = STK_DEVICE_IDENTITY_TYPE_TERMINAL,
			e.timer_expiration.id = i + 1;
			timer_value_from_seconds(&e.timer_expiration.value,
							seconds);

			/*
			 * TODO: resubmit until success, providing current
			 * time difference every time we re-send.
			 */
			if (stk_send_envelope(stk, &e, timer_expiration_cb, 0))
				timer_expiration_cb(stk, FALSE, NULL, -1);

			continue;
		}

		if (stk->timers[i].expiry < now + min || min == 0)
			min = stk->timers[i].expiry - now;
	}

	if (min)
		stk->timers_source = g_timeout_add_seconds(min, timers_cb, stk);
}

static gboolean handle_command_timer_mgmt(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	int op = cmd->qualifier & 3;
	time_t seconds, now = time(NULL);
	struct stk_timer *tmr;

	if (cmd->timer_mgmt.timer_id < 1 || cmd->timer_mgmt.timer_id > 8) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	tmr = &stk->timers[cmd->timer_mgmt.timer_id - 1];

	switch (op) {
	case 0: /* Start */
		seconds = cmd->timer_mgmt.timer_value.second +
			cmd->timer_mgmt.timer_value.minute * 60 +
			cmd->timer_mgmt.timer_value.hour * 3600;

		tmr->expiry = now + seconds;
		tmr->start = now;

		timers_update(stk);
		break;

	case 1: /* Deactivate */
		if (!tmr->expiry) {
			rsp->result.type = STK_RESULT_TYPE_TIMER_CONFLICT;

			return TRUE;
		}

		seconds = MAX(0, tmr->expiry - now);
		tmr->expiry = 0;

		timers_update(stk);

		timer_value_from_seconds(&rsp->timer_mgmt.value, seconds);
		break;

	case 2: /* Get current value */
		if (!tmr->expiry) {
			rsp->result.type = STK_RESULT_TYPE_TIMER_CONFLICT;

			return TRUE;
		}

		seconds = MAX(0, tmr->expiry - now);
		timer_value_from_seconds(&rsp->timer_mgmt.value, seconds);
		break;

	default:
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;

		return TRUE;
	}

	rsp->timer_mgmt.id = cmd->timer_mgmt.timer_id;

	return TRUE;
}

static gboolean handle_command_poll_interval(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	int seconds;

	switch (cmd->poll_interval.duration.unit) {
	case STK_DURATION_TYPE_MINUTES:
		seconds = cmd->poll_interval.duration.interval * 60;
		break;
	case STK_DURATION_TYPE_SECONDS:
		seconds = cmd->poll_interval.duration.interval;
		break;
	case STK_DURATION_TYPE_SECOND_TENTHS:
		seconds = (4 + cmd->poll_interval.duration.interval) / 10;
		if (seconds < 1)
			seconds = 1;
		break;
	default:
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	ofono_modem_set_integer(modem, "status-poll-interval", seconds);

	if (seconds > 255) {
		rsp->poll_interval.max_interval.unit =
			STK_DURATION_TYPE_MINUTES;
		rsp->poll_interval.max_interval.interval = seconds / 60;
	} else {
		rsp->poll_interval.max_interval.unit =
			STK_DURATION_TYPE_SECONDS;
		rsp->poll_interval.max_interval.interval = seconds;
	}

	return TRUE;
}

static gboolean handle_command_set_up_menu(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	gboolean modified = FALSE;

	if (stk->main_menu) {
		stk_menu_free(stk->main_menu);
		stk->main_menu = NULL;

		modified = TRUE;
	}

	if (cmd->setup_menu.items) {
		stk->main_menu = stk_menu_create_from_set_up_menu(cmd);

		if (stk->main_menu)
			modified = TRUE;
		else
			rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
	}

	if (modified)
		emit_menu_changed(stk);

	return TRUE;
}

static void request_selection_destroy(void *user_data)
{
	struct ofono_stk *stk = user_data;

	stk_menu_free(stk->select_item_menu);
	stk->select_item_menu = NULL;
}

static void request_selection_cb(enum stk_agent_result result, uint8_t id,
					void *user_data)
{
	struct ofono_stk *stk = user_data;

	stk->respond_on_exit = FALSE;

	switch (result) {
	case STK_AGENT_RESULT_OK:
	{
		static struct ofono_error error =
			{ .type = OFONO_ERROR_TYPE_FAILURE };
		struct stk_response rsp;

		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_SUCCESS;
		rsp.select_item.item_id = id;

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		break;
	}

	case STK_AGENT_RESULT_BACK:
		send_simple_response(stk, STK_RESULT_TYPE_GO_BACK);
		break;

	case STK_AGENT_RESULT_TIMEOUT:
		send_simple_response(stk, STK_RESULT_TYPE_NO_RESPONSE);
		break;

	case STK_AGENT_RESULT_TERMINATE:
	default:
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
		break;
	}
}

static gboolean handle_command_select_item(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	stk->select_item_menu = stk_menu_create_from_select_item(cmd);

	if (!stk->select_item_menu) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;

		return TRUE;
	}

	/* We most likely got an out of memory error, tell SIM to retry */
	if (stk_agent_request_selection(stk->current_agent,
					stk->select_item_menu,
					request_selection_cb, stk,
					request_selection_destroy,
					stk->timeout * 1000) < 0) {
		request_selection_destroy(stk);

		rsp->result.type = STK_RESULT_TYPE_TERMINAL_BUSY;
		return TRUE;
	}

	stk->cancel_cmd = stk_request_cancel;
	stk->respond_on_exit = TRUE;

	return FALSE;
}

static void display_text_destroy(void *user_data)
{
	struct ofono_stk *stk = user_data;

	stk->immediate_response = FALSE;
}

static void display_text_cb(enum stk_agent_result result, void *user_data)
{
	struct ofono_stk *stk = user_data;
	gboolean confirm;

	stk->respond_on_exit = FALSE;

	/*
	 * There are four possible paths for DisplayText with immediate
	 * response flag set:
	 *	1. Agent drops off the bus.  In that case regular removal
	 *	semantics apply and the agent is removed.
	 *
	 *	2. A new SIM command arrives.  In this case the agent is
	 *	canceled and a new command is processed.  This function is
	 *	not called in this case.
	 *
	 *	3. The session is ended by the SIM.  This case is ignored,
	 *	and will result in either case 1, 2 or 4 occurring.
	 *
	 *	4. Agent reports an error or success.  This function is called
	 *	with the result.
	 *
	 *	NOTE: If the agent reports a TERMINATE result, the agent will
	 *	be removed.  Since the response has been already sent, there
	 *	is no way to signal the end of session to the SIM.  Hence
	 *	it is assumed that immediate response flagged commands will
	 *	only occur at the end of session.
	 */
	if (stk->immediate_response) {
		if (stk->session_agent)
			session_agent_remove(stk);

		return;
	}

	switch (result) {
	case STK_AGENT_RESULT_OK:
		send_simple_response(stk, STK_RESULT_TYPE_SUCCESS);
		break;

	case STK_AGENT_RESULT_BACK:
		send_simple_response(stk, STK_RESULT_TYPE_GO_BACK);
		break;

	case STK_AGENT_RESULT_TIMEOUT:
		confirm = (stk->pending_cmd->qualifier & (1 << 7)) != 0;
		send_simple_response(stk, confirm ?
			STK_RESULT_TYPE_NO_RESPONSE : STK_RESULT_TYPE_SUCCESS);
		break;

	case STK_AGENT_RESULT_TERMINATE:
	default:
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
		break;
	}
}

static gboolean handle_command_display_text(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	int timeout = stk->short_timeout * 1000;
	struct stk_command_display_text *dt = &stk->pending_cmd->display_text;
	uint8_t qualifier = stk->pending_cmd->qualifier;
	ofono_bool_t priority = (qualifier & (1 << 0)) != 0;

	if (dt->duration.interval) {
		timeout = dt->duration.interval;
		switch (dt->duration.unit) {
		case STK_DURATION_TYPE_MINUTES:
			timeout *= 60;
		case STK_DURATION_TYPE_SECONDS:
			timeout *= 10;
		case STK_DURATION_TYPE_SECOND_TENTHS:
			timeout *= 100;
		}
	}

	/* We most likely got an out of memory error, tell SIM to retry */
	if (stk_agent_display_text(stk->current_agent, dt->text, 0, priority,
					display_text_cb, stk,
					display_text_destroy, timeout) < 0) {
		rsp->result.type = STK_RESULT_TYPE_TERMINAL_BUSY;
		return TRUE;
	}

	if (cmd->display_text.immediate_response)
		stk->immediate_response = TRUE;

	DBG("Immediate Response: %d", stk->immediate_response);

	if (stk->immediate_response == FALSE) {
		stk->respond_on_exit = TRUE;
		stk->cancel_cmd = stk_request_cancel;
	}

	return stk->immediate_response;
}

static void set_get_inkey_duration(struct stk_duration *duration,
					struct timeval *start_ts)
{
	struct timeval end_ts;
	int interval;

	gettimeofday(&end_ts, NULL);

	interval = (end_ts.tv_usec + 1099999 - start_ts->tv_usec) / 100000;
	interval += (end_ts.tv_sec - start_ts->tv_sec) * 10;
	interval -= 10;

	switch (duration->unit) {
	case STK_DURATION_TYPE_MINUTES:
		interval = (interval + 59) / 60;
	case STK_DURATION_TYPE_SECONDS:
		interval = (interval + 9) / 10;
	case STK_DURATION_TYPE_SECOND_TENTHS:
		break;
	}

	duration->interval = interval;
}

static void request_confirmation_cb(enum stk_agent_result result,
					gboolean confirm,
					void *user_data)
{
	struct ofono_stk *stk = user_data;
	static struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_command_get_inkey *cmd = &stk->pending_cmd->get_inkey;
	struct stk_response rsp;

	stk->respond_on_exit = FALSE;

	switch (result) {
	case STK_AGENT_RESULT_OK:
		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_SUCCESS;
		rsp.get_inkey.text.text = confirm ? "" : NULL;
		rsp.get_inkey.text.yesno = TRUE;

		if (cmd->duration.interval) {
			rsp.get_inkey.duration.unit = cmd->duration.unit;
			set_get_inkey_duration(&rsp.get_inkey.duration,
						&stk->get_inkey_start_ts);
		}

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		break;

	case STK_AGENT_RESULT_BACK:
		send_simple_response(stk, STK_RESULT_TYPE_GO_BACK);
		break;

	case STK_AGENT_RESULT_TIMEOUT:
		send_simple_response(stk, STK_RESULT_TYPE_NO_RESPONSE);
		break;

	case STK_AGENT_RESULT_TERMINATE:
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
		break;
	}
}

static void request_key_cb(enum stk_agent_result result, char *string,
				void *user_data)
{
	struct ofono_stk *stk = user_data;
	static struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_command_get_inkey *cmd = &stk->pending_cmd->get_inkey;
	struct stk_response rsp;

	stk->respond_on_exit = FALSE;

	switch (result) {
	case STK_AGENT_RESULT_OK:
		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_SUCCESS;
		rsp.get_inkey.text.text = string;

		if (cmd->duration.interval) {
			rsp.get_inkey.duration.unit = cmd->duration.unit;
			set_get_inkey_duration(&rsp.get_inkey.duration,
						&stk->get_inkey_start_ts);
		}

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		break;

	case STK_AGENT_RESULT_BACK:
		send_simple_response(stk, STK_RESULT_TYPE_GO_BACK);
		break;

	case STK_AGENT_RESULT_TIMEOUT:
		send_simple_response(stk, STK_RESULT_TYPE_NO_RESPONSE);
		break;

	case STK_AGENT_RESULT_TERMINATE:
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
		break;
	}
}

static gboolean handle_command_get_inkey(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	int timeout = stk->timeout * 1000;
	const struct stk_command_get_inkey *gi = &cmd->get_inkey;
	uint8_t qualifier = stk->pending_cmd->qualifier;
	gboolean alphabet = (qualifier & (1 << 0)) != 0;
	gboolean ucs2 = (qualifier & (1 << 1)) != 0;
	gboolean yesno = (qualifier & (1 << 2)) != 0;
	/*
	 * Note: immediate response and help parameter values are not
	 * provided by current api.
	 */
	uint8_t icon_id = 0;
	int err;

	if (gi->duration.interval) {
		timeout = gi->duration.interval;
		switch (gi->duration.unit) {
		case STK_DURATION_TYPE_MINUTES:
			timeout *= 60;
		case STK_DURATION_TYPE_SECONDS:
			timeout *= 10;
		case STK_DURATION_TYPE_SECOND_TENTHS:
			timeout *= 100;
		}
	}

	gettimeofday(&stk->get_inkey_start_ts, NULL);

	if (yesno)
		err = stk_agent_request_confirmation(stk->current_agent,
							gi->text, icon_id,
							request_confirmation_cb,
							stk, NULL, timeout);
	else if (alphabet)
		err = stk_agent_request_key(stk->current_agent, gi->text,
						icon_id, ucs2, request_key_cb,
						stk, NULL, timeout);
	else
		err = stk_agent_request_digit(stk->current_agent, gi->text,
						icon_id, request_key_cb,
						stk, NULL, timeout);

	if (err < 0) {
		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		rsp->result.type = STK_RESULT_TYPE_TERMINAL_BUSY;
		return TRUE;
	}

	stk->respond_on_exit = TRUE;
	stk->cancel_cmd = stk_request_cancel;

	return FALSE;
}

static void request_string_cb(enum stk_agent_result result, char *string,
				void *user_data)
{
	struct ofono_stk *stk = user_data;
	static struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	uint8_t qualifier = stk->pending_cmd->qualifier;
	gboolean packed = (qualifier & (1 << 3)) != 0;
	struct stk_response rsp;

	stk->respond_on_exit = FALSE;

	switch (result) {
	case STK_AGENT_RESULT_OK:
		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_SUCCESS;
		rsp.get_input.text.text = string;
		rsp.get_input.text.packed = packed;

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		break;

	case STK_AGENT_RESULT_BACK:
		send_simple_response(stk, STK_RESULT_TYPE_GO_BACK);
		break;

	case STK_AGENT_RESULT_TIMEOUT:
		send_simple_response(stk, STK_RESULT_TYPE_NO_RESPONSE);
		break;

	case STK_AGENT_RESULT_TERMINATE:
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
		break;
	}
}

static gboolean handle_command_get_input(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	int timeout = stk->timeout * 1000;
	const struct stk_command_get_input *gi = &cmd->get_input;
	uint8_t qualifier = stk->pending_cmd->qualifier;
	gboolean alphabet = (qualifier & (1 << 0)) != 0;
	gboolean ucs2 = (qualifier & (1 << 1)) != 0;
	gboolean hidden = (qualifier & (1 << 2)) != 0;
	uint8_t icon_id = 0;
	int err;

	if (alphabet)
		err = stk_agent_request_input(stk->current_agent, gi->text,
						icon_id, gi->default_text, ucs2,
						gi->resp_len.min,
						gi->resp_len.max, hidden,
						request_string_cb,
						stk, NULL, timeout);
	else
		err = stk_agent_request_digits(stk->current_agent, gi->text,
						icon_id, gi->default_text,
						gi->resp_len.min,
						gi->resp_len.max, hidden,
						request_string_cb,
						stk, NULL, timeout);

	if (err < 0) {
		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		rsp->result.type = STK_RESULT_TYPE_TERMINAL_BUSY;
		return TRUE;
	}

	stk->respond_on_exit = TRUE;
	stk->cancel_cmd = stk_request_cancel;

	return FALSE;
}

static void call_setup_connected(struct ofono_call *call, void *data)
{
	struct ofono_stk *stk = data;
	struct stk_response rsp;
	static struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	static unsigned char facility_rejected_result[] = { 0x9d };

	if (!call) {
		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_NETWORK_UNAVAILABLE;
		rsp.result.additional_len = sizeof(facility_rejected_result);
		rsp.result.additional = facility_rejected_result;

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		return;
	}

	if (call->status == CALL_STATUS_ACTIVE)
		send_simple_response(stk, STK_RESULT_TYPE_SUCCESS);
	else
		send_simple_response(stk, STK_RESULT_TYPE_USER_CANCEL);
}

static void call_setup_cancel(struct ofono_stk *stk)
{
	struct ofono_voicecall *vc;
	struct ofono_atom *vc_atom;

	vc_atom = __ofono_modem_find_atom(__ofono_atom_get_modem(stk->atom),
						OFONO_ATOM_TYPE_VOICECALL);
	if (!vc_atom)
		return;

	vc = __ofono_atom_get_data(vc_atom);
	if (vc)
		__ofono_voicecall_dial_cancel(vc);
}

static void confirm_call_cb(enum stk_agent_result result, gboolean confirm,
				void *user_data)
{
	struct ofono_stk *stk = user_data;
	static struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	const struct stk_command_setup_call *sc = &stk->pending_cmd->setup_call;
	uint8_t qualifier = stk->pending_cmd->qualifier;
	static unsigned char busy_on_call_result[] = { 0x02 };
	static unsigned char no_cause_result[] = { 0x00 };
	struct ofono_voicecall *vc = NULL;
	struct ofono_atom *vc_atom;
	struct stk_response rsp;
	int err;

	stk->respond_on_exit = FALSE;

	switch (result) {
	case STK_AGENT_RESULT_TIMEOUT:
		confirm = FALSE;
		/* Fall through */

	case STK_AGENT_RESULT_OK:
		if (confirm)
			break;

		send_simple_response(stk, STK_RESULT_TYPE_USER_REJECT);
		return;

	case STK_AGENT_RESULT_TERMINATE:
	default:
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
		return;
	}

	vc_atom = __ofono_modem_find_atom(__ofono_atom_get_modem(stk->atom),
						OFONO_ATOM_TYPE_VOICECALL);
	if (vc_atom)
		vc = __ofono_atom_get_data(vc_atom);

	if (!vc) {
		send_simple_response(stk, STK_RESULT_TYPE_NOT_CAPABLE);
		return;
	}

	err = __ofono_voicecall_dial(vc, sc->addr.number, sc->addr.ton_npi,
					sc->alpha_id_call_setup, 0,
					qualifier >> 1, call_setup_connected,
					stk);
	if (err >= 0) {
		stk->cancel_cmd = call_setup_cancel;

		return;
	}

	if (err == -EBUSY) {
		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_TERMINAL_BUSY;
		rsp.result.additional_len = sizeof(busy_on_call_result);
		rsp.result.additional = busy_on_call_result;

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		return;
	}

	if (err == -ENOSYS) {
		send_simple_response(stk, STK_RESULT_TYPE_NOT_CAPABLE);

		return;
	}

	memset(&rsp, 0, sizeof(rsp));

	rsp.result.type = STK_RESULT_TYPE_NETWORK_UNAVAILABLE;
	rsp.result.additional_len = sizeof(no_cause_result);
	rsp.result.additional = no_cause_result;

	if (stk_respond(stk, &rsp, stk_command_cb))
		stk_command_cb(&error, stk);
}

static gboolean handle_command_set_up_call(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	const struct stk_command_setup_call *sc = &cmd->setup_call;
	uint8_t qualifier = cmd->qualifier;
	static unsigned char busy_on_call_result[] = { 0x02 };
	struct ofono_voicecall *vc = NULL;
	struct ofono_atom *vc_atom;
	int err;

	if (qualifier > 5) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	/*
	 * Passing called party subaddress and establishing non-speech
	 * calls are not supported.
	 */
	if (sc->ccp.len || sc->subaddr.len) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	vc_atom = __ofono_modem_find_atom(__ofono_atom_get_modem(stk->atom),
						OFONO_ATOM_TYPE_VOICECALL);
	if (vc_atom)
		vc = __ofono_atom_get_data(vc_atom);

	if (!vc) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	if (__ofono_voicecall_is_busy(vc, qualifier >> 1)) {
		rsp->result.type = STK_RESULT_TYPE_TERMINAL_BUSY;
		rsp->result.additional_len = sizeof(busy_on_call_result);
		rsp->result.additional = busy_on_call_result;
		return TRUE;
	}

	err = stk_agent_confirm_call(stk->current_agent, sc->alpha_id_usr_cfm,
					0, confirm_call_cb, stk, NULL,
					stk->timeout * 1000);

	if (err < 0) {
		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		rsp->result.type = STK_RESULT_TYPE_TERMINAL_BUSY;
		return TRUE;
	}

	stk->respond_on_exit = TRUE;
	stk->cancel_cmd = stk_request_cancel;

	return FALSE;
}

static void stk_proactive_command_cancel(struct ofono_stk *stk)
{
	if (stk->immediate_response)
		stk_request_cancel(stk);

	if (stk->pending_cmd) {
		stk->cancel_cmd(stk);
		stk_command_free(stk->pending_cmd);
		stk->pending_cmd = NULL;
		stk->cancel_cmd = NULL;
	}
}

void ofono_stk_proactive_session_end_notify(struct ofono_stk *stk)
{
	/* Wait until we receive the next command */
	if (stk->immediate_response)
		return;

	stk_proactive_command_cancel(stk);

	if (stk->session_agent)
		stk_agent_free(stk->session_agent);
}

void ofono_stk_proactive_command_notify(struct ofono_stk *stk,
					int length, const unsigned char *pdu)
{
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;
	int err;
	gboolean respond = TRUE;

	/*
	 * Depending on the hardware we may have received a new
	 * command before we managed to send a TERMINAL RESPONSE to
	 * the previous one.  3GPP says in the current revision only
	 * one command can be executing at any time, so assume that
	 * the previous one is being cancelled and the card just
	 * expects a response to the new one.
	 */
	stk_proactive_command_cancel(stk);

	stk->pending_cmd = stk_command_new_from_pdu(pdu, length);
	if (!stk->pending_cmd) {
		ofono_error("Can't parse proactive command");

		/*
		 * Nothing we can do, we'd need at least Command Details
		 * to be able to respond with an error.
		 */
		return;
	}

	switch (stk->pending_cmd->status) {
	case STK_PARSE_RESULT_OK:
		break;

	case STK_PARSE_RESULT_MISSING_VALUE:
		send_simple_response(stk, STK_RESULT_TYPE_MINIMUM_NOT_MET);
		return;

	case STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD:
		send_simple_response(stk, STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD);
		return;

	case STK_PARSE_RESULT_TYPE_NOT_UNDERSTOOD:
	default:
		send_simple_response(stk,
					STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD);
		return;
	}

	/*
	 * In case no agent is registered, we should reject commands destined
	 * to the Agent with a NOT_CAPABLE error.
	 */
	if (stk->current_agent == NULL) {
		switch (stk->pending_cmd->type) {
		case STK_COMMAND_TYPE_SELECT_ITEM:
		case STK_COMMAND_TYPE_DISPLAY_TEXT:
		case STK_COMMAND_TYPE_GET_INKEY:
		case STK_COMMAND_TYPE_GET_INPUT:
		case STK_COMMAND_TYPE_PLAY_TONE:
		case STK_COMMAND_TYPE_SETUP_CALL:
			send_simple_response(stk, STK_RESULT_TYPE_NOT_CAPABLE);
			return;

		default:
			break;
		}
	}

	memset(&rsp, 0, sizeof(rsp));

	switch (stk->pending_cmd->type) {
	case STK_COMMAND_TYPE_MORE_TIME:
		respond = handle_command_more_time(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_SEND_SMS:
		respond = handle_command_send_sms(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_SETUP_IDLE_MODE_TEXT:
		respond = handle_command_set_idle_text(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_TIMER_MANAGEMENT:
		respond = handle_command_timer_mgmt(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_POLL_INTERVAL:
		respond = handle_command_poll_interval(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_SETUP_MENU:
		respond = handle_command_set_up_menu(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_SELECT_ITEM:
		respond = handle_command_select_item(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_DISPLAY_TEXT:
		respond = handle_command_display_text(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_GET_INKEY:
		respond = handle_command_get_inkey(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_GET_INPUT:
		respond = handle_command_get_input(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_SETUP_CALL:
		respond = handle_command_set_up_call(stk->pending_cmd,
							&rsp, stk);
		break;

	default:
		rsp.result.type = STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD;
		break;
	}

	if (respond == FALSE)
		return;

	err = stk_respond(stk, &rsp, stk_command_cb);
	if (err)
		stk_command_cb(&error, stk);
}

int ofono_stk_driver_register(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_stk_driver_unregister(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void stk_unregister(struct ofono_atom *atom)
{
	struct ofono_stk *stk = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	if (stk->session_agent)
		stk_agent_free(stk->session_agent);

	if (stk->default_agent)
		stk_agent_free(stk->default_agent);

	if (stk->pending_cmd) {
		stk_command_free(stk->pending_cmd);
		stk->pending_cmd = NULL;
		stk->cancel_cmd = NULL;
	}

	if (stk->idle_mode_text) {
		g_free(stk->idle_mode_text);
		stk->idle_mode_text = NULL;
	}

	if (stk->timers_source) {
		g_source_remove(stk->timers_source);
		stk->timers_source = 0;
	}

	if (stk->main_menu) {
		stk_menu_free(stk->main_menu);
		stk->main_menu = NULL;
	}

	g_queue_foreach(stk->envelope_q, (GFunc) g_free, NULL);
	g_queue_free(stk->envelope_q);

	ofono_modem_remove_interface(modem, OFONO_STK_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_STK_INTERFACE);
}

static void stk_remove(struct ofono_atom *atom)
{
	struct ofono_stk *stk = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (stk == NULL)
		return;

	if (stk->driver && stk->driver->remove)
		stk->driver->remove(stk);

	g_free(stk);
}

struct ofono_stk *ofono_stk_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_stk *stk;
	GSList *l;

	if (driver == NULL)
		return NULL;

	stk = g_try_new0(struct ofono_stk, 1);

	if (stk == NULL)
		return NULL;

	stk->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_STK,
						stk_remove, stk);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_stk_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(stk, vendor, data) < 0)
			continue;

		stk->driver = drv;
		break;
	}

	return stk;
}

void ofono_stk_register(struct ofono_stk *stk)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	const char *path = __ofono_atom_get_path(stk->atom);

	if (!g_dbus_register_interface(conn, path, OFONO_STK_INTERFACE,
					stk_methods, stk_signals, NULL,
					stk, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_STK_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_STK_INTERFACE);

	__ofono_atom_register(stk->atom, stk_unregister);

	stk->timeout = 600; /* 10 minutes */
	stk->short_timeout = 20; /* 20 seconds */
	stk->envelope_q = g_queue_new();
}

void ofono_stk_remove(struct ofono_stk *stk)
{
	__ofono_atom_free(stk->atom);
}

void ofono_stk_set_data(struct ofono_stk *stk, void *data)
{
	stk->driver_data = data;
}

void *ofono_stk_get_data(struct ofono_stk *stk)
{
	return stk->driver_data;
}
