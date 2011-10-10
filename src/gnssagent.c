/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  ST-Ericsson AB.
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
#include "gnssagent.h"

struct gnss_agent {
	char *path;
	char *bus;
	guint disconnect_watch;
	ofono_destroy_func removed_cb;
	void *removed_data;
};

static void gnss_agent_send_noreply(struct gnss_agent *agent,
					const char *method, int type, ...)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *message;
	va_list args;

	message = dbus_message_new_method_call(agent->bus, agent->path,
					OFONO_GNSS_POSR_AGENT_INTERFACE,
					method);

	va_start(args, type);
	dbus_message_append_args_valist(message, type, args);
	va_end(args);

	dbus_message_set_no_reply(message, TRUE);

	g_dbus_send_message(conn, message);
}

static inline void gnss_agent_send_release(struct gnss_agent *agent)
{
	gnss_agent_send_noreply(agent, "Release", DBUS_TYPE_INVALID);
}

void gnss_agent_receive_request(struct gnss_agent *agent, const char *xml)
{
	gnss_agent_send_noreply(agent, "Request", DBUS_TYPE_STRING, &xml,
				DBUS_TYPE_INVALID);
}

void gnss_agent_receive_reset(struct gnss_agent *agent)
{
	gnss_agent_send_noreply(agent, "ResetAssistanceData",
				DBUS_TYPE_INVALID);
}

ofono_bool_t gnss_agent_matches(struct gnss_agent *agent,
				const char *path, const char *sender)
{
	return g_str_equal(agent->path, path) &&
			g_str_equal(agent->bus, sender);
}

ofono_bool_t gnss_agent_sender_matches(struct gnss_agent *agent,
					const char *sender)
{
	return g_str_equal(agent->bus, sender);
}

void gnss_agent_set_removed_notify(struct gnss_agent *agent,
					ofono_destroy_func destroy,
					void *user_data)
{
	agent->removed_cb = destroy;
	agent->removed_data = user_data;
}

void gnss_agent_free(struct gnss_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent->disconnect_watch) {
		gnss_agent_send_release(agent);
		g_dbus_remove_watch(conn, agent->disconnect_watch);
		agent->disconnect_watch = 0;
	}

	if (agent->removed_cb)
		agent->removed_cb(agent->removed_data);

	g_free(agent->path);
	g_free(agent->bus);
	g_free(agent);
}

static void gnss_agent_disconnect_cb(DBusConnection *conn, void *user_data)
{
	struct gnss_agent *agent = user_data;

	agent->disconnect_watch = 0;

	gnss_agent_free(agent);
}

struct gnss_agent *gnss_agent_new(const char *path, const char *sender)
{
	struct gnss_agent *agent = g_try_new0(struct gnss_agent, 1);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent == NULL)
		return NULL;

	agent->path = g_strdup(path);
	agent->bus = g_strdup(sender);

	agent->disconnect_watch = g_dbus_add_disconnect_watch(conn, sender,
						gnss_agent_disconnect_cb,
						agent, NULL);

	return agent;
}
