/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011 ST-Ericsson AB.
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

struct gnss_agent;

struct gnss_agent *gnss_agent_new(const char *path, const char *sender);

void gnss_agent_free(struct gnss_agent *agent);

void gnss_agent_receive_request(struct gnss_agent *agent, const char *xml);

void gnss_agent_receive_reset(struct gnss_agent *agent);

void gnss_agent_set_removed_notify(struct gnss_agent *agent,
					ofono_destroy_func removed_cb,
					void *user_data);

ofono_bool_t gnss_agent_matches(struct gnss_agent *agent,
				const char *path, const char *sender);

ofono_bool_t gnss_agent_sender_matches(struct gnss_agent *agent,
					const char *sender);
