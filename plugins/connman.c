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

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <gdbus.h>
#include <string.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/log.h>
#include <ofono/dbus.h>
#include <ofono/plugin.h>
#include <ofono/private-network.h>

#define CONNMAN_SERVICE			"net.connman"
#define CONNMAN_PATH			"/net/connman"

#define CONNMAN_MANAGER_INTERFACE	CONNMAN_SERVICE ".Manager"
#define CONNMAN_MANAGER_PATH		"/"

static DBusConnection *connection;
static GHashTable *requests;
static unsigned int id;

struct pns_req {
	int uid;
	DBusPendingCall *pending;
	ofono_private_network_cb_t cb;
	void *data;
	gboolean redundant;
	char *path;
};

static void send_release(const char *path)
{
	DBusMessage *message;

	message = dbus_message_new_method_call(CONNMAN_SERVICE,
						CONNMAN_MANAGER_PATH,
						CONNMAN_MANAGER_INTERFACE,
						"ReleasePrivateNetwork");
	if (message == NULL)
		return;

	dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);
	dbus_message_set_no_reply(message, TRUE);
	dbus_connection_send(connection, message, NULL);
	dbus_message_unref(message);
}

static void pns_release(int uid)
{
	struct pns_req *req;

	DBG("");

	req = g_hash_table_lookup(requests, &uid);
	if (req == NULL)
		return;

	if (req->pending) {
		/*
		* We want to cancel the request but we have to wait
		* the response of ConnMan. So we mark request as
		* redundant until we get the response, then we remove
		* it from hash table.
		*/
		req->redundant = TRUE;
		return;
	}

	send_release(req->path);
	g_hash_table_remove(requests, &req->uid);
}

static void request_reply(DBusPendingCall *call, void *user_data)
{
	struct pns_req *req = user_data;
	struct ofono_private_network_settings pns;
	DBusMessageIter array, dict, entry;
	DBusMessage *reply;
	const char *path;

	DBG("");

	pns.fd = -1;
	pns.server_ip = NULL;
	pns.peer_ip = NULL;
	pns.primary_dns = NULL;
	pns.secondary_dns = NULL;

	req->pending = NULL;

	reply = dbus_pending_call_steal_reply(call);
	if (!reply)
		goto error;

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
		goto error;

	if (dbus_message_iter_init(reply, &array) == FALSE)
		goto error;

	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_OBJECT_PATH)
		goto error;

	dbus_message_iter_get_basic(&array, &path);

	dbus_message_iter_next(&array);
	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY)
		goto error;

	if (req->redundant == TRUE)
		goto release;

	dbus_message_iter_recurse(&array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter iter;
		const char *key;
		int type;

		dbus_message_iter_recurse(&dict, &entry);

		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &iter);

		type = dbus_message_iter_get_arg_type(&iter);
		if (type != DBUS_TYPE_STRING)
			break;

		if (g_str_equal(key, "ServerIPv4") &&
				type == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&iter, &pns.server_ip);
		else if (g_str_equal(key, "PeerIPv4") &&
				type == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&iter, &pns.peer_ip);
		else if (g_str_equal(key, "PrimaryDNS") &&
				type == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&iter, &pns.primary_dns);
		else if (g_str_equal(key, "SecondaryDNS") &&
				type == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&iter, &pns.secondary_dns);

		dbus_message_iter_next(&dict);
	}

	dbus_message_iter_next(&array);
	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_UNIX_FD)
		goto error;

	dbus_message_iter_get_basic(&array, &pns.fd);
	DBG("Fildescriptor = %d\n", pns.fd);

	req->path = g_strdup(path);
	DBG("Object path = %s\n", req->path);

	if (pns.server_ip == NULL || pns.peer_ip == NULL ||
			pns.primary_dns == NULL || pns.secondary_dns == NULL ||
			pns.fd < 0) {
		ofono_error("Error while reading dictionnary...\n");
		goto release;
	}

	req->cb(&pns, req->data);

	dbus_message_unref(reply);
	dbus_pending_call_unref(call);

	return;

release:
	pns_release(req->uid);
error:
	if (pns.fd >= 0)
		close(pns.fd);

	if (req->redundant == FALSE)
		req->cb(NULL, req->data);

	if (reply)
		dbus_message_unref(reply);

	dbus_pending_call_unref(call);
}

static int pns_request(ofono_private_network_cb_t cb, void *data)
{
	DBusMessage *message;
	DBusPendingCall *call;
	struct pns_req *req;

	DBG("");

	req = g_try_new(struct pns_req, 1);

	if (req == NULL)
		return -ENOMEM;

	message = dbus_message_new_method_call(CONNMAN_SERVICE,
						CONNMAN_MANAGER_PATH,
						CONNMAN_MANAGER_INTERFACE,
						"RequestPrivateNetwork");

	if (message == NULL) {
		g_free(req);
		return -ENOMEM;
	}

	if (dbus_connection_send_with_reply(connection, message,
						&call, 5000) == FALSE) {
		g_free(req);
		dbus_message_unref(message);
		return -EIO;
	}

	id++;
	req->pending = call;
	req->cb = cb;
	req->data = data;
	req->uid = id;
	req->redundant = FALSE;

	dbus_pending_call_set_notify(call, request_reply, req, NULL);
	g_hash_table_insert(requests, &req->uid, req);
	dbus_message_unref(message);

	return req->uid;
}

static struct ofono_private_network_driver pn_driver = {
	.name		= "ConnMan Private Network",
	.request	= pns_request,
	.release	= pns_release,
};

static void request_free(gpointer user_data)
{
	struct pns_req *req = user_data;

	g_free(req->path);
	g_free(req);
}

static int connman_init(void)
{
	DBG("");

	connection = ofono_dbus_get_connection();
	requests = g_hash_table_new_full(g_int_hash, g_int_equal, NULL,
						request_free);

	return ofono_private_network_driver_register(&pn_driver);
}

static void connman_exit(void)
{
	g_hash_table_destroy(requests);
	ofono_private_network_driver_unregister(&pn_driver);
}

OFONO_PLUGIN_DEFINE(connman, "ConnMan plugin", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, connman_init, connman_exit)
