/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014 Jolla Ltd
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

struct voicecall_agent;

void voicecall_agent_ringback_tone(struct voicecall_agent *agent,
					const ofono_bool_t play_tone);

void voicecall_agent_set_removed_notify(struct voicecall_agent *agent,
					ofono_destroy_func removed_cb,
					void *user_data);

void voicecall_agent_free(struct voicecall_agent *agent);

ofono_bool_t voicecall_agent_matches(struct voicecall_agent *agent,
					const char *path, const char *sender);

struct voicecall_agent *voicecall_agent_new(const char *path,
						const char *sender);

void voicecall_agent_disconnect_cb(DBusConnection *conn,
		void *user_data);
