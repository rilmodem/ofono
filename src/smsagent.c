/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include "smsagent.h"

struct sms_agent {
	char *interface;
	char *path;
	char *service;
	guint disconnect_watch;
	ofono_destroy_func removed_cb;
	void *removed_data;
	GSList *reqs;
};

struct sms_agent_request {
	struct sms_agent *agent;
	DBusMessage *msg;
	DBusPendingCall *call;
	sms_agent_dispatch_cb dispatch_cb;
	void *dispatch_data;
	ofono_destroy_func destroy;
};

static struct sms_agent_request *sms_agent_request_new(struct sms_agent *agent,
						sms_agent_dispatch_cb cb,
						void *user_data,
						ofono_destroy_func destroy)
{
	struct sms_agent_request *req;

	req = g_try_new0(struct sms_agent_request, 1);
	if (req == NULL)
		return NULL;

	req->agent = agent;
	req->dispatch_cb = cb;
	req->dispatch_data = user_data;
	req->destroy = destroy;

	return req;
}

static void sms_agent_request_free(struct sms_agent_request *req)
{
	if (req->msg) {
		dbus_message_unref(req->msg);
		req->msg = NULL;
	}

	if (req->call) {
		dbus_pending_call_unref(req->call);
		req->call = NULL;
	}

	if (req->destroy)
		req->destroy(req->dispatch_data);

	g_free(req);
}

static void sms_agent_send_noreply(struct sms_agent *agent, const char *method)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *message;

	message = dbus_message_new_method_call(agent->service, agent->path,
						agent->interface, method);
	if (message == NULL)
		return;

	dbus_message_set_no_reply(message, TRUE);

	DBG("Sending: '%s.%s' to '%s' at '%s'", agent->interface, method,
			agent->service, agent->path);

	g_dbus_send_message(conn, message);
}

static inline void sms_agent_send_release(struct sms_agent *agent)
{
	sms_agent_send_noreply(agent, "Release");
}

static void sms_agent_disconnect_cb(DBusConnection *conn, void *data)
{
	struct sms_agent *agent = data;

	agent->disconnect_watch = 0;

	sms_agent_free(agent);
}

struct sms_agent *sms_agent_new(const char *interface,
				const char *service, const char *path)
{
	struct sms_agent *agent = g_try_new0(struct sms_agent, 1);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent == NULL)
		return NULL;

	agent->interface = g_strdup(interface);
	agent->service = g_strdup(service);
	agent->path = g_strdup(path);

	agent->disconnect_watch = g_dbus_add_disconnect_watch(conn, service,
							sms_agent_disconnect_cb,
							agent, NULL);

	return agent;
}

void sms_agent_set_removed_notify(struct sms_agent *agent,
					ofono_destroy_func destroy,
					void *user_data)
{
	agent->removed_cb = destroy;
	agent->removed_data = user_data;
}

static void sms_agent_request_cancel(gpointer element, gpointer userdata)
{
	struct sms_agent_request *req = element;

	dbus_pending_call_cancel(req->call);
	sms_agent_request_free(req);
}

void sms_agent_free(struct sms_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent == NULL)
		return;

	if (agent->disconnect_watch) {
		sms_agent_send_release(agent);

		g_dbus_remove_watch(conn, agent->disconnect_watch);
		agent->disconnect_watch = 0;
	}

	if (agent->removed_cb)
		agent->removed_cb(agent->removed_data);

	g_slist_foreach(agent->reqs, sms_agent_request_cancel, NULL);
	g_slist_free(agent->reqs);

	g_free(agent->path);
	g_free(agent->service);
	g_free(agent->interface);
	g_free(agent);
}

ofono_bool_t sms_agent_matches(struct sms_agent *agent, const char *service,
				const char *path)
{
	if (path == NULL || service == NULL)
		return FALSE;

	return g_str_equal(agent->path, path) &&
			g_str_equal(agent->service, service);
}

static int check_error(struct sms_agent *agent, DBusMessage *reply,
				enum sms_agent_result *out_result)
{
	DBusError err;
	int result = 0;

	dbus_error_init(&err);

	if (dbus_set_error_from_message(&err, reply) == FALSE) {
		*out_result = SMS_AGENT_RESULT_OK;
		return 0;
	}

	DBG("SmsAgent %s replied with error %s, %s",
			agent->path, err.name, err.message);

	/* Timeout is always valid */
	if (g_str_equal(err.name, DBUS_ERROR_NO_REPLY)) {
		*out_result = SMS_AGENT_RESULT_TIMEOUT;
		goto out;
	}

	result = -EINVAL;

out:
	dbus_error_free(&err);
	return result;
}

static void sms_agent_dispatch_reply_cb(DBusPendingCall *call, void *data)
{
	struct sms_agent_request *req = data;
	struct sms_agent *agent = req->agent;
	sms_agent_dispatch_cb cb = req->dispatch_cb;
	void *dispatch_data = req->dispatch_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(req->call);
	enum sms_agent_result result;

	if (check_error(agent, reply, &result) == -EINVAL) {
		dbus_message_unref(reply);
		sms_agent_free(agent);
		return;
	}

	agent->reqs = g_slist_remove(agent->reqs, req);
	sms_agent_request_free(req);

	if (cb)
		cb(agent, result, dispatch_data);

	dbus_message_unref(reply);
}

int sms_agent_dispatch_datagram(struct sms_agent *agent, const char *method,
				const char *from,
				const struct tm *remote_sent_time,
				const struct tm *local_sent_time,
				const unsigned char *content, unsigned int len,
				sms_agent_dispatch_cb cb, void *user_data,
				ofono_destroy_func destroy)
{
	struct sms_agent_request *req;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessageIter array;
	char buf[128];
	const char *str = buf;

	req = sms_agent_request_new(agent, cb, user_data, destroy);
	if (req == NULL)
		return -ENOMEM;

	req->msg = dbus_message_new_method_call(agent->service, agent->path,
						agent->interface, method);
	if (req->msg == NULL) {
		sms_agent_request_free(req);
		return -ENOMEM;
	}

	dbus_message_iter_init_append(req->msg, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);
	dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
						&content, len);
	dbus_message_iter_close_container(&iter, &array);


	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", local_sent_time);
	buf[127] = '\0';
	ofono_dbus_dict_append(&dict, "LocalSentTime", DBUS_TYPE_STRING, &str);

	strftime(buf, 127, "%Y-%m-%dT%H:%M:%S%z", remote_sent_time);
	buf[127] = '\0';
	ofono_dbus_dict_append(&dict, "SentTime", DBUS_TYPE_STRING, &str);

	ofono_dbus_dict_append(&dict, "Sender", DBUS_TYPE_STRING, &from);

	dbus_message_iter_close_container(&iter, &dict);

	if (!dbus_connection_send_with_reply(conn, req->msg, &req->call, -1)) {
		ofono_error("Sending D-Bus method failed");
		sms_agent_request_free(req);
		return -EIO;
	}

	agent->reqs = g_slist_append(agent->reqs, req);

	dbus_pending_call_set_notify(req->call, sms_agent_dispatch_reply_cb,
					req, NULL);

	return 0;
}
