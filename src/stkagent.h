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

struct stk_agent;

enum stk_agent_result {
	STK_AGENT_RESULT_OK,
	STK_AGENT_RESULT_BACK,
	STK_AGENT_RESULT_TERMINATE,
	STK_AGENT_RESULT_TIMEOUT,
};

struct stk_menu_item {
	char *text;
	uint8_t icon_id;
	uint8_t item_id;
};

struct stk_menu {
	char *title;
	uint8_t icon_id;
	struct stk_menu_item *items;
	int default_item;
	gboolean soft_key;
	gboolean has_help;
};

typedef void (*stk_agent_display_text_cb)(enum stk_agent_result result,
						void *user_data);

typedef void (*stk_agent_selection_cb)(enum stk_agent_result result,
					uint8_t id, void *user_data);

typedef void (*stk_agent_confirmation_cb)(enum stk_agent_result result,
						ofono_bool_t confirm,
						void *user_data);

typedef void (*stk_agent_string_cb)(enum stk_agent_result result,
					char *string, void *user_data);

struct stk_agent *stk_agent_new(const char *path, const char *sender,
					ofono_bool_t remove_on_terminate);

void stk_agent_free(struct stk_agent *agent);

void stk_agent_set_removed_notify(struct stk_agent *agent,
					ofono_destroy_func removed_cb,
					void *user_data);

ofono_bool_t stk_agent_matches(struct stk_agent *agent,
				const char *path, const char *sender);

void stk_agent_request_cancel(struct stk_agent *agent);

int stk_agent_request_selection(struct stk_agent *agent,
				const struct stk_menu *menu,
				stk_agent_selection_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout);

int stk_agent_display_text(struct stk_agent *agent, const char *text,
				uint8_t icon_id, ofono_bool_t urgent,
				stk_agent_display_text_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout);

int stk_agent_request_confirmation(struct stk_agent *agent,
					const char *text, uint8_t icon_id,
					stk_agent_confirmation_cb cb,
					void *user_data,
					ofono_destroy_func destroy,
					int timeout);

int stk_agent_request_digit(struct stk_agent *agent,
				const char *text, uint8_t icon_id,
				stk_agent_string_cb cb, void *user_data,
				ofono_destroy_func destroy, int timeout);

int stk_agent_request_key(struct stk_agent *agent, const char *text,
				uint8_t icon_id, ofono_bool_t unicode_charset,
				stk_agent_string_cb cb, void *user_data,
				ofono_destroy_func destroy, int timeout);

int stk_agent_request_digits(struct stk_agent *agent, const char *text,
				uint8_t icon_id, const char *default_text,
				int min, int max, ofono_bool_t hidden,
				stk_agent_string_cb cb, void *user_data,
				ofono_destroy_func destroy, int timeout);

int stk_agent_request_input(struct stk_agent *agent, const char *text,
				uint8_t icon_id, const char *default_text,
				ofono_bool_t unicode_charset, int min, int max,
				ofono_bool_t hidden, stk_agent_string_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout);

int stk_agent_confirm_call(struct stk_agent *agent, const char *text,
				uint8_t icon_id, stk_agent_confirmation_cb cb,
				void *user_data, ofono_destroy_func destroy,
				int timeout);

void append_menu_items_variant(DBusMessageIter *iter,
				const struct stk_menu_item *items);
