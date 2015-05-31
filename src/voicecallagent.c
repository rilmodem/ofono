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

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"
#include "voicecallagent.h"

#define OFONO_VOICECALL_AGENT_INTERFACE "org.ofono.VoiceCallAgent"

struct voicecall_agent {
	char *path;			/* Agent Path */
	char *bus;			/* Agent bus */
	guint disconnect_watch;		/* DBus disconnect watch */
	ofono_destroy_func removed_cb;
	void *removed_data;
};

void voicecall_agent_send_release(struct voicecall_agent *agent);

void voicecall_agent_ringback_tone(struct voicecall_agent *agent,
					const ofono_bool_t play_tone)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *message = dbus_message_new_method_call(
			agent->bus, agent->path,
			OFONO_VOICECALL_AGENT_INTERFACE,
			"RingbackTone");

	if (message == NULL)
		return;

	if (!dbus_message_append_args(message, DBUS_TYPE_BOOLEAN, &play_tone,
				DBUS_TYPE_INVALID))
		return;

	dbus_message_set_no_reply(message, TRUE);
	g_dbus_send_message(conn, message);
}

void voicecall_agent_send_release(struct voicecall_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *message = dbus_message_new_method_call(
			agent->bus, agent->path,
			OFONO_VOICECALL_AGENT_INTERFACE,
			"Release");

	if (message == NULL)
		return;

	dbus_message_set_no_reply(message, TRUE);
	g_dbus_send_message(conn, message);
}

void voicecall_agent_set_removed_notify(struct voicecall_agent *agent,
					ofono_destroy_func destroy,
					void *user_data)
{
	agent->removed_cb = destroy;
	agent->removed_data = user_data; /* voicecall atom (not owned) */
}

void voicecall_agent_free(struct voicecall_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent->disconnect_watch) {
		voicecall_agent_send_release(agent);
		g_dbus_remove_watch(conn, agent->disconnect_watch);
		agent->disconnect_watch = 0;
	}

	if (agent->removed_cb)
		agent->removed_cb(agent->removed_data);

	g_free(agent->path);
	g_free(agent->bus);
	g_free(agent);
}

ofono_bool_t voicecall_agent_matches(struct voicecall_agent *agent,
				const char *path, const char *sender)
{
	return g_str_equal(agent->path, path) &&
			g_str_equal(agent->bus, sender);
}

void voicecall_agent_disconnect_cb(DBusConnection *conn, void *user_data)
{
	struct voicecall_agent *agent = user_data;

	agent->disconnect_watch = 0;
	voicecall_agent_free(agent);
}

struct voicecall_agent *voicecall_agent_new(const char *path,
						const char *sender)
{
	struct voicecall_agent *agent = g_try_new0(struct voicecall_agent, 1);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent == NULL)
		return NULL;

	agent->path = g_strdup(path);
	agent->bus = g_strdup(sender);

	agent->disconnect_watch = g_dbus_add_disconnect_watch(conn, sender,
						voicecall_agent_disconnect_cb,
						agent, NULL);

	return agent;
}
