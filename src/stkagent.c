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
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "stkagent.h"

typedef void (*stk_agent_request_return)(struct stk_agent *agent,
						enum stk_agent_result result,
						DBusMessage *reply);

struct stk_agent {
	char *path;				/* Agent Path */
	char *bus;				/* Agent bus */
	ofono_bool_t is_default;		/* False if user-session */
	guint disconnect_watch;			/* DBus disconnect watch */
	ofono_destroy_func removed_cb;
	void *removed_data;
	DBusMessage *msg;
	DBusPendingCall *call;
	guint cmd_send_source;
	stk_agent_request_return cmd_cb;
	int cmd_timeout;
	stk_agent_generic_cb user_cb;
	void *user_data;

	const struct stk_menu *request_selection_menu;
};

#define OFONO_NAVIGATION_PREFIX OFONO_SERVICE ".Error"
#define OFONO_NAVIGATION_GOBACK OFONO_NAVIGATION_PREFIX ".GoBack"
#define OFONO_NAVIGATION_TERMINATED OFONO_NAVIGATION_PREFIX ".EndSession"

static void stk_agent_send_noreply(struct stk_agent *agent, const char *method)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *message;

	message = dbus_message_new_method_call(agent->bus, agent->path,
						OFONO_SIM_APP_INTERFACE,
						method);
	if (message == NULL)
		return;

	dbus_message_set_no_reply(message, TRUE);

	g_dbus_send_message(conn, message);
}

static inline void stk_agent_send_release(struct stk_agent *agent)
{
	stk_agent_send_noreply(agent, "Release");
}

static inline void stk_agent_send_cancel(struct stk_agent *agent)
{
	stk_agent_send_noreply(agent, "Cancel");
}

static void stk_agent_request_end(struct stk_agent *agent)
{
	agent->cmd_cb = NULL;

	if (agent->cmd_send_source) {
		g_source_remove(agent->cmd_send_source);
		agent->cmd_send_source = 0;
	}

	if (agent->msg) {
		dbus_message_unref(agent->msg);
		agent->msg = NULL;
	}

	if (agent->call) {
		dbus_pending_call_cancel(agent->call);
		dbus_pending_call_unref(agent->call);
		agent->call = NULL;
	}
}

ofono_bool_t stk_agent_busy(struct stk_agent *agent)
{
	return agent->cmd_cb != NULL;
}

ofono_bool_t stk_agent_matches(struct stk_agent *agent,
				const char *path, const char *sender)
{
	return !strcmp(agent->path, path) && !strcmp(agent->bus, sender);
}

void stk_agent_set_removed_notify(struct stk_agent *agent,
					ofono_destroy_func destroy,
					void *user_data)
{
	agent->removed_cb = destroy;
	agent->removed_data = user_data;
}

void stk_agent_request_cancel(struct stk_agent *agent)
{
	if (!stk_agent_busy(agent))
		return;

	agent->cmd_cb(agent, STK_AGENT_RESULT_CANCEL, NULL);

	stk_agent_request_end(agent);

	stk_agent_send_cancel(agent);
}

static void stk_agent_request_terminate(struct stk_agent *agent)
{
	agent->cmd_cb(agent, STK_AGENT_RESULT_TERMINATE, NULL);

	stk_agent_request_end(agent);
}

void stk_agent_free(struct stk_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	gboolean busy = stk_agent_busy(agent);

	if (busy)
		stk_agent_request_terminate(agent);

	if (agent->disconnect_watch) {
		if (busy)
			stk_agent_send_cancel(agent);

		stk_agent_send_release(agent);

		g_dbus_remove_watch(conn, agent->disconnect_watch);
		agent->disconnect_watch = 0;
	}

	if (agent->removed_cb)
		agent->removed_cb(agent->removed_data);

	g_free(agent->path);
	g_free(agent->bus);
	g_free(agent);
}

static void stk_agent_request_reply_handle(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	DBusError err;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result = STK_AGENT_RESULT_OK;

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, reply)) {
		ofono_error("SimAppAgent %s replied with error %s, %s",
				agent->path, err.name, err.message);

		if (g_str_equal(err.name, DBUS_ERROR_NO_REPLY))
			result = STK_AGENT_RESULT_TIMEOUT;
		if (g_str_equal(err.name, OFONO_NAVIGATION_GOBACK))
			result = STK_AGENT_RESULT_BACK;
		else
			result = STK_AGENT_RESULT_TERMINATE;

		dbus_error_free(&err);
	}

	agent->cmd_cb(agent, result, reply);

	stk_agent_request_end(agent);

	dbus_message_unref(reply);

	if (result != STK_AGENT_RESULT_TERMINATE)
		return;

	if (agent->is_default)
		return;

	stk_agent_free(agent);
}

static gboolean stk_agent_request_send(gpointer user_data)
{
	struct stk_agent *agent = user_data;
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->cmd_send_source = 0;

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						agent->cmd_timeout) == FALSE ||
			agent->call == NULL) {
		ofono_error("Couldn't send a method call");

		stk_agent_request_terminate(agent);

		return FALSE;
	}

	dbus_pending_call_set_notify(agent->call,
					stk_agent_request_reply_handle,
					agent, NULL);

	return FALSE;
}

static gboolean stk_agent_request_start(struct stk_agent *agent,
					const char *method,
					stk_agent_request_return cb,
					stk_agent_generic_cb user_cb,
					void *user_data, int timeout)
{
	if (agent == NULL) {
		cb(agent, STK_AGENT_RESULT_TERMINATE, NULL);

		return FALSE;
	}

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							method);
	if (agent->msg == NULL) {
		ofono_error("Couldn't make a DBusMessage");

		cb(agent, STK_AGENT_RESULT_TERMINATE, NULL);

		return FALSE;
	}

	if (stk_agent_busy(agent))
		stk_agent_request_cancel(agent);

	agent->cmd_cb = cb;
	agent->cmd_timeout = timeout;
	agent->user_cb = user_cb;
	agent->user_data = user_data;

	agent->cmd_send_source = g_timeout_add(0, stk_agent_request_send,
						agent);

	return TRUE;
}

static void stk_agent_disconnect_cb(DBusConnection *conn, void *user_data)
{
	struct stk_agent *agent = user_data;

	ofono_debug("Agent exited without calling Unregister");

	agent->disconnect_watch = 0;

	stk_agent_free(agent);
}

struct stk_agent *stk_agent_new(const char *path, const char *sender,
				ofono_bool_t is_default)
{
	struct stk_agent *agent = g_try_new0(struct stk_agent, 1);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (!agent)
		return NULL;

	agent->path = g_strdup(path);
	agent->bus = g_strdup(sender);
	agent->is_default = is_default;

	agent->disconnect_watch = g_dbus_add_disconnect_watch(conn, sender,
							stk_agent_disconnect_cb,
							agent, NULL);

	return agent;
}

static void append_menu_items(DBusMessageIter *iter,
				const struct stk_menu_item *item)
{
	DBusMessageIter array, entry;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
						"(sy)", &array);

	for (; item->text; item++) {
		dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT,
							NULL, &entry);

		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
						&item->text);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_BYTE,
						&item->icon_id);

		dbus_message_iter_close_container(&array, &entry);
	}

	dbus_message_iter_close_container(iter, &array);
}

void append_menu_items_variant(DBusMessageIter *iter,
				const struct stk_menu_item *items)
{
	DBusMessageIter variant;

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						"a(sy)", &variant);

	append_menu_items(&variant, items);

	dbus_message_iter_close_container(iter, &variant);
}

static void request_selection_cb(struct stk_agent *agent,
					enum stk_agent_result result,
					DBusMessage *reply)
{
	const struct stk_menu *menu = agent->request_selection_menu;
	stk_agent_selection_cb cb = (stk_agent_selection_cb) agent->user_cb;
	unsigned char selection, i;

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, 0, agent->user_data);

		return;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_BYTE, &selection,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to RequestSelection()");

		cb(STK_AGENT_RESULT_TERMINATE, 0, agent->user_data);

		return;
	}

	for (i = 0; i < selection && menu->items[i].text; i++);

	if (i != selection) {
		ofono_error("Invalid item selected");

		cb(STK_AGENT_RESULT_TERMINATE, 0, agent->user_data);

		return;
	}

	cb(result, menu->items[selection].item_id, agent->user_data);
}

void stk_agent_request_selection(struct stk_agent *agent,
					const struct stk_menu *menu,
					stk_agent_selection_cb cb,
					void *user_data, int timeout)
{
	dbus_int16_t default_item = menu->default_item;
	DBusMessageIter iter;

	if (!stk_agent_request_start(agent, "RequestSelection",
					request_selection_cb,
					(stk_agent_generic_cb) cb,
					user_data, timeout))
		return;

	dbus_message_iter_init_append(agent->msg, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &menu->title);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE, &menu->icon_id);
	append_menu_items(&iter, menu->items);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT16, &default_item);

	agent->request_selection_menu = menu;
}

static void display_text_cb(struct stk_agent *agent,
				enum stk_agent_result result,
				DBusMessage *reply)
{
	stk_agent_generic_cb cb = agent->user_cb;

	if (result == STK_AGENT_RESULT_OK && dbus_message_get_args(
				reply, NULL, DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to DisplayText()");

		result = STK_AGENT_RESULT_TERMINATE;
	}

	cb(result, agent->user_data);
}

void stk_agent_display_text(struct stk_agent *agent, const char *text,
				uint8_t icon_id, ofono_bool_t urgent,
				ofono_bool_t ack, stk_agent_generic_cb cb,
				void *user_data, int timeout)
{
	dbus_bool_t priority = urgent;
	dbus_bool_t confirm = ack;

	if (!stk_agent_request_start(agent, "DisplayText", display_text_cb,
					cb, user_data, timeout))
		return;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon_id,
					DBUS_TYPE_BOOLEAN, &priority,
					DBUS_TYPE_BOOLEAN, &confirm,
					DBUS_TYPE_INVALID);
}
