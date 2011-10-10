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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <glib.h>
#include <gdbus.h>
#include <errno.h>

#include "ofono.h"

#include "common.h"
#include "gnssagent.h"

static GSList *g_drivers = NULL;

struct ofono_gnss {
	const struct ofono_gnss_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	DBusMessage *pending;
	struct gnss_agent *posr_agent;
	ofono_bool_t enabled;
};

static void gnss_unregister_agent_cb(const struct ofono_error *error,
					void *data)
{
	DBusMessage *reply;
	struct ofono_gnss *gnss = data;

	DBG("");

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_error("Disabling Location Reporting Failed");

	gnss->enabled = FALSE;

	if (gnss->posr_agent)
		gnss_agent_free(gnss->posr_agent);

	reply = dbus_message_new_method_return(gnss->pending);
	__ofono_dbus_pending_reply(&gnss->pending, reply);
}

static void gnss_disable_posr_cb(const struct ofono_error *error, void *data)
{
	struct ofono_gnss *gnss = data;

	gnss->enabled = FALSE;
}

static void gnss_register_agent_cb(const struct ofono_error *error,
					void *data)
{
	DBusMessage *reply;
	struct ofono_gnss *gnss = data;

	DBG("");

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Enabling Location Reporting Failed");
		reply = __ofono_error_failed(gnss->pending);

		if (gnss->posr_agent)
			gnss_agent_free(gnss->posr_agent);

		__ofono_dbus_pending_reply(&gnss->pending, reply);
		return;
	}

	reply = dbus_message_new_method_return(gnss->pending);
	__ofono_dbus_pending_reply(&gnss->pending, reply);

	gnss->enabled = TRUE;

	if (gnss->posr_agent == NULL)
		gnss->driver->set_position_reporting(gnss, FALSE,
							gnss_disable_posr_cb,
							gnss);
}

static void gnss_agent_notify(gpointer user_data)
{
	struct ofono_gnss *gnss = user_data;

	gnss->posr_agent = NULL;

	if (gnss->enabled == TRUE)
		gnss->driver->set_position_reporting(gnss, FALSE,
							gnss_disable_posr_cb,
							gnss);
}

static DBusMessage *gnss_register_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gnss *gnss = data;
	const char *agent_path;

	if (gnss->pending)
		return __ofono_error_busy(msg);

	if (gnss->posr_agent)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH,
				&agent_path, DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!__ofono_dbus_valid_object_path(agent_path))
		return __ofono_error_invalid_format(msg);

	gnss->posr_agent = gnss_agent_new(agent_path,
						dbus_message_get_sender(msg));

	if (gnss->posr_agent == NULL)
		return __ofono_error_failed(msg);

	gnss_agent_set_removed_notify(gnss->posr_agent,
					gnss_agent_notify, gnss);

	gnss->driver->set_position_reporting(gnss, TRUE, gnss_register_agent_cb,
						gnss);

	gnss->pending = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *gnss_unregister_agent(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_gnss *gnss = data;
	const char *agent_path;
	const char *agent_bus = dbus_message_get_sender(msg);

	if (gnss->pending)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (gnss->posr_agent == NULL)
		return __ofono_error_failed(msg);

	if (!gnss_agent_matches(gnss->posr_agent, agent_path, agent_bus))
		return __ofono_error_access_denied(msg);

	gnss->pending = dbus_message_ref(msg);

	gnss->enabled = FALSE;
	gnss->driver->set_position_reporting(gnss, FALSE,
						gnss_unregister_agent_cb,
						gnss);

	return NULL;
}

static void gnss_send_element_cb(const struct ofono_error *error,
				void *data)
{
	DBusMessage *reply;
	struct ofono_gnss *gnss = data;

	DBG("");

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Sending Positioning Element failed");
		reply = __ofono_error_failed(gnss->pending);
	} else
		reply = dbus_message_new_method_return(gnss->pending);

	__ofono_dbus_pending_reply(&gnss->pending, reply);
}

static DBusMessage *gnss_send_element(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *caller = dbus_message_get_sender(msg);
	struct ofono_gnss *gnss = data;
	const char *xml;

	DBG("");

	if (gnss->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &xml,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (gnss->posr_agent == NULL)
		return __ofono_error_not_available(msg);

	if (!gnss_agent_sender_matches(gnss->posr_agent, caller))
		return __ofono_error_access_denied(msg);

	gnss->pending = dbus_message_ref(msg);

	gnss->driver->send_element(gnss, xml, gnss_send_element_cb, gnss);

	return NULL;
}

static GDBusMethodTable gnss_methods[] = {
	{ "SendPositioningElement",		"s",	"",
			gnss_send_element, G_DBUS_METHOD_FLAG_ASYNC },
	{ "RegisterPositioningRequestAgent",	"o",	"",
			gnss_register_agent, G_DBUS_METHOD_FLAG_ASYNC },
	{ "UnregisterPositioningRequestAgent",	"o",	"",
			gnss_unregister_agent, G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static void gnss_unregister(struct ofono_atom *atom)
{
	struct ofono_gnss *gnss = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	if (gnss->posr_agent)
		gnss_agent_free(gnss->posr_agent);

	ofono_modem_remove_interface(modem, OFONO_GNSS_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_GNSS_INTERFACE);
}

static void gnss_remove(struct ofono_atom *atom)
{
	struct ofono_gnss *gnss = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (gnss == NULL)
		return;

	if (gnss->driver && gnss->driver->remove)
		gnss->driver->remove(gnss);

	g_free(gnss);
}

void ofono_gnss_register(struct ofono_gnss *gnss)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(gnss->atom);
	const char *path = __ofono_atom_get_path(gnss->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_GNSS_INTERFACE,
					gnss_methods, NULL, NULL,
					gnss, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_GNSS_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_GNSS_INTERFACE);

	__ofono_atom_register(gnss->atom, gnss_unregister);
}

int ofono_gnss_driver_register(const struct ofono_gnss_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_gnss_driver_unregister(const struct ofono_gnss_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

struct ofono_gnss *ofono_gnss_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_gnss *gnss;
	GSList *l;

	if (driver == NULL)
		return NULL;

	gnss = g_try_new0(struct ofono_gnss, 1);

	if (gnss == NULL)
		return NULL;

	gnss->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_GNSS,
						gnss_remove, gnss);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_gnss_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(gnss, vendor, data) < 0)
			continue;

		gnss->driver = drv;
		break;
	}

	return gnss;
}

void ofono_gnss_notify_posr_request(struct ofono_gnss *gnss, const char *xml)
{
	if (gnss->posr_agent)
		gnss_agent_receive_request(gnss->posr_agent, xml);
}

void ofono_gnss_notify_posr_reset(struct ofono_gnss *gnss)
{
	if (gnss->posr_agent)
		gnss_agent_receive_reset(gnss->posr_agent);
}

void ofono_gnss_remove(struct ofono_gnss *gnss)
{
	__ofono_atom_free(gnss->atom);
}

void ofono_gnss_set_data(struct ofono_gnss *gnss, void *data)
{
	gnss->driver_data = data;
}

void *ofono_gnss_get_data(struct ofono_gnss *gnss)
{
	return gnss->driver_data;
}
