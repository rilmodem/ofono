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

enum stk_agent_result {
	STK_AGENT_RESULT_OK,
	STK_AGENT_RESULT_BACK,
	STK_AGENT_RESULT_TERMINATE,
	STK_AGENT_RESULT_HELP,
	STK_AGENT_RESULT_TIMEOUT,
	STK_AGENT_RESULT_CANCEL,
};

typedef void (*stk_agent_generic_cb)(enum stk_agent_result result,
					void *user_data);

struct stk_agent;

struct stk_agent *stk_agent_new(const char *path, const char *sender,
					ofono_bool_t is_default);

void stk_agent_remove(struct stk_agent *agent);

ofono_bool_t stk_agent_busy(struct stk_agent *agent);
ofono_bool_t stk_agent_matches(struct stk_agent *agent,
				const char *path, const char *sender);
void stk_agent_set_destroy_watch(struct stk_agent *agent, GDestroyNotify notify,
					void *user_data);

void stk_agent_request_cancel(struct stk_agent *agent);
