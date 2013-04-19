/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <glib.h>
#include <ofono.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

#include "hfp.h"
#include "bluez5.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define HFP_AG_EXT_PROFILE_PATH   "/bluetooth/profile/hfp_ag"

static guint modemwatch_id;
static GList *modems;
static GHashTable *sim_hash = NULL;
static GHashTable *connection_hash;

static void connection_destroy(gpointer data)
{
	int fd = GPOINTER_TO_INT(data);

	DBG("fd %d", fd);

	close(fd);
}

static DBusMessage *profile_new_connection(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessageIter entry;
	const char *device;
	int fd;
	struct ofono_emulator *em;
	struct ofono_modem *modem;

	DBG("Profile handler NewConnection");

	if (dbus_message_iter_init(msg, &entry) == FALSE)
		goto invalid;

	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_OBJECT_PATH)
		goto invalid;

	dbus_message_iter_get_basic(&entry, &device);
	dbus_message_iter_next(&entry);

	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_UNIX_FD)
		goto invalid;

	dbus_message_iter_get_basic(&entry, &fd);
	dbus_message_iter_next(&entry);

	if (fd < 0)
		goto invalid;

	DBG("%s", device);

	/* Pick the first voicecall capable modem */
	if (modems == NULL) {
		close(fd);
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
						".Rejected",
						"No voice call capable modem");
	}

	modem = modems->data;
	DBG("Picked modem %p for emulator", modem);

	em = ofono_emulator_create(modem, OFONO_EMULATOR_TYPE_HFP);
	if (em == NULL) {
		close(fd);
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
						".Rejected",
						"Not enough resources");
	}

	ofono_emulator_register(em, fd);

	return dbus_message_new_method_return(msg);

invalid:
	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE ".Rejected",
					"Invalid arguments in method call");
}

static DBusMessage *profile_release(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBG("Profile handler Release");

	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
						".NotImplemented",
						"Implementation not provided");
}

static DBusMessage *profile_cancel(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBG("Profile handler Cancel");

	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".NotImplemented",
					"Implementation not provided");
}

static DBusMessage *profile_disconnection(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBG("Profile handler RequestDisconnection");

	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".NotImplemented",
					"Implementation not provided");
}

static const GDBusMethodTable profile_methods[] = {
	{ GDBUS_ASYNC_METHOD("NewConnection",
				GDBUS_ARGS({ "device", "o"}, { "fd", "h"},
						{ "fd_properties", "a{sv}" }),
				NULL, profile_new_connection) },
	{ GDBUS_METHOD("Release", NULL, NULL, profile_release) },
	{ GDBUS_METHOD("Cancel", NULL, NULL, profile_cancel) },
	{ GDBUS_METHOD("RequestDisconnection",
				GDBUS_ARGS({"device", "o"}), NULL,
				profile_disconnection) },
	{ }
};

static void sim_state_watch(enum ofono_sim_state new_state, void *data)
{
	struct ofono_modem *modem = data;
	DBusConnection *conn = ofono_dbus_get_connection();

	if (new_state != OFONO_SIM_STATE_READY) {
		if (modems == NULL)
			return;

		modems = g_list_remove(modems, modem);
		if (modems != NULL)
			return;

		bt_unregister_profile(conn, HFP_AG_EXT_PROFILE_PATH);

		return;
	}

	if (__ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_VOICECALL) == NULL)
		return;

	modems = g_list_append(modems, modem);

	if (modems->next != NULL)
		return;

	bt_register_profile(conn, HFP_AG_UUID, HFP_VERSION_1_5, "hfp_ag",
						HFP_AG_EXT_PROFILE_PATH);
}

static gboolean sim_watch_remove(gpointer key, gpointer value,
				gpointer user_data)
{
	struct ofono_sim *sim = key;

	ofono_sim_remove_state_watch(sim, GPOINTER_TO_UINT(value));

	return TRUE;
}

static void sim_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_sim *sim = __ofono_atom_get_data(atom);
	struct ofono_modem *modem = data;
	int watch;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		sim_state_watch(OFONO_SIM_STATE_NOT_PRESENT, modem);

		sim_watch_remove(sim, g_hash_table_lookup(sim_hash, sim), NULL);
		g_hash_table_remove(sim_hash, sim);

		return;
	}

	watch = ofono_sim_add_state_watch(sim, sim_state_watch, modem, NULL);
	g_hash_table_insert(sim_hash, sim, GUINT_TO_POINTER(watch));
	sim_state_watch(ofono_sim_get_state(sim), modem);
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	DBG("modem: %p, added: %d", modem, added);

	if (added == FALSE)
		return;

	__ofono_modem_add_atom_watch(modem, OFONO_ATOM_TYPE_SIM,
					sim_watch, modem, NULL);
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int hfp_ag_init(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	/* Registers External Profile handler */
	if (!g_dbus_register_interface(conn, HFP_AG_EXT_PROFILE_PATH,
					BLUEZ_PROFILE_INTERFACE,
					profile_methods, NULL,
					NULL, NULL, NULL)) {
		ofono_error("Register Profile interface failed: %s",
						HFP_AG_EXT_PROFILE_PATH);
		return -EIO;
	}

	sim_hash = g_hash_table_new(g_direct_hash, g_direct_equal);

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);
	__ofono_modem_foreach(call_modemwatch, NULL);

	connection_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, connection_destroy);

	return 0;
}

static void hfp_ag_exit(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	__ofono_modemwatch_remove(modemwatch_id);
	g_dbus_unregister_interface(conn, HFP_AG_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);

	g_hash_table_destroy(connection_hash);

	g_list_free(modems);
	g_hash_table_foreach_remove(sim_hash, sim_watch_remove, NULL);
	g_hash_table_destroy(sim_hash);
}

OFONO_PLUGIN_DEFINE(hfp_ag_bluez5, "Hands-Free Audio Gateway Profile Plugins",
				VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
				hfp_ag_init, hfp_ag_exit)
