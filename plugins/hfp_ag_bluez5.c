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
#include <stdlib.h>
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
#include <ofono/handsfree-audio.h>

typedef struct GAtChat GAtChat;
typedef struct GAtResult GAtResult;

#include "drivers/atmodem/atutil.h"

#include "hfp.h"
#include "bluez5.h"
#include "bluetooth.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define HFP_AG_EXT_PROFILE_PATH   "/bluetooth/profile/hfp_ag"
#define BT_ADDR_SIZE 18

#define HFP_AG_DRIVER		"hfp-ag-driver"

static guint modemwatch_id;
static GList *modems;
static GHashTable *sim_hash = NULL;
static GHashTable *connection_hash;

static int hfp_card_probe(struct ofono_handsfree_card *card,
					unsigned int vendor, void *data)
{
	DBG("");

	return 0;
}

static void hfp_card_remove(struct ofono_handsfree_card *card)
{
	DBG("");
}

static void codec_negotiation_done_cb(int err, void *data)
{
	struct cb_data *cbd = data;
	ofono_handsfree_card_connect_cb_t cb = cbd->cb;

	DBG("err %d", err);

	if (err < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	/*
	 * We don't have anything to do at this point as when the
	 * codec negotiation succeeded the emulator internally
	 * already triggered the SCO connection setup of the
	 * handsfree card which also takes over the processing
	 * of the pending dbus message
	 */

done:
	g_free(cbd);
}

static void hfp_card_connect(struct ofono_handsfree_card *card,
					ofono_handsfree_card_connect_cb_t cb,
					void *data)
{
	int err;
	struct ofono_emulator *em = ofono_handsfree_card_get_data(card);
	struct cb_data *cbd;

	DBG("");

	cbd = cb_data_new(cb, data);

	/*
	 * The emulator core will take care if the remote side supports
	 * codec negotiation or not.
	 */
	err = ofono_emulator_start_codec_negotiation(em,
					codec_negotiation_done_cb, cbd);
	if (err < 0) {
		CALLBACK_WITH_FAILURE(cb, data);

		g_free(cbd);
		return;
	}

	/*
	 * We hand over to the emulator core here to establish the
	 * SCO connection once the codec is negotiated
	 * */
}

static void hfp_sco_connected_hint(struct ofono_handsfree_card *card)
{
	DBG("");
}

static struct ofono_handsfree_card_driver hfp_ag_driver = {
	.name			= HFP_AG_DRIVER,
	.probe			= hfp_card_probe,
	.remove			= hfp_card_remove,
	.connect		= hfp_card_connect,
	.sco_connected_hint	= hfp_sco_connected_hint,
};

static void connection_destroy(gpointer data)
{
	int fd = GPOINTER_TO_INT(data);

	DBG("fd %d", fd);

	close(fd);
}

static gboolean io_hup_cb(GIOChannel *io, GIOCondition cond, gpointer data)
{
	char *device = data;

	DBG("Remove %s", device);

	g_hash_table_remove(connection_hash, device);

	return FALSE;
}

static DBusMessage *profile_new_connection(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessageIter entry;
	const char *device;
	GIOChannel *io;
	int fd, fd_dup;
	struct sockaddr_rc saddr;
	socklen_t optlen;
	struct ofono_emulator *em;
	char local[BT_ADDR_SIZE], remote[BT_ADDR_SIZE];
	struct ofono_handsfree_card *card;
	int err;

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

	if (fd < 0)
		goto invalid;

	dbus_message_iter_next(&entry);
	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_ARRAY) {
		close(fd);
		goto invalid;
	}

	if (modems == NULL) {
		close(fd);
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
						".Rejected",
						"No voice call capable modem");
	}

	memset(&saddr, 0, sizeof(saddr));
	optlen = sizeof(saddr);

	if (getsockname(fd, (struct sockaddr *) &saddr, &optlen) < 0) {
		err = errno;
		ofono_error("RFCOMM getsockname(): %s (%d)", strerror(err),
									err);
		close(fd);
		goto invalid;
	}

	bt_ba2str(&saddr.rc_bdaddr, local);

	memset(&saddr, 0, sizeof(saddr));
	optlen = sizeof(saddr);

	if (getpeername(fd, (struct sockaddr *) &saddr, &optlen) < 0) {
		err = errno;
		ofono_error("RFCOMM getpeername(): %s (%d)", strerror(err),
									err);
		close(fd);
		goto invalid;
	}

	bt_ba2str(&saddr.rc_bdaddr, remote);

	em = ofono_emulator_create(modems, OFONO_EMULATOR_TYPE_HFP);
	if (em == NULL) {
		close(fd);
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
						".Rejected",
						"Not enough resources");
	}

	ofono_emulator_register(em, fd);

	fd_dup = dup(fd);
	io = g_io_channel_unix_new(fd_dup);
	g_io_add_watch_full(io, G_PRIORITY_DEFAULT, G_IO_HUP, io_hup_cb,
						g_strdup(device), g_free);
	g_io_channel_unref(io);

	card = ofono_handsfree_card_create(0,
					OFONO_HANDSFREE_CARD_TYPE_GATEWAY,
					HFP_AG_DRIVER, em);

	ofono_handsfree_card_set_data(card, em);

	ofono_handsfree_card_set_local(card, local);
	ofono_handsfree_card_set_remote(card, remote);

	ofono_emulator_set_handsfree_card(em, card);

	g_hash_table_insert(connection_hash, g_strdup(device),
						GINT_TO_POINTER(fd_dup));

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
	DBusMessageIter iter;
	const char *device;
	gpointer fd;

	DBG("Profile handler RequestDisconnection");

	if (!dbus_message_iter_init(msg, &iter))
		goto invalid;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
		goto invalid;

	dbus_message_iter_get_basic(&iter, &device);

	DBG("%s", device);

	fd = g_hash_table_lookup(connection_hash, device);
	if (fd == NULL)
		goto invalid;

	shutdown(GPOINTER_TO_INT(fd), SHUT_RDWR);

	g_hash_table_remove(connection_hash, device);

	return dbus_message_new_method_return(msg);

invalid:
	return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE ".Rejected",
					"Invalid arguments in method call");
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

	bt_register_profile(conn, HFP_AG_UUID, HFP_VERSION_1_7, "hfp_ag",
					HFP_AG_EXT_PROFILE_PATH, NULL, 0);
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

static void voicecall_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_atom *sim_atom;
	struct ofono_sim *sim;
	struct ofono_modem *modem;
	DBusConnection *conn = ofono_dbus_get_connection();

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED)
		return;

	/*
	 * This logic is only intended to handle voicecall atoms
	 * registered in post_sim state or later
	 */
	modem = __ofono_atom_get_modem(atom);

	sim_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SIM);
	if (sim_atom == NULL)
		return;

	sim = __ofono_atom_get_data(sim_atom);
	if (ofono_sim_get_state(sim) != OFONO_SIM_STATE_READY)
		return;

	modems = g_list_append(modems, modem);

	if (modems->next != NULL)
		return;

	bt_register_profile(conn, HFP_AG_UUID, HFP_VERSION_1_7, "hfp_ag",
					HFP_AG_EXT_PROFILE_PATH, NULL, 0);
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	DBG("modem: %p, added: %d", modem, added);

	if (added == FALSE)
		return;

	__ofono_modem_add_atom_watch(modem, OFONO_ATOM_TYPE_SIM,
					sim_watch, modem, NULL);
	__ofono_modem_add_atom_watch(modem, OFONO_ATOM_TYPE_VOICECALL,
					voicecall_watch, modem, NULL);
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int hfp_ag_init(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	int err;

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

	err = ofono_handsfree_card_driver_register(&hfp_ag_driver);
	if (err < 0) {
		g_dbus_unregister_interface(conn, HFP_AG_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);
		return err;
	}

	sim_hash = g_hash_table_new(g_direct_hash, g_direct_equal);

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);
	__ofono_modem_foreach(call_modemwatch, NULL);

	connection_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, connection_destroy);

	ofono_handsfree_audio_ref();

	return 0;
}

static void hfp_ag_exit(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	__ofono_modemwatch_remove(modemwatch_id);
	g_dbus_unregister_interface(conn, HFP_AG_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);

	ofono_handsfree_card_driver_unregister(&hfp_ag_driver);

	g_hash_table_destroy(connection_hash);

	g_list_free(modems);
	g_hash_table_foreach_remove(sim_hash, sim_watch_remove, NULL);
	g_hash_table_destroy(sim_hash);

	ofono_handsfree_audio_unref();
}

OFONO_PLUGIN_DEFINE(hfp_ag_bluez5, "Hands-Free Audio Gateway Profile Plugins",
				VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
				hfp_ag_init, hfp_ag_exit)
