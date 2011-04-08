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
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"
#include "smsutil.h"
#include "stkutil.h"
#include "stkagent.h"

enum allowed_error {
	ALLOWED_ERROR_GO_BACK	= 0x1,
	ALLOWED_ERROR_TERMINATE	= 0x2,
	ALLOWED_ERROR_BUSY		= 0x4,
};

struct stk_agent {
	char *path;				/* Agent Path */
	char *bus;				/* Agent bus */
	guint disconnect_watch;			/* DBus disconnect watch */
	ofono_bool_t remove_on_terminate;
	ofono_destroy_func removed_cb;
	void *removed_data;
	DBusMessage *msg;
	DBusPendingCall *call;
	void *user_cb;
	void *user_data;
	ofono_destroy_func user_destroy;

	const struct stk_menu *request_selection_menu;
};

#define ERROR_PREFIX OFONO_SERVICE ".Error"
#define GOBACK_ERROR ERROR_PREFIX ".GoBack"
#define TERMINATE_ERROR ERROR_PREFIX ".EndSession"
#define BUSY_ERROR ERROR_PREFIX ".Busy"

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
	if (agent->msg) {
		dbus_message_unref(agent->msg);
		agent->msg = NULL;
	}

	if (agent->call) {
		dbus_pending_call_unref(agent->call);
		agent->call = NULL;
	}

	if (agent->user_destroy)
		agent->user_destroy(agent->user_data);

	agent->user_destroy = NULL;
	agent->user_data = NULL;
	agent->user_cb = NULL;
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
	if (agent->call == NULL)
		return;

	dbus_pending_call_cancel(agent->call);

	if (agent->disconnect_watch)
		stk_agent_send_cancel(agent);

	stk_agent_request_end(agent);
}

void stk_agent_free(struct stk_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	stk_agent_request_cancel(agent);

	if (agent->disconnect_watch) {
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

static int check_error(struct stk_agent *agent, DBusMessage *reply,
				int allowed_errors,
				enum stk_agent_result *out_result)
{
	DBusError err;
	int result = 0;

	dbus_error_init(&err);

	if (dbus_set_error_from_message(&err, reply) == FALSE) {
		*out_result = STK_AGENT_RESULT_OK;
		return 0;
	}

	ofono_debug("SimToolkitAgent %s replied with error %s, %s",
			agent->path, err.name, err.message);

	/* Timeout is always valid */
	if (g_str_equal(err.name, DBUS_ERROR_NO_REPLY)) {
		/* Send a Cancel() to the agent since its taking too long */
		stk_agent_send_cancel(agent);
		*out_result = STK_AGENT_RESULT_TIMEOUT;
		goto out;
	}

	if ((allowed_errors & ALLOWED_ERROR_GO_BACK) &&
			g_str_equal(err.name, GOBACK_ERROR)) {
		*out_result = STK_AGENT_RESULT_BACK;
		goto out;
	}

	if ((allowed_errors & ALLOWED_ERROR_TERMINATE) &&
			g_str_equal(err.name, TERMINATE_ERROR)) {
		*out_result = STK_AGENT_RESULT_TERMINATE;
		goto out;
	}

	if ((allowed_errors & ALLOWED_ERROR_BUSY) &&
			g_str_equal(err.name, BUSY_ERROR)) {
		*out_result = STK_AGENT_RESULT_BUSY;
		goto out;
	}

	result = -EINVAL;

out:
	dbus_error_free(&err);
	return result;
}

static void stk_agent_disconnect_cb(DBusConnection *conn, void *user_data)
{
	struct stk_agent *agent = user_data;

	ofono_debug("Agent exited without calling Unregister");

	agent->disconnect_watch = 0;

	stk_agent_free(agent);
}

struct stk_agent *stk_agent_new(const char *path, const char *sender,
				ofono_bool_t remove_on_terminate)
{
	struct stk_agent *agent = g_try_new0(struct stk_agent, 1);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent == NULL)
		return NULL;

	agent->path = g_strdup(path);
	agent->bus = g_strdup(sender);
	agent->remove_on_terminate = remove_on_terminate;

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

	while (item && item->text) {
		dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT,
							NULL, &entry);

		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
						&item->text);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_BYTE,
						&item->icon_id);

		dbus_message_iter_close_container(&array, &entry);
		item++;
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

#define CALLBACK_END()						\
done:								\
	if (result == STK_AGENT_RESULT_TERMINATE &&		\
			agent->remove_on_terminate)		\
		remove_agent = TRUE;				\
	else							\
		remove_agent = FALSE;				\
								\
error:								\
	stk_agent_request_end(agent);				\
	dbus_message_unref(reply);				\
								\
	if (remove_agent)					\
		stk_agent_free(agent)				\

static void request_selection_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	const struct stk_menu *menu = agent->request_selection_menu;
	stk_agent_selection_cb cb = (stk_agent_selection_cb) agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	unsigned char selection, i;
	enum stk_agent_result result;
	gboolean remove_agent;

	if (check_error(agent, reply,
			ALLOWED_ERROR_GO_BACK | ALLOWED_ERROR_TERMINATE,
			&result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, 0, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_BYTE, &selection,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to RequestSelection()");
		remove_agent = TRUE;
		goto error;
	}

	for (i = 0; i < selection && menu->items[i].text; i++);

	if (i != selection) {
		ofono_error("Invalid item selected");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, menu->items[selection].item_id, agent->user_data);

	CALLBACK_END();
}

int stk_agent_request_selection(struct stk_agent *agent,
				const struct stk_menu *menu,
				stk_agent_selection_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_int16_t default_item = menu->default_item;
	DBusMessageIter iter;

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"RequestSelection");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(agent->msg, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &menu->title);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE, &menu->icon.id);
	append_menu_items(&iter, menu->items);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT16, &default_item);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	agent->request_selection_menu = menu;

	dbus_pending_call_set_notify(agent->call, request_selection_cb,
					agent, NULL);

	return 0;
}

static void display_text_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_display_text_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;

	if (check_error(agent, reply,
			ALLOWED_ERROR_GO_BACK | ALLOWED_ERROR_TERMINATE |
			ALLOWED_ERROR_BUSY, &result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to DisplayText()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, agent->user_data);

	CALLBACK_END();
}

int stk_agent_display_text(struct stk_agent *agent, const char *text,
				const struct stk_icon_id *icon,
				ofono_bool_t urgent,
				stk_agent_display_text_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t priority = urgent;

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"DisplayText");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_BOOLEAN, &priority,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, display_text_cb,
					agent, NULL);

	return 0;
}

static void get_confirmation_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_confirmation_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	dbus_bool_t confirm;

	if (check_error(agent, reply,
			ALLOWED_ERROR_GO_BACK | ALLOWED_ERROR_TERMINATE,
			&result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, FALSE, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_BOOLEAN, &confirm,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to GetConfirmation()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, confirm, agent->user_data);

	CALLBACK_END();
}

int stk_agent_request_confirmation(struct stk_agent *agent, const char *text,
					const struct stk_icon_id *icon,
					stk_agent_confirmation_cb cb,
					void *user_data,
					ofono_destroy_func destroy,
					int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"RequestConfirmation");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, get_confirmation_cb,
					agent, NULL);

	return 0;
}

static void get_digit_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_string_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	char *digit;

	if (check_error(agent, reply,
			ALLOWED_ERROR_GO_BACK | ALLOWED_ERROR_TERMINATE,
			&result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, NULL, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_STRING, &digit,
					DBUS_TYPE_INVALID) == FALSE ||
			strlen(digit) != 1 ||
			!valid_phone_number_format(digit)) {
		ofono_error("Can't parse the reply to GetDigit()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, digit, agent->user_data);

	CALLBACK_END();
}

int stk_agent_request_digit(struct stk_agent *agent, const char *text,
				const struct stk_icon_id *icon,
				stk_agent_string_cb cb, void *user_data,
				ofono_destroy_func destroy, int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"RequestDigit");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, get_digit_cb, agent, NULL);

	return 0;
}

static void get_key_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_string_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	char *key;

	if (check_error(agent, reply,
			ALLOWED_ERROR_GO_BACK | ALLOWED_ERROR_TERMINATE,
			&result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, NULL, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_STRING, &key,
					DBUS_TYPE_INVALID) == FALSE ||
			g_utf8_strlen(key, 10) != 1) {
		ofono_error("Can't parse the reply to GetKey()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, key, agent->user_data);

	CALLBACK_END();
}

int stk_agent_request_key(struct stk_agent *agent, const char *text,
				const struct stk_icon_id *icon,
				ofono_bool_t unicode_charset,
				stk_agent_string_cb cb, void *user_data,
				ofono_destroy_func destroy, int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"RequestKey");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, get_key_cb, agent, NULL);

	return 0;
}

static void get_digits_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_string_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	char *string;

	if (check_error(agent, reply,
			ALLOWED_ERROR_GO_BACK | ALLOWED_ERROR_TERMINATE,
			&result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, NULL, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_STRING, &string,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to GetDigits()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, string, agent->user_data);

	CALLBACK_END();
}

int stk_agent_request_digits(struct stk_agent *agent, const char *text,
				const struct stk_icon_id *icon,
				const char *default_text,
				int min, int max, ofono_bool_t hidden,
				stk_agent_string_cb cb, void *user_data,
				ofono_destroy_func destroy, int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	uint8_t min_val = min;
	uint8_t max_val = max;
	dbus_bool_t hidden_val = hidden;

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"RequestDigits");
	if (agent->msg == NULL)
		return -ENOMEM;

	if (default_text == NULL)
		default_text = "";

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_STRING, &default_text,
					DBUS_TYPE_BYTE, &min_val,
					DBUS_TYPE_BYTE, &max_val,
					DBUS_TYPE_BOOLEAN, &hidden_val,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, get_digits_cb, agent, NULL);

	return 0;
}

static void get_input_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_string_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	char *string;

	if (check_error(agent, reply,
			ALLOWED_ERROR_GO_BACK | ALLOWED_ERROR_TERMINATE,
			&result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, NULL, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_STRING, &string,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to GetInput()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, string, agent->user_data);

	CALLBACK_END();
}

int stk_agent_request_input(struct stk_agent *agent, const char *text,
				const struct stk_icon_id *icon,
				const char *default_text,
				ofono_bool_t unicode_charset, int min, int max,
				ofono_bool_t hidden, stk_agent_string_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	uint8_t min_val = min;
	uint8_t max_val = max;
	dbus_bool_t hidden_val = hidden;

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"RequestInput");
	if (agent->msg == NULL)
		return -ENOMEM;

	if (default_text == NULL)
		default_text = "";

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_STRING, &default_text,
					DBUS_TYPE_BYTE, &min_val,
					DBUS_TYPE_BYTE, &max_val,
					DBUS_TYPE_BOOLEAN, &hidden_val,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, get_input_cb, agent, NULL);

	return 0;
}

static void confirm_call_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_confirmation_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	dbus_bool_t confirm;

	if (check_error(agent, reply,
			ALLOWED_ERROR_TERMINATE, &result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, FALSE, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_BOOLEAN, &confirm,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to ConfirmCallSetup()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, confirm, agent->user_data);

	CALLBACK_END();
}

int stk_agent_confirm_call(struct stk_agent *agent, const char *text,
				const struct stk_icon_id *icon,
				stk_agent_confirmation_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"ConfirmCallSetup");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, confirm_call_cb, agent, NULL);

	return 0;
}

static void play_tone_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_tone_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;

	if (check_error(agent, reply,
			ALLOWED_ERROR_TERMINATE, &result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to PlayTone()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, agent->user_data);
	goto done;

	CALLBACK_END();
}

int stk_agent_play_tone(struct stk_agent *agent, const char *text,
			const struct stk_icon_id *icon, ofono_bool_t vibrate,
			const char *tone, stk_agent_tone_cb cb, void *user_data,
			ofono_destroy_func destroy, int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"PlayTone");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &tone,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, play_tone_cb,
					agent, NULL);

	return 0;
}

int stk_agent_loop_tone(struct stk_agent *agent, const char *text,
			const struct stk_icon_id *icon, ofono_bool_t vibrate,
			const char *tone, stk_agent_tone_cb cb, void *user_data,
			ofono_destroy_func destroy, int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"LoopTone");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &tone,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
			agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, play_tone_cb,
					agent, NULL);

	return 0;
}

static void action_info_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;

	if (check_error(agent, reply, 0, &result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to DisplayActionInfo()");
		remove_agent = TRUE;
		goto error;
	}

	goto done;

	CALLBACK_END();
}

int stk_agent_display_action_info(struct stk_agent *agent, const char *text,
					const struct stk_icon_id *icon)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
						OFONO_SIM_APP_INTERFACE,
						"DisplayActionInformation");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						0) == FALSE ||
			agent->call == NULL)
		return -EIO;

	dbus_pending_call_set_notify(agent->call, action_info_cb, agent, NULL);

	return 0;
}

static void confirm_launch_browser_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_confirmation_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	dbus_bool_t confirm;

	if (check_error(agent, reply, 0, &result) == -EINVAL) {
		remove_agent = TRUE;
		cb(STK_AGENT_RESULT_TERMINATE, FALSE, agent->user_data);
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, FALSE, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_BOOLEAN, &confirm,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to ConfirmLaunchBrowser()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, confirm, agent->user_data);

	CALLBACK_END();
}

int stk_agent_confirm_launch_browser(struct stk_agent *agent, const char *text,
					unsigned char icon_id, const char *url,
					stk_agent_confirmation_cb cb,
					void *user_data,
					ofono_destroy_func destroy, int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"ConfirmLaunchBrowser");
	if (agent->msg == NULL)
		return -ENOMEM;

	if (url == NULL)
		url = "";

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon_id,
					DBUS_TYPE_STRING, &url,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
						agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, confirm_launch_browser_cb,
					agent, NULL);

	return 0;
}

static void display_action_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_display_action_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;

	if (check_error(agent, reply,
			ALLOWED_ERROR_TERMINATE, &result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to DisplayAction()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, agent->user_data);
	goto done;

	CALLBACK_END();
}

int stk_agent_display_action(struct stk_agent *agent,
					const char *text,
					const struct stk_icon_id *icon,
					stk_agent_display_action_cb cb,
					void *user_data,
					ofono_destroy_func destroy)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
						OFONO_SIM_APP_INTERFACE,
						"DisplayAction");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						0) == FALSE ||
						agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, display_action_cb,
					agent, NULL);

	return 0;
}

static void confirm_open_channel_cb(DBusPendingCall *call, void *data)
{
	struct stk_agent *agent = data;
	stk_agent_confirmation_cb cb = agent->user_cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum stk_agent_result result;
	gboolean remove_agent;
	dbus_bool_t confirm;

	if (check_error(agent, reply,
			ALLOWED_ERROR_TERMINATE, &result) == -EINVAL) {
		remove_agent = TRUE;
		goto error;
	}

	if (result != STK_AGENT_RESULT_OK) {
		cb(result, FALSE, agent->user_data);
		goto done;
	}

	if (dbus_message_get_args(reply, NULL,
					DBUS_TYPE_BOOLEAN, &confirm,
					DBUS_TYPE_INVALID) == FALSE) {
		ofono_error("Can't parse the reply to ConfirmOpenChannel()");
		remove_agent = TRUE;
		goto error;
	}

	cb(result, confirm, agent->user_data);

	CALLBACK_END();
}

int stk_agent_confirm_open_channel(struct stk_agent *agent, const char *text,
					const struct stk_icon_id *icon,
					stk_agent_confirmation_cb cb,
					void *user_data,
					ofono_destroy_func destroy, int timeout)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
							OFONO_SIM_APP_INTERFACE,
							"ConfirmOpenChannel");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_append_args(agent->msg,
					DBUS_TYPE_STRING, &text,
					DBUS_TYPE_BYTE, &icon->id,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(conn, agent->msg, &agent->call,
						timeout) == FALSE ||
						agent->call == NULL)
		return -EIO;

	agent->user_cb = cb;
	agent->user_data = user_data;
	agent->user_destroy = destroy;

	dbus_pending_call_set_notify(agent->call, confirm_open_channel_cb,
					agent, NULL);

	return 0;
}
