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

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "util.h"

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
	struct extern_req *extern_req;
	char *idle_mode_text;
	struct stk_icon_id idle_mode_icon;
	struct timeval get_inkey_start_ts;
	int dtmf_id;

	__ofono_sms_sim_download_cb_t sms_pp_cb;
	void *sms_pp_userdata;
};

struct envelope_op {
	uint8_t tlv[256];
	unsigned int tlv_len;
	int retries;
	void (*cb)(struct ofono_stk *stk, gboolean ok,
			const unsigned char *data, int length);
};

struct extern_req {
	struct ofono_stk *stk;
	gboolean cancelled;
};

#define ENVELOPE_RETRIES_DEFAULT 5

static void envelope_queue_run(struct ofono_stk *stk);
static void timers_update(struct ofono_stk *stk);

#define ADD_ERROR_RESULT(result, error, addn_info)		\
		result.type = error;				\
		result.additional_len = sizeof(addn_info);	\
		result.additional = addn_info;			\

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
	if (tlv == NULL)
		return -EINVAL;

	stk_command_free(stk->pending_cmd);
	stk->pending_cmd = NULL;
	stk->cancel_cmd = NULL;
	stk->respond_on_exit = FALSE;

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
	if (tlv == NULL)
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

static void stk_sms_download_cb(struct ofono_stk *stk, gboolean ok,
				const unsigned char *data, int len)
{
	DBG("SMS-PP download to UICC reported %s", ok ? "success" : "error");

	if (stk->sms_pp_cb)
		stk->sms_pp_cb(ok, data, len, stk->sms_pp_userdata);
}

int __ofono_sms_sim_download(struct ofono_stk *stk, const struct sms *msg,
				__ofono_sms_sim_download_cb_t cb, void *data)
{
	struct stk_envelope e;

	if (msg->type != SMS_TYPE_DELIVER)
		return -EINVAL;

	DBG("");

	memset(&e, 0, sizeof(e));

	e.type = STK_ENVELOPE_TYPE_SMS_PP_DOWNLOAD;
	e.src = STK_DEVICE_IDENTITY_TYPE_NETWORK;

	e.sms_pp_download.address.number = (char *) msg->sc_addr.address;
	e.sms_pp_download.address.ton_npi = msg->sc_addr.numbering_plan |
		(msg->sc_addr.number_type << 4);
	memcpy(&e.sms_pp_download.message, &msg->deliver, sizeof(msg->deliver));

	stk->sms_pp_cb = cb;
	stk->sms_pp_userdata = data;

	return stk_send_envelope(stk, &e, stk_sms_download_cb,
					ENVELOPE_RETRIES_DEFAULT);
}

static char *dbus_apply_text_attributes(const char *text,
					const struct stk_text_attribute *attr)
{
	uint16_t buf[256], *i = buf;
	const uint8_t *j = attr->attributes;
	const uint8_t *end = j + attr->len;

	if (text == NULL)
		return NULL;

	if (attr->len & 3)
		return NULL;

	while (j < end)
		*i++ = *j++;

	return stk_text_to_html(text, buf, attr->len / 4);
}

static struct stk_menu *stk_menu_create(const char *title,
		const struct stk_text_attribute *title_attr,
		const struct stk_icon_id *icon, GSList *items,
		const struct stk_item_text_attribute_list *item_attrs,
		const struct stk_item_icon_id_list *item_icon_ids,
		int default_id, gboolean soft_key, gboolean has_help)
{
	unsigned int len = g_slist_length(items);
	struct stk_menu *ret;
	GSList *l;
	int i;
	struct stk_text_attribute attr;

	DBG("");

	if (item_attrs && item_attrs->len && item_attrs->len != len * 4)
		return NULL;

	if (item_icon_ids && item_icon_ids->len && item_icon_ids->len != len)
		return NULL;

	ret = g_try_new(struct stk_menu, 1);
	if (ret == NULL)
		return NULL;

	ret->title = dbus_apply_text_attributes(title ? title : "",
						title_attr);
	if (ret->title == NULL)
		ret->title = g_strdup(title ? title : "");

	memcpy(&ret->icon, icon, sizeof(ret->icon));
	ret->items = g_new0(struct stk_menu_item, len + 1);
	ret->default_item = -1;
	ret->soft_key = soft_key;
	ret->has_help = has_help;

	for (l = items, i = 0; l; l = l->next, i++) {
		struct stk_item *item = l->data;
		char *text;

		ret->items[i].item_id = item->id;

		text = NULL;

		if (item_attrs && item_attrs->len) {
			memcpy(attr.attributes, &item_attrs->list[i * 4], 4);
			attr.len = 4;

			text = dbus_apply_text_attributes(item->text, &attr);
		}

		if (text == NULL)
			text = strdup(item->text);

		ret->items[i].text = text;

		if (item_icon_ids && item_icon_ids->len)
			ret->items[i].icon_id = item_icon_ids->list[i];

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
				&cmd->setup_menu.icon_id,
				cmd->setup_menu.items,
				&cmd->setup_menu.item_text_attr_list,
				&cmd->setup_menu.item_icon_id_list,
				0, soft_key, has_help);
}

static struct stk_menu *stk_menu_create_from_select_item(
						const struct stk_command *cmd)
{
	gboolean soft_key = (cmd->qualifier & (1 << 2)) != 0;
	gboolean has_help = (cmd->qualifier & (1 << 7)) != 0;

	return stk_menu_create(cmd->select_item.alpha_id,
				&cmd->select_item.text_attr,
				&cmd->select_item.icon_id,
				cmd->select_item.items,
				&cmd->select_item.item_text_attr_list,
				&cmd->select_item.item_icon_id_list,
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

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_STK_INTERFACE,
						"MainMenuIcon",
						DBUS_TYPE_BYTE, &menu->icon.id);

	signal = dbus_message_new_signal(path, OFONO_STK_INTERFACE,
						"PropertyChanged");
	if (signal == NULL) {
		ofono_error("Unable to allocate new %s.PropertyChanged signal",
				OFONO_SIM_APP_INTERFACE);

		return;
	}

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

	append_menu_items_variant(&iter, menu->items);

	g_dbus_send_message(conn, signal);
}

static void cancel_pending_dtmf(struct ofono_stk *stk)
{
	struct ofono_voicecall *vc;

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL,
				__ofono_atom_get_modem(stk->atom));

	if (vc) /* Should be always true here */
		__ofono_voicecall_tone_cancel(vc, stk->dtmf_id);
}

static void user_termination_cb(enum stk_agent_result result, void *user_data)
{
	struct ofono_stk *stk = user_data;

	if (result != STK_AGENT_RESULT_TERMINATE)
		return;

	switch (stk->pending_cmd->type) {
	case STK_COMMAND_TYPE_SEND_DTMF:
		cancel_pending_dtmf(stk);
		break;
	}

	send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
}

static gboolean stk_alpha_id_set(struct ofono_stk *stk,
			const char *text, const struct stk_text_attribute *attr,
			const struct stk_icon_id *icon)
{
	char *alpha = dbus_apply_text_attributes(text, attr);

	/*
	 * Currently, we are treating null data object(len = 0, no value part)
	 * and no alpha identifier cases equally. This may be changed once
	 * better idea is found out.
	 */
	if (alpha == NULL)
		return FALSE;

	if (stk->respond_on_exit)
		stk_agent_display_action(stk->current_agent, alpha, icon,
						user_termination_cb, stk, NULL);
	else
		stk_agent_display_action_info(stk->current_agent, alpha, icon);

	g_free(alpha);

	return TRUE;
}

static void stk_alpha_id_unset(struct ofono_stk *stk)
{
	/*
	 * If there is no default agent, then current agent also will be NULL.
	 * So, call request cancel only when there is a valid current agent.
	 */
	if (stk->current_agent)
		stk_agent_request_cancel(stk->current_agent);
}

static int duration_to_msecs(const struct stk_duration *duration)
{
	int msecs = duration->interval;

	switch (duration->unit) {
	case STK_DURATION_TYPE_MINUTES:
		msecs *= 60;
		/* Fall through.  */
	case STK_DURATION_TYPE_SECONDS:
		msecs *= 10;
		/* Fall through.  */
	case STK_DURATION_TYPE_SECOND_TENTHS:
		msecs *= 100;
	}

	return msecs;
}

static DBusMessage *stk_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_stk *stk = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessageIter entry;
	const char *key = "MainMenu";
	const char *str;
	unsigned char icon;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	str = stk->idle_mode_text;
	ofono_dbus_dict_append(&dict, "IdleModeText", DBUS_TYPE_STRING, &str);

	icon = stk->idle_mode_icon.id;
	ofono_dbus_dict_append(&dict, "IdleModeIcon", DBUS_TYPE_BYTE, &icon);

	str = stk->main_menu ? stk->main_menu->title : "";
	ofono_dbus_dict_append(&dict, "MainMenuTitle", DBUS_TYPE_STRING, &str);

	icon = stk->main_menu ? stk->main_menu->icon.id : 0;
	ofono_dbus_dict_append(&dict, "MainMenuIcon", DBUS_TYPE_BYTE, &icon);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	append_menu_items_variant(&entry,
				stk->main_menu ? stk->main_menu->items : NULL);

	dbus_message_iter_close_container(&dict, &entry);
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

	if (stk->current_agent == stk->default_agent && stk->respond_on_exit) {
		if (stk->pending_cmd)
			stk->cancel_cmd(stk);

		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
	}

	stk->default_agent = NULL;
	stk->current_agent = stk->session_agent;
}

static void session_agent_notify(gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	DBG("Session Agent removed");

	if (stk->current_agent == stk->session_agent && stk->respond_on_exit) {
		if (stk->pending_cmd)
			stk->cancel_cmd(stk);

		DBG("Sending Terminate response for session agent");
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
	}

	stk->session_agent = NULL;
	stk->current_agent = stk->default_agent;

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
	if (stk->default_agent == NULL)
		return __ofono_error_failed(msg);

	stk_agent_set_removed_notify(stk->default_agent,
					default_agent_notify, stk);

	if (stk->session_agent == NULL)
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

	if (stk->default_agent == NULL)
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
	if (stk->session_agent == NULL) {
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

	if (menu == NULL)
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

	stk->pending = dbus_message_ref(msg);

	if (stk_send_envelope(stk, &e, menu_selection_envelope_cb, 0))
		__ofono_dbus_pending_reply(&stk->pending,
					__ofono_error_failed(stk->pending));

	return NULL;
}

static const GDBusMethodTable stk_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			stk_get_properties) },
	{ GDBUS_ASYNC_METHOD("SelectItem",
			GDBUS_ARGS({ "item", "y" }, { "agent", "o" }), NULL,
			stk_select_item) },
	{ GDBUS_METHOD("RegisterAgent",
			GDBUS_ARGS({ "path", "o" }), NULL,
			stk_register_agent) },
	{ GDBUS_METHOD("UnregisterAgent",
			GDBUS_ARGS({ "path", "o" }), NULL,
			stk_unregister_agent) },
	{ }
};

static const GDBusSignalTable stk_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
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
	stk->extern_req->cancelled = TRUE;

	stk_alpha_id_unset(stk);
}

static void send_sms_submit_cb(gboolean ok, void *data)
{
	struct extern_req *req = data;
	struct ofono_stk *stk = req->stk;
	struct ofono_error failure = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;

	DBG("SMS submission %s", ok ? "successful" : "failed");

	if (req->cancelled) {
		DBG("Received an SMS submitted callback after the "
				"proactive command was cancelled");
		return;
	}

	stk_alpha_id_unset(stk);

	memset(&rsp, 0, sizeof(rsp));

	if (ok == FALSE)
		rsp.result.type = STK_RESULT_TYPE_NETWORK_UNAVAILABLE;

	if (stk_respond(stk, &rsp, stk_command_cb))
		stk_command_cb(&failure, stk);
}

static void extern_req_start(struct ofono_stk *stk)
{
	stk->extern_req = g_new0(struct extern_req, 1);
	stk->extern_req->stk = stk;
}

static gboolean handle_command_send_sms(const struct stk_command *cmd,
					struct stk_response *rsp,
					struct ofono_stk *stk)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	struct ofono_sms *sms;
	GSList msg_list;
	struct ofono_uuid uuid;

	sms = __ofono_atom_find(OFONO_ATOM_TYPE_SMS, modem);

	if (sms == NULL) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	extern_req_start(stk);

	msg_list.data = (void *) &cmd->send_sms.gsm_sms;
	msg_list.next = NULL;

	if (__ofono_sms_txq_submit(sms, &msg_list, 0, &uuid, NULL, NULL) < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
		return TRUE;
	}

	__ofono_sms_txq_set_submit_notify(sms, &uuid, send_sms_submit_cb,
						stk->extern_req, g_free);
	stk->cancel_cmd = send_sms_cancel;

	stk_alpha_id_set(stk, cmd->send_sms.alpha_id, &cmd->send_sms.text_attr,
				&cmd->send_sms.icon_id);

	return FALSE;
}

/* Note: may be called from ofono_stk_proactive_command_handled_notify */
static gboolean handle_command_set_idle_text(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(stk->atom);
	char *idle_mode_text;

	idle_mode_text = dbus_apply_text_attributes(
					cmd->setup_idle_mode_text.text,
					&cmd->setup_idle_mode_text.text_attr);

	if (idle_mode_text == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	if (stk->idle_mode_text)
		g_free(stk->idle_mode_text);

	stk->idle_mode_text = idle_mode_text;

	ofono_dbus_signal_property_changed(conn, path, OFONO_STK_INTERFACE,
						"IdleModeText",
						DBUS_TYPE_STRING,
						&idle_mode_text);

	if (stk->idle_mode_icon.id != cmd->setup_idle_mode_text.icon_id.id) {
		memcpy(&stk->idle_mode_icon, &cmd->setup_idle_mode_text.icon_id,
				sizeof(stk->idle_mode_icon));

		ofono_dbus_signal_property_changed(conn, path,
						OFONO_STK_INTERFACE,
						"IdleModeIcon", DBUS_TYPE_BYTE,
						&stk->idle_mode_icon.id);
	}

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

	if (!cmd->poll_interval.duration.interval) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	seconds = MAX(duration_to_msecs(&cmd->poll_interval.duration) / 1000,
			1);

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

/* Note: may be called from ofono_stk_proactive_command_handled_notify */
static gboolean handle_command_set_up_menu(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	struct stk_menu *menu = NULL;

	if (cmd->setup_menu.items) {
		menu = stk_menu_create_from_set_up_menu(cmd);

		if (menu == NULL) {
			rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
			return TRUE;
		}
	}

	if (menu == NULL && stk->main_menu == NULL)
		return TRUE;

	if (stk->main_menu)
		stk_menu_free(stk->main_menu);

	stk->main_menu = menu;

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

	switch (result) {
	case STK_AGENT_RESULT_OK:
	{
		static struct ofono_error error = {
			.type = OFONO_ERROR_TYPE_FAILURE
		};
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

	if (stk->select_item_menu == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;

		return TRUE;
	}

	/* We most likely got an out of memory error, tell SIM to retry */
	if (stk_agent_request_selection(stk->current_agent,
					stk->select_item_menu,
					request_selection_cb, stk,
					request_selection_destroy,
					stk->timeout * 1000) < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		request_selection_destroy(stk);

		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
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
	struct stk_response rsp;
	static unsigned char screen_busy_result[] = { 0x01 };
	static struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };

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

	case STK_AGENT_RESULT_BUSY:
		memset(&rsp, 0, sizeof(rsp));
		ADD_ERROR_RESULT(rsp.result, STK_RESULT_TYPE_TERMINAL_BUSY,
					screen_busy_result);
		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);
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
	char *text = dbus_apply_text_attributes(dt->text, &dt->text_attr);
	int err;

	if (text == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	if (qualifier & (1 << 7))
		timeout = stk->timeout * 1000;

	if (dt->duration.interval)
		timeout = duration_to_msecs(&dt->duration);

	err = stk_agent_display_text(stk->current_agent, text, &dt->icon_id,
					priority, display_text_cb, stk,
					display_text_destroy, timeout);
	g_free(text);

	/* We most likely got an out of memory error, tell SIM to retry */
	if (err < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
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
		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_NO_RESPONSE;

		if (cmd->duration.interval) {
			rsp.get_inkey.duration.unit = cmd->duration.unit;
			set_get_inkey_duration(&rsp.get_inkey.duration,
						&stk->get_inkey_start_ts);
		}

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		break;

	case STK_AGENT_RESULT_TERMINATE:
	default:
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
		memset(&rsp, 0, sizeof(rsp));

		rsp.result.type = STK_RESULT_TYPE_NO_RESPONSE;

		if (cmd->duration.interval) {
			rsp.get_inkey.duration.unit = cmd->duration.unit;
			set_get_inkey_duration(&rsp.get_inkey.duration,
						&stk->get_inkey_start_ts);
		}

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		break;

	case STK_AGENT_RESULT_TERMINATE:
	default:
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
	char *text = dbus_apply_text_attributes(gi->text, &gi->text_attr);
	uint8_t qualifier = stk->pending_cmd->qualifier;
	gboolean alphabet = (qualifier & (1 << 0)) != 0;
	gboolean ucs2 = (qualifier & (1 << 1)) != 0;
	gboolean yesno = (qualifier & (1 << 2)) != 0;
	/*
	 * Note: immediate response and help parameter values are not
	 * provided by current api.
	 */
	int err;

	if (text == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	if (gi->duration.interval)
		timeout = duration_to_msecs(&gi->duration);

	gettimeofday(&stk->get_inkey_start_ts, NULL);

	if (yesno)
		err = stk_agent_request_confirmation(stk->current_agent,
							text, &gi->icon_id,
							request_confirmation_cb,
							stk, NULL, timeout);
	else if (alphabet)
		err = stk_agent_request_key(stk->current_agent, text,
						&gi->icon_id, ucs2,
						request_key_cb, stk, NULL,
						timeout);
	else
		err = stk_agent_request_digit(stk->current_agent, text,
						&gi->icon_id, request_key_cb,
						stk, NULL, timeout);

	g_free(text);

	if (err < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
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
	default:
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
	char *text = dbus_apply_text_attributes(gi->text, &gi->text_attr);
	uint8_t qualifier = stk->pending_cmd->qualifier;
	gboolean alphabet = (qualifier & (1 << 0)) != 0;
	gboolean ucs2 = (qualifier & (1 << 1)) != 0;
	gboolean hidden = (qualifier & (1 << 2)) != 0;
	int err;

	if (text == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	if (alphabet)
		err = stk_agent_request_input(stk->current_agent, text,
						&gi->icon_id, gi->default_text,
						ucs2, gi->resp_len.min,
						gi->resp_len.max, hidden,
						request_string_cb,
						stk, NULL, timeout);
	else
		err = stk_agent_request_digits(stk->current_agent, text,
						&gi->icon_id, gi->default_text,
						gi->resp_len.min,
						gi->resp_len.max, hidden,
						request_string_cb,
						stk, NULL, timeout);

	g_free(text);

	if (err < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
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

	if (call == NULL || call->status == CALL_STATUS_DISCONNECTED) {
		memset(&rsp, 0, sizeof(rsp));

		ADD_ERROR_RESULT(rsp.result,
					STK_RESULT_TYPE_NETWORK_UNAVAILABLE,
					facility_rejected_result);

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

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL,
				__ofono_atom_get_modem(stk->atom));

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
	char *alpha_id = NULL;
	struct ofono_voicecall *vc;
	struct stk_response rsp;
	int err;

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

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL,
				__ofono_atom_get_modem(stk->atom));
	if (vc == NULL) {
		send_simple_response(stk, STK_RESULT_TYPE_NOT_CAPABLE);
		return;
	}

	if (sc->alpha_id_call_setup) {
		alpha_id = dbus_apply_text_attributes(sc->alpha_id_call_setup,
						&sc->text_attr_call_setup);
		if (alpha_id == NULL) {
			send_simple_response(stk,
					STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD);
			return;
		}
	}

	err = __ofono_voicecall_dial(vc, sc->addr.number, sc->addr.ton_npi,
					alpha_id, sc->icon_id_call_setup.id,
					qualifier >> 1, call_setup_connected,
					stk);
	g_free(alpha_id);

	if (err >= 0) {
		stk->cancel_cmd = call_setup_cancel;

		return;
	}

	if (err == -EBUSY) {
		memset(&rsp, 0, sizeof(rsp));

		ADD_ERROR_RESULT(rsp.result, STK_RESULT_TYPE_TERMINAL_BUSY,
					busy_on_call_result);

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&error, stk);

		return;
	}

	if (err == -ENOSYS) {
		send_simple_response(stk, STK_RESULT_TYPE_NOT_CAPABLE);

		return;
	}

	memset(&rsp, 0, sizeof(rsp));

	ADD_ERROR_RESULT(rsp.result, STK_RESULT_TYPE_NETWORK_UNAVAILABLE,
				no_cause_result);

	if (stk_respond(stk, &rsp, stk_command_cb))
		stk_command_cb(&error, stk);
}

static void confirm_handled_call_cb(enum stk_agent_result result,
					gboolean confirm, void *user_data)
{
	struct ofono_stk *stk = user_data;
	const struct stk_command_setup_call *sc =
					&stk->pending_cmd->setup_call;
	struct ofono_voicecall *vc;

	if (stk->driver->user_confirmation == NULL)
		goto out;

	if (result != STK_AGENT_RESULT_OK) {
		stk->driver->user_confirmation(stk, FALSE);
		goto out;
	}

	stk->driver->user_confirmation(stk, confirm);

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL,
				__ofono_atom_get_modem(stk->atom));
	if (vc == NULL)
		goto out;

	__ofono_voicecall_set_alpha_and_icon_id(vc, sc->addr.number,
						sc->addr.ton_npi,
						sc->alpha_id_call_setup,
						sc->icon_id_call_setup.id);

	return;

out:
	stk_command_free(stk->pending_cmd);
	stk->pending_cmd = NULL;
}

static gboolean handle_command_set_up_call(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	const struct stk_command_setup_call *sc = &cmd->setup_call;
	uint8_t qualifier = cmd->qualifier;
	static unsigned char busy_on_call_result[] = { 0x02 };
	char *alpha_id = NULL;
	struct ofono_voicecall *vc = NULL;
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

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL,
				__ofono_atom_get_modem(stk->atom));
	if (vc == NULL) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	if (__ofono_voicecall_is_busy(vc, qualifier >> 1)) {
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					busy_on_call_result);
		return TRUE;
	}

	alpha_id = dbus_apply_text_attributes(sc->alpha_id_usr_cfm ?
						sc->alpha_id_usr_cfm : "",
						&sc->text_attr_usr_cfm);
	if (alpha_id == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	err = stk_agent_confirm_call(stk->current_agent, alpha_id,
					&sc->icon_id_usr_cfm, confirm_call_cb,
					stk, NULL, stk->timeout * 1000);
	g_free(alpha_id);

	if (err < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
		return TRUE;
	}

	stk->respond_on_exit = TRUE;
	stk->cancel_cmd = stk_request_cancel;

	return FALSE;
}

static void send_ussd_cancel(struct ofono_stk *stk)
{
	struct ofono_ussd *ussd;

	ussd = __ofono_atom_find(OFONO_ATOM_TYPE_USSD,
					__ofono_atom_get_modem(stk->atom));
	if (ussd)
		__ofono_ussd_initiate_cancel(ussd);

	stk_alpha_id_unset(stk);
}

static void send_ussd_callback(int error, int dcs, const unsigned char *msg,
				int msg_len, void *userdata)
{
	struct ofono_stk *stk = userdata;
	struct ofono_error failure = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;
	enum sms_charset charset;
	unsigned char no_cause[] = { 0x00 };

	stk_alpha_id_unset(stk);

	memset(&rsp, 0, sizeof(rsp));

	switch (error) {
	case 0:
		if (cbs_dcs_decode(dcs, NULL, NULL, &charset,
					NULL, NULL, NULL)) {
			if (charset == SMS_CHARSET_7BIT)
				rsp.send_ussd.text.dcs = 0x00;
			else if (charset == SMS_CHARSET_8BIT)
				rsp.send_ussd.text.dcs = 0x04;
			else if (charset == SMS_CHARSET_UCS2)
				rsp.send_ussd.text.dcs = 0x08;

			rsp.result.type = STK_RESULT_TYPE_SUCCESS;
			rsp.send_ussd.text.text = msg;
			rsp.send_ussd.text.len = msg_len;
			rsp.send_ussd.text.has_text = TRUE;
		} else
			rsp.result.type = STK_RESULT_TYPE_USSD_RETURN_ERROR;

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&failure, stk);

		break;

	case -ECANCELED:
		send_simple_response(stk,
				STK_RESULT_TYPE_USSD_OR_SS_USER_TERMINATION);
		break;

	case -ETIMEDOUT:
		send_simple_response(stk, STK_RESULT_TYPE_NETWORK_UNAVAILABLE);
		break;

	default:
		ADD_ERROR_RESULT(rsp.result, STK_RESULT_TYPE_USSD_RETURN_ERROR,
					no_cause);

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&failure, stk);

		break;
	}
}

static gboolean ss_is_busy(struct ofono_modem *modem)
{
	struct ofono_atom *atom;

	atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_CALL_FORWARDING);
	if (atom != NULL) {
		struct ofono_call_forwarding *cf = __ofono_atom_get_data(atom);

		if (__ofono_call_forwarding_is_busy(cf))
			return TRUE;
	}

	atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_CALL_BARRING);
	if (atom != NULL) {
		struct ofono_call_barring *cb = __ofono_atom_get_data(atom);

		if (__ofono_call_barring_is_busy(cb))
			return TRUE;
	}

	atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_CALL_SETTINGS);
	if (atom != NULL) {
		struct ofono_call_settings *cs = __ofono_atom_get_data(atom);

		if (__ofono_call_settings_is_busy(cs))
			return TRUE;
	}

	return FALSE;
}

static gboolean handle_command_send_ussd(const struct stk_command *cmd,
					struct stk_response *rsp,
					struct ofono_stk *stk)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	static unsigned char busy_on_ss_result[] = { 0x03 };
	static unsigned char busy_on_ussd_result[] = { 0x08 };
	struct ofono_ussd *ussd;
	int err;

	ussd = __ofono_atom_find(OFONO_ATOM_TYPE_USSD, modem);
	if (ussd == NULL) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	if (__ofono_ussd_is_busy(ussd)) {
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					busy_on_ussd_result);
		return TRUE;
	}

	if (ss_is_busy(modem)) {
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					busy_on_ss_result);
		return TRUE;
	}

	err = __ofono_ussd_initiate(ussd, cmd->send_ussd.ussd_string.dcs,
					cmd->send_ussd.ussd_string.string,
					cmd->send_ussd.ussd_string.len,
					send_ussd_callback, stk);

	if (err >= 0) {
		stk->cancel_cmd = send_ussd_cancel;

		return FALSE;
	}

	if (err == -ENOSYS) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	if (err == -EBUSY) {
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					busy_on_ussd_result);
		return TRUE;
	}

	stk_alpha_id_set(stk, cmd->send_ussd.alpha_id,
				&cmd->send_ussd.text_attr,
				&cmd->send_ussd.icon_id);

	return FALSE;
}

static void free_idle_mode_text(struct ofono_stk *stk)
{
	g_free(stk->idle_mode_text);
	stk->idle_mode_text = NULL;

	memset(&stk->idle_mode_icon, 0, sizeof(stk->idle_mode_icon));
}

/* Note: may be called from ofono_stk_proactive_command_handled_notify */
static gboolean handle_command_refresh(const struct stk_command *cmd,
					struct stk_response *rsp,
					struct ofono_stk *stk)
{
	struct ofono_error failure = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	struct ofono_sim *sim;
	uint8_t addnl_info[1];
	int err;
	GSList *l;

	DBG("");

	switch (cmd->qualifier) {
	case 0:
		DBG("NAA Initialization and "
				"Full File Change Notification");
		break;

	case 1:
		DBG("File Change Notification");
		break;

	case 2:
		DBG("NAA Initialization and File Change Notification");
		break;

	case 3:
		DBG("NAA Initialization");
		break;

	case 4:
		DBG("UICC Reset");
		break;

	case 5:
		DBG("NAA Application Reset");
		break;

	case 6:
		DBG("NAA Session Reset");
		break;

	default:
		ofono_info("Undefined Refresh qualifier: %d", cmd->qualifier);

		if (rsp != NULL)
			rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;

		return TRUE;
	}

	DBG("Files:");
	for (l = cmd->refresh.file_list; l; l = l->next) {
		struct stk_file *file = l->data;
		char buf[17];

		encode_hex_own_buf(file->file, file->len, 0, buf);
		DBG("%s", buf);
	}

	DBG("Icon: %d, qualifier: %d", cmd->refresh.icon_id.id,
					cmd->refresh.icon_id.qualifier);
	DBG("Alpha ID: %s", cmd->refresh.alpha_id);

	sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);
	if (sim == NULL) {
		if (rsp != NULL)
			rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;

		return TRUE;
	}

	if (rsp != NULL) {
		struct ofono_ussd *ussd;
		struct ofono_voicecall *vc;

		ussd = __ofono_atom_find(OFONO_ATOM_TYPE_USSD, modem);

		if (ussd && __ofono_ussd_is_busy(ussd)) {
			addnl_info[0] = STK_RESULT_ADDNL_ME_PB_USSD_BUSY;

			ADD_ERROR_RESULT(rsp->result,
					STK_RESULT_TYPE_TERMINAL_BUSY,
					addnl_info);
			return TRUE;
		}

		vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL, modem);

		if (vc && __ofono_voicecall_is_busy(vc,
					OFONO_VOICECALL_INTERACTION_NONE)) {
			addnl_info[0] = STK_RESULT_ADDNL_ME_PB_BUSY_ON_CALL;

			ADD_ERROR_RESULT(rsp->result,
					STK_RESULT_TYPE_TERMINAL_BUSY,
					addnl_info);
			return TRUE;
		}

		if (ss_is_busy(__ofono_atom_get_modem(stk->atom))) {
			addnl_info[0] = STK_RESULT_ADDNL_ME_PB_SS_BUSY;

			ADD_ERROR_RESULT(rsp->result,
					STK_RESULT_TYPE_TERMINAL_BUSY,
					addnl_info);
			return TRUE;
		}
	}

	/*
	 * For now we can handle the Refresh types that don't require
	 * a SIM reset except if that part of the task has been already
	 * handled by modem firmware (indicated by rsp == NULL) in which
	 * case we just restart our SIM initialisation.
	 */
	if (cmd->qualifier < 4 || rsp == NULL) {
		int qualifier = stk->pending_cmd->qualifier;
		GSList *file_list = stk->pending_cmd->refresh.file_list;

		/* Don't free the list yet */
		stk->pending_cmd->refresh.file_list = NULL;

		/*
		 * Queue the TERMINAL RESPONSE before triggering potential
		 * file accesses.
		 *
		 * TODO: Find out if we need to send the "Refresh performed
		 * with additional EFs read" response.
		 */
		if (rsp != NULL) {
			err = stk_respond(stk, rsp, stk_command_cb);
			if (err)
				stk_command_cb(&failure, stk);
		}

		/* TODO: use the alphaId / icon */
		/* TODO: if AID is supplied, check its value */
		/* TODO: possibly check if a D-bus call is pending or
		 * an STK session ongoing. */

		/* TODO: free some elements of the atom state */

		switch (qualifier) {
		case 0:
			free_idle_mode_text(stk);
			__ofono_sim_refresh(sim, file_list, TRUE, TRUE);
			break;
		case 1:
			__ofono_sim_refresh(sim, file_list, FALSE, FALSE);
			break;
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
			free_idle_mode_text(stk);
			__ofono_sim_refresh(sim, file_list, FALSE, TRUE);
			break;
		}

		g_slist_foreach(file_list, (GFunc) g_free, NULL);
		g_slist_free(file_list);

		return FALSE;
	}

	rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
	return TRUE;
}

static void get_time(struct stk_response *rsp)
{
	time_t now;
	struct tm *t;

	time(&now);
	t = localtime(&now);

	rsp->result.type = STK_RESULT_TYPE_SUCCESS;

	if (t->tm_year > 100)
		rsp->provide_local_info.datetime.year = t->tm_year - 100;
	else
		rsp->provide_local_info.datetime.year = t->tm_year;

	rsp->provide_local_info.datetime.month = t->tm_mon + 1;
	rsp->provide_local_info.datetime.day = t->tm_mday;
	rsp->provide_local_info.datetime.hour = t->tm_hour;
	rsp->provide_local_info.datetime.minute = t->tm_min;
	rsp->provide_local_info.datetime.second = t->tm_sec;
	rsp->provide_local_info.datetime.timezone = t->tm_gmtoff / 900;
	rsp->provide_local_info.datetime.has_timezone = TRUE;

	return;
}

static void get_lang(struct stk_response *rsp, struct ofono_stk *stk)
{
	char *l;
	char lang[3];
	struct ofono_error failure = { .type = OFONO_ERROR_TYPE_FAILURE };

	l = getenv("LANG");
	if (l == NULL) {
		l = "en";
		ofono_warn("LANG environment variable not set"
				" - defaulting to en");
	}

	memcpy(lang, l, 2);
	lang[2] = '\0';

	rsp->result.type = STK_RESULT_TYPE_SUCCESS;
	rsp->provide_local_info.language = lang;

	if (stk_respond(stk, rsp, stk_command_cb))
		stk_command_cb(&failure, stk);
}

static gboolean handle_command_provide_local_info(const struct stk_command *cmd,
				struct stk_response *rsp, struct ofono_stk *stk)
{
	switch (cmd->qualifier) {
	case 3:
		DBG("Date, time and time zone");
		get_time(rsp);
		return TRUE;

	case 4:
		DBG("Language setting");
		get_lang(rsp, stk);
		return FALSE;

	default:
		ofono_info("Unsupported Provide Local Info qualifier: %d",
				cmd->qualifier);
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}
}

static void send_dtmf_cancel(struct ofono_stk *stk)
{
	cancel_pending_dtmf(stk);
	stk_alpha_id_unset(stk);
}

static void dtmf_sent_cb(int error, void *user_data)
{
	struct ofono_stk *stk = user_data;

	stk_alpha_id_unset(stk);

	if (error == ENOENT) {
		struct stk_response rsp;
		static unsigned char not_in_speech_call_result[] = { 0x07 };
		static struct ofono_error failure = {
			.type = OFONO_ERROR_TYPE_FAILURE
		};

		memset(&rsp, 0, sizeof(rsp));

		ADD_ERROR_RESULT(rsp.result, STK_RESULT_TYPE_TERMINAL_BUSY,
					not_in_speech_call_result);

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&failure, stk);

		return;
	}

	if (error != 0)
		send_simple_response(stk, STK_RESULT_TYPE_NOT_CAPABLE);
	else
		send_simple_response(stk, STK_RESULT_TYPE_SUCCESS);
}

static gboolean handle_command_send_dtmf(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	static unsigned char not_in_speech_call_result[] = { 0x07 };
	struct ofono_voicecall *vc = NULL;
	char dtmf[256], *digit;
	char *dtmf_from = "01234567890abcABC";
	char *dtmf_to = "01234567890*#p*#p";
	int err, pos;

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL,
				__ofono_atom_get_modem(stk->atom));
	if (vc == NULL) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	/* Convert the DTMF string to phone number format */
	for (pos = 0; cmd->send_dtmf.dtmf[pos] != '\0'; pos++) {
		digit = strchr(dtmf_from, cmd->send_dtmf.dtmf[pos]);
		if (digit == NULL) {
			rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
			return TRUE;
		}

		dtmf[pos] = dtmf_to[digit - dtmf_from];
	}

	dtmf[pos] = '\0';

	err = __ofono_voicecall_tone_send(vc, dtmf, dtmf_sent_cb, stk);

	if (err == -ENOSYS) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	if (err == -ENOENT) {
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					not_in_speech_call_result);
		return TRUE;
	}

	if (err == -EINVAL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	if (err < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
		return TRUE;
	}

	/*
	 * Note that we don't strictly require an agent to be connected,
	 * but to comply with 6.4.24 we need to send a End Session when
	 * the user decides so.
	 */
	stk->respond_on_exit = TRUE;
	stk->cancel_cmd = send_dtmf_cancel;
	stk->dtmf_id = err;

	stk_alpha_id_set(stk, cmd->send_dtmf.alpha_id,
				&cmd->send_dtmf.text_attr,
				&cmd->send_dtmf.icon_id);

	return FALSE;
}

static void play_tone_cb(enum stk_agent_result result, void *user_data)
{
	struct ofono_stk *stk = user_data;

	switch (result) {
	case STK_AGENT_RESULT_OK:
	case STK_AGENT_RESULT_TIMEOUT:
		send_simple_response(stk, STK_RESULT_TYPE_SUCCESS);
		break;

	default:
		send_simple_response(stk, STK_RESULT_TYPE_USER_TERMINATED);
		break;
	}
}

static gboolean handle_command_play_tone(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	static int manufacturer_timeout = 10000; /* 10 seconds */
	static const struct {
		const char *name;
		/* Continuous true/false according to 02.40 */
		gboolean continuous;
	} tone_infos[] = {
		/* Default */
		[0x00] = { "general-beep", FALSE },

		/* Standard */
		[0x01] = { "dial-tone", TRUE },
		[0x02] = { "busy", TRUE },
		[0x03] = { "congestion", TRUE },
		[0x04] = { "radio-path-acknowledge", FALSE },
		[0x05] = { "radio-path-not-available", FALSE },
		[0x06] = { "error", TRUE },
		[0x07] = { "call-waiting", FALSE },
		[0x08] = { "ringing-tone", TRUE },

		/* Proprietary */
		[0x10] = { "general-beep", FALSE },
		[0x11] = { "positive-acknowledgement", FALSE },
		[0x12] = { "negative-acknowledgement", FALSE },
		[0x13] = { "user-ringing-tone", TRUE },
		[0x14] = { "user-sms-alert", FALSE },
		[0x15] = { "critical", FALSE },
		[0x20] = { "vibrate", TRUE },

		/* Themed */
		[0x30] = { "happy", FALSE },
		[0x31] = { "sad", FALSE },
		[0x32] = { "urgent-action", FALSE },
		[0x33] = { "question", FALSE },
		[0x34] = { "message-received", FALSE },

		/* Melody */
		[0x40] = { "melody-1", FALSE },
		[0x41] = { "melody-2", FALSE },
		[0x42] = { "melody-3", FALSE },
		[0x43] = { "melody-4", FALSE },
		[0x44] = { "melody-5", FALSE },
		[0x45] = { "melody-6", FALSE },
		[0x46] = { "melody-7", FALSE },
		[0x47] = { "melody-8", FALSE },
	};

	const struct stk_command_play_tone *pt = &cmd->play_tone;
	uint8_t qualifier = stk->pending_cmd->qualifier;
	gboolean vibrate = (qualifier & (1 << 0)) != 0;
	char *text;
	int timeout;
	int err;

	if (pt->tone > sizeof(tone_infos) / sizeof(*tone_infos) ||
			tone_infos[pt->tone].name == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;

		return TRUE;
	}

	text = dbus_apply_text_attributes(pt->alpha_id ? pt->alpha_id : "",
						&pt->text_attr);
	if (text == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;

		return TRUE;
	}

	if (pt->duration.interval)
		timeout = duration_to_msecs(&pt->duration);
	else
		timeout = manufacturer_timeout;

	if (!tone_infos[pt->tone].continuous)
		/* Duration ignored */
		err = stk_agent_play_tone(stk->current_agent, text,
						&pt->icon_id, vibrate,
						tone_infos[pt->tone].name,
						play_tone_cb, stk, NULL,
						stk->timeout * 1000);
	else
		err = stk_agent_loop_tone(stk->current_agent, text,
						&pt->icon_id, vibrate,
						tone_infos[pt->tone].name,
						play_tone_cb, stk, NULL,
						timeout);

	g_free(text);

	if (err < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
		return TRUE;
	}

	stk->respond_on_exit = TRUE;
	stk->cancel_cmd = stk_request_cancel;

	return FALSE;
}

static void confirm_launch_browser_cb(enum stk_agent_result result,
					gboolean confirm,
					void *user_data)
{
	struct ofono_stk *stk = user_data;
	unsigned char no_cause[] = { 0x00 };
	struct ofono_error failure = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;

	switch (result) {
	case STK_AGENT_RESULT_TIMEOUT:
		confirm = FALSE;
		/* Fall through */

	case STK_AGENT_RESULT_OK:
		if (confirm)
			break;
		/* Fall through */

	default:
		memset(&rsp, 0, sizeof(rsp));
		ADD_ERROR_RESULT(rsp.result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause);

		if (stk_respond(stk, &rsp, stk_command_cb))
			stk_command_cb(&failure, stk);

		return;
	}

	send_simple_response(stk, STK_RESULT_TYPE_SUCCESS);
}

static gboolean handle_command_launch_browser(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	const struct stk_command_launch_browser *lb = &cmd->launch_browser;
	char *alpha_id;
	int err;

	alpha_id = dbus_apply_text_attributes(lb->alpha_id ? lb->alpha_id : "",
							&lb->text_attr);
	if (alpha_id == NULL) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	err = stk_agent_confirm_launch_browser(stk->current_agent, alpha_id,
						lb->icon_id.id, lb->url,
						confirm_launch_browser_cb,
						stk, NULL, stk->timeout * 1000);
	g_free(alpha_id);

	if (err < 0) {
		unsigned char no_cause_result[] = { 0x00 };

		/*
		 * We most likely got an out of memory error, tell SIM
		 * to retry
		 */
		ADD_ERROR_RESULT(rsp->result, STK_RESULT_TYPE_TERMINAL_BUSY,
					no_cause_result);
		return TRUE;
	}

	stk->respond_on_exit = TRUE;
	stk->cancel_cmd = stk_request_cancel;

	return FALSE;
}

static void setup_call_handled_cancel(struct ofono_stk *stk)
{
	struct ofono_voicecall *vc;

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL,
				__ofono_atom_get_modem(stk->atom));

	if (vc != NULL)
		__ofono_voicecall_clear_alpha_and_icon_id(vc);
}

static gboolean handle_setup_call_confirmation_req(struct stk_command *cmd,
						struct ofono_stk *stk)
{
	const struct stk_command_setup_call *sc = &cmd->setup_call;
	int err;
	char *alpha_id = dbus_apply_text_attributes(
					sc->alpha_id_usr_cfm ?
					sc->alpha_id_usr_cfm : "",
					&sc->text_attr_usr_cfm);
	if (alpha_id == NULL)
		goto out;

	err = stk_agent_confirm_call(stk->current_agent, alpha_id,
					&sc->icon_id_usr_cfm,
					confirm_handled_call_cb,
					stk, NULL,
					stk->timeout * 1000);
	g_free(alpha_id);

	if (err < 0)
		goto out;

	stk->cancel_cmd = setup_call_handled_cancel;

	return TRUE;

out:
	if (stk->driver->user_confirmation)
		stk->driver->user_confirmation(stk, FALSE);

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
		stk->respond_on_exit = FALSE;
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
	if (stk->pending_cmd == NULL) {
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

	case STK_COMMAND_TYPE_SEND_USSD:
		respond = handle_command_send_ussd(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_LANGUAGE_NOTIFICATION:
		/*
		 * If any clients are interested, then the ISO639
		 * 2-letter codes has to be convered to language strings.
		 * Converted language strings has to be added to the
		 * property list.
		 */
		ofono_info("Language Code: %s",
			stk->pending_cmd->language_notification.language);
		break;

	case STK_COMMAND_TYPE_REFRESH:
		respond = handle_command_refresh(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_PROVIDE_LOCAL_INFO:
		respond = handle_command_provide_local_info(stk->pending_cmd,
								&rsp, stk);
		break;

	case STK_COMMAND_TYPE_SEND_DTMF:
		respond = handle_command_send_dtmf(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_PLAY_TONE:
		respond = handle_command_play_tone(stk->pending_cmd,
							&rsp, stk);
		break;

	case STK_COMMAND_TYPE_LAUNCH_BROWSER:
		respond = handle_command_launch_browser(stk->pending_cmd,
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

static gboolean handled_alpha_id_set(struct ofono_stk *stk,
			const char *text, const struct stk_text_attribute *attr,
			const struct stk_icon_id *icon)
{
	if (stk_alpha_id_set(stk, text, attr, icon) == FALSE)
		return FALSE;

	stk->cancel_cmd = stk_alpha_id_unset;
	return TRUE;
}

void ofono_stk_proactive_command_handled_notify(struct ofono_stk *stk,
						int length,
						const unsigned char *pdu)
{
	struct stk_response dummyrsp;
	gboolean ok = FALSE;

	/*
	 * Modems send us the proactive command details and terminal responses
	 * sent by the modem as a response to the command.  Terminal responses
	 * start with the Command Details CTLV tag (0x81).  We filter terminal
	 * responses here
	 */
	if (length > 0 && pdu[0] == 0x81) {
		stk_proactive_command_cancel(stk);
		return;
	}

	stk_proactive_command_cancel(stk);

	stk->pending_cmd = stk_command_new_from_pdu(pdu, length);
	if (stk->pending_cmd == NULL)
		return;

	if (stk->pending_cmd->status != STK_PARSE_RESULT_OK) {
		ofono_error("Can't parse modem-handled proactive command");
		ok = FALSE;
		goto out;
	}

	DBG("type: %d", stk->pending_cmd->type);

	switch (stk->pending_cmd->type) {
	case STK_COMMAND_TYPE_SEND_SMS:
		ok = handled_alpha_id_set(stk,
					stk->pending_cmd->send_sms.alpha_id,
					&stk->pending_cmd->send_sms.text_attr,
					&stk->pending_cmd->send_sms.icon_id);
		break;

	case STK_COMMAND_TYPE_SETUP_IDLE_MODE_TEXT:
		handle_command_set_idle_text(stk->pending_cmd, &dummyrsp, stk);
		break;

	case STK_COMMAND_TYPE_SETUP_MENU:
		handle_command_set_up_menu(stk->pending_cmd, &dummyrsp, stk);
		break;

	case STK_COMMAND_TYPE_SETUP_CALL:
		ok = handle_setup_call_confirmation_req(stk->pending_cmd, stk);
		break;

	case STK_COMMAND_TYPE_SEND_USSD:
		ok = handled_alpha_id_set(stk,
					stk->pending_cmd->send_ussd.alpha_id,
					&stk->pending_cmd->send_ussd.text_attr,
					&stk->pending_cmd->send_ussd.icon_id);
		break;

	case STK_COMMAND_TYPE_SEND_SS:
		ok = handled_alpha_id_set(stk,
					stk->pending_cmd->send_ss.alpha_id,
					&stk->pending_cmd->send_ss.text_attr,
					&stk->pending_cmd->send_ss.icon_id);
		break;

	case STK_COMMAND_TYPE_SEND_DTMF:
		ok = handled_alpha_id_set(stk,
					stk->pending_cmd->send_dtmf.alpha_id,
					&stk->pending_cmd->send_dtmf.text_attr,
					&stk->pending_cmd->send_dtmf.icon_id);
		break;

	case STK_COMMAND_TYPE_REFRESH:
		handle_command_refresh(stk->pending_cmd, NULL, stk);
		break;
	}

out:
	if (ok == FALSE) {
		stk_command_free(stk->pending_cmd);
		stk->pending_cmd = NULL;
	}
}

int ofono_stk_driver_register(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_stk_driver_unregister(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
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

	g_free(stk->idle_mode_text);
	stk->idle_mode_text = NULL;

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

	stk->timeout = 180; /* 3 minutes */
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
