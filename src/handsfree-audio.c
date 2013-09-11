/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
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
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include <gdbus.h>

#include <ofono/handsfree-audio.h>

#include "bluetooth.h"
#include "hfp.h"
#include "ofono.h"

#define HFP_AUDIO_MANAGER_INTERFACE	OFONO_SERVICE ".HandsfreeAudioManager"
#define HFP_AUDIO_AGENT_INTERFACE	OFONO_SERVICE ".HandsfreeAudioAgent"
#define HFP_AUDIO_CARD_INTERFACE	OFONO_SERVICE ".HandsfreeAudioCard"

struct ofono_handsfree_card {
	char *remote;
	char *local;
	char *path;
	DBusMessage *msg;
	unsigned char selected_codec;
	const struct ofono_handsfree_card_driver *driver;
	void *driver_data;
};

struct agent {
	char *owner;
	char *path;
	guint watch;
};

static struct agent *agent = NULL;
static int ref_count = 0;
static GSList *card_list = 0;
static guint sco_watch = 0;
static GSList *drivers = 0;
static ofono_bool_t has_wideband = FALSE;
static int defer_setup = 1;
static ofono_bool_t transparent_sco = FALSE;

static uint16_t codec2setting(uint8_t codec)
{
	switch (codec) {
		case HFP_CODEC_CVSD:
			return BT_VOICE_CVSD_16BIT;
		default:
			return BT_VOICE_TRANSPARENT;
	}
}

static ofono_bool_t apply_settings_from_codec(int fd, uint8_t codec)
{
	struct bt_voice voice;

	memset(&voice, 0, sizeof(voice));
	voice.setting = codec2setting(codec);

	/* CVSD is the default, no need to set BT_VOICE. */
	if (voice.setting == BT_VOICE_CVSD_16BIT)
		return TRUE;

	if (setsockopt(fd, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) < 0)
		return FALSE;

	return TRUE;
}

static void send_new_connection(const char *card, int fd, uint8_t codec)
{
	DBusMessage *msg;
	DBusMessageIter iter;

	msg = dbus_message_new_method_call(agent->owner, agent->path,
				HFP_AUDIO_AGENT_INTERFACE, "NewConnection");
	if (msg == NULL)
		return;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &card);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_UNIX_FD, &fd);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE, &codec);

	g_dbus_send_message(ofono_dbus_get_connection(), msg);
}

static struct ofono_handsfree_card *card_find(const char *remote,
							const char *local)
{
	GSList *list;

	for (list = card_list; list; list = g_slist_next(list)) {
		struct ofono_handsfree_card *card = list->data;

		if (g_str_equal(card->remote, remote) &&
				g_str_equal(card->local, local))
			return card;
	}

	return NULL;
}

static gboolean sco_accept(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct ofono_handsfree_card *card;
	struct sockaddr_sco saddr;
	socklen_t alen;
	int sk, nsk;
	char local[18], remote[18];

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
		return FALSE;

	sk = g_io_channel_unix_get_fd(io);

	memset(&saddr, 0, sizeof(saddr));
	alen = sizeof(saddr);

	nsk = accept(sk, (struct sockaddr *) &saddr, &alen);
	if (nsk < 0)
		return TRUE;

	if (agent == NULL) {
		ofono_error("Reject SCO: Agent not registered");
		close(nsk);
		return TRUE;
	}

	bt_ba2str(&saddr.sco_bdaddr, remote);

	memset(&saddr, 0, sizeof(saddr));
	alen = sizeof(saddr);

	if (getsockname(nsk, (struct sockaddr *) &saddr, &alen) < 0) {
		ofono_error("SCO getsockname(): %s (%d)",
						strerror(errno), errno);
		close(nsk);
		return TRUE;
	}

	bt_ba2str(&saddr.sco_bdaddr, local);

	card = card_find(remote, local);
	if (card == NULL || card->path == NULL) {
		ofono_error("Rejecting SCO: Audio Card not found!");
		close(nsk);
		return TRUE;
	}

	if (apply_settings_from_codec(nsk, card->selected_codec) == FALSE) {
		close(nsk);
		return TRUE;
	}

	send_new_connection(card->path, nsk, card->selected_codec);
	close(nsk);

	return TRUE;
}

static int sco_init(void)
{
	GIOChannel *sco_io;
	struct sockaddr_sco saddr;
	struct bt_voice voice;
	int sk;
	socklen_t len;

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET | O_NONBLOCK | SOCK_CLOEXEC,
								BTPROTO_SCO);
	if (sk < 0)
		return -errno;

	/* Bind to local address */
	memset(&saddr, 0, sizeof(saddr));
	saddr.sco_family = AF_BLUETOOTH;
	bt_bacpy(&saddr.sco_bdaddr, BDADDR_ANY);

	if (bind(sk, (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
		close(sk);
		return -errno;
	}

	if (setsockopt(sk, SOL_BLUETOOTH, BT_DEFER_SETUP,
				&defer_setup, sizeof(defer_setup)) < 0) {
		defer_setup = 0;
		ofono_warn("Can't enable deferred setup: %s (%d)",
						strerror(errno), errno);
	}

	memset(&voice, 0, sizeof(voice));
	len = sizeof(voice);

	if (defer_setup && getsockopt(sk, SOL_BLUETOOTH, BT_VOICE,
						&voice, &len) == 0)
		transparent_sco = TRUE;

	if (listen(sk, 5) < 0) {
		close(sk);
		return -errno;
	}

	sco_io = g_io_channel_unix_new(sk);
	sco_watch = g_io_add_watch(sco_io,
				G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
				sco_accept, NULL);

	g_io_channel_unref(sco_io);

	return 0;
}

static void card_append_properties(struct ofono_handsfree_card *card,
					DBusMessageIter *dict)
{
	ofono_dbus_dict_append(dict, "RemoteAddress",
				DBUS_TYPE_STRING, &card->remote);

	ofono_dbus_dict_append(dict, "LocalAddress",
				DBUS_TYPE_STRING, &card->local);
}

static DBusMessage *card_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_handsfree_card *card = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	card_append_properties(card, &dict);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static gboolean sco_connect_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)

{
	struct ofono_handsfree_card *card = user_data;
	DBusMessage *reply;
	int sk;

	if (agent == NULL) {
		/* There's no agent, so there's no one to reply to */
		reply = NULL;
		goto done;
	}

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		if (card->msg)
			reply = __ofono_error_failed(card->msg);
		goto done;
	}

	sk = g_io_channel_unix_get_fd(io);

	send_new_connection(card->path, sk, card->selected_codec);

	close(sk);

	if (card->msg)
		reply = dbus_message_new_method_return(card->msg);

done:
	if (card->msg == NULL)
		return FALSE;

	if (reply)
		g_dbus_send_message(ofono_dbus_get_connection(), reply);

	dbus_message_unref(card->msg);
	card->msg = NULL;

	return FALSE;
}

static void card_connect_reply_cb(const struct ofono_error *error, void *data)
{
	struct ofono_handsfree_card *card = data;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(card->msg);
	else
		reply = __ofono_error_failed(card->msg);

	__ofono_dbus_pending_reply(&card->msg, reply);
}

static DBusMessage *card_connect(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_handsfree_card *card = data;
	const struct ofono_handsfree_card_driver *driver = card->driver;
	const char *sender;
	int err;

	if (agent == NULL)
		return __ofono_error_not_available(msg);

	sender = dbus_message_get_sender(msg);

	if (!g_str_equal(sender, agent->owner))
		return __ofono_error_not_allowed(msg);

	if (card->msg)
		return __ofono_error_busy(msg);

	if (!driver || !driver->connect)
		goto fallback;

	card->msg = dbus_message_ref(msg);

	driver->connect(card, card_connect_reply_cb, card);

	return NULL;

fallback:
	/* There's no driver, fallback to direct SCO connection */
	err = ofono_handsfree_card_connect_sco(card);
	if (err < 0)
		return __ofono_error_failed(msg);

	card->msg = dbus_message_ref(msg);

	return NULL;
}

static const GDBusMethodTable card_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			card_get_properties) },
	{ GDBUS_ASYNC_METHOD("Connect", NULL, NULL, card_connect) },
	{ }
};

static const GDBusSignalTable card_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

struct ofono_handsfree_card *ofono_handsfree_card_create(unsigned int vendor,
							const char *driver,
							void *data)
{
	struct ofono_handsfree_card *card;
	GSList *l;

	card = g_new0(struct ofono_handsfree_card, 1);

	card->selected_codec = HFP_CODEC_CVSD;

	card_list = g_slist_prepend(card_list, card);

	for (l = drivers; l; l = l->next) {
		const struct ofono_handsfree_card_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(card, vendor, data) < 0)
			continue;

		card->driver = drv;
		break;
	}

	return card;
}

void ofono_handsfree_card_set_data(struct ofono_handsfree_card *card,
					void *data)
{
	card->driver_data = data;
}

void *ofono_handsfree_card_get_data(struct ofono_handsfree_card *card)
{
	return card->driver_data;
}

void ofono_handsfree_card_set_remote(struct ofono_handsfree_card *card,
					const char *remote)
{
	if (card->remote)
		g_free(card->remote);

	card->remote = g_strdup(remote);
}

const char *ofono_handsfree_card_get_remote(struct ofono_handsfree_card *card)
{
	return card->remote;
}

void ofono_handsfree_card_set_local(struct ofono_handsfree_card *card,
					const char *local)
{
	if (card->local)
		g_free(card->local);

	card->local = g_strdup(local);
}

const char *ofono_handsfree_card_get_local(struct ofono_handsfree_card *card)
{
	return card->local;
}

int ofono_handsfree_card_connect_sco(struct ofono_handsfree_card *card)
{
	GIOChannel *io;
	struct sockaddr_sco addr;
	int sk, ret;

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET | O_NONBLOCK | SOCK_CLOEXEC,
								BTPROTO_SCO);
	if (sk < 0)
		return -1;

	/* Bind to local address */
	memset(&addr, 0, sizeof(addr));
	addr.sco_family = AF_BLUETOOTH;
	bt_str2ba(card->local, &addr.sco_bdaddr);

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		ofono_error("Could not bind SCO socket");
		close(sk);
		return -1;
	}

	/* Connect to remote device */
	memset(&addr, 0, sizeof(addr));
	addr.sco_family = AF_BLUETOOTH;
	bt_str2ba(card->remote, &addr.sco_bdaddr);

	ret = connect(sk, (struct sockaddr *) &addr, sizeof(addr));
	if (ret < 0 && errno != EINPROGRESS) {
		close(sk);
		return -1;
	}

	io = g_io_channel_unix_new(sk);
	g_io_add_watch(io, G_IO_OUT | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
						sco_connect_cb, card);
	g_io_channel_unref(io);

	return 0;
}

static void emit_card_added(struct ofono_handsfree_card *card)
{
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *path;

	signal = dbus_message_new_signal(OFONO_MANAGER_PATH,
						HFP_AUDIO_MANAGER_INTERFACE,
						"CardAdded");

	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);

	path = card->path;
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	card_append_properties(card, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(ofono_dbus_get_connection(), signal);
}

int ofono_handsfree_card_register(struct ofono_handsfree_card *card)
{
	static int next_card_id = 1;
	char path[64];

	if (card == NULL)
		return -EINVAL;

	snprintf(path, sizeof(path), "/card_%d", next_card_id);

	if (!g_dbus_register_interface(ofono_dbus_get_connection(), path,
					HFP_AUDIO_CARD_INTERFACE,
					card_methods, card_signals, NULL,
					card, NULL))
		return -EIO;

	next_card_id += 1;

	card->path = g_strdup(path);
	emit_card_added(card);

	return 0;
}

static void emit_card_removed(struct ofono_handsfree_card *card)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = card->path;

	g_dbus_emit_signal(conn, OFONO_MANAGER_PATH,
				HFP_AUDIO_MANAGER_INTERFACE,
				"CardRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
}

static void card_unregister(struct ofono_handsfree_card *card)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	g_dbus_unregister_interface(conn, card->path, HFP_AUDIO_CARD_INTERFACE);

	emit_card_removed(card);

	g_free(card->path);
	card->path = NULL;
}

void ofono_handsfree_card_remove(struct ofono_handsfree_card *card)
{
	DBG("%p", card);

	if (card == NULL)
		return;

	if (card->path)
		card_unregister(card);

	card_list = g_slist_remove(card_list, card);

	g_free(card->remote);
	g_free(card->local);

	if (card->driver && card->driver->remove)
		card->driver->remove(card);

	g_free(card);
}

ofono_bool_t ofono_handsfree_card_set_codec(struct ofono_handsfree_card *card,
							unsigned char codec)
{
	if (codec == HFP_CODEC_CVSD)
		goto done;

	if (codec == HFP_CODEC_MSBC && has_wideband)
		goto done;

	return FALSE;

done:
	card->selected_codec = codec;

	return TRUE;
}

ofono_bool_t ofono_handsfree_audio_has_wideband(void)
{
	return has_wideband;
}

ofono_bool_t ofono_handsfree_audio_has_transparent_sco(void)
{
	return transparent_sco;
}

static void agent_free(struct agent *agent)
{
	if (agent->watch > 0)
		g_dbus_remove_watch(ofono_dbus_get_connection(), agent->watch);

	g_free(agent->owner);
	g_free(agent->path);
	g_free(agent);
}

static void agent_release(struct agent *agent)
{
	DBusMessage *msg;

	msg = dbus_message_new_method_call(agent->owner, agent->path,
					HFP_AUDIO_AGENT_INTERFACE, "Release");

	g_dbus_send_message(ofono_dbus_get_connection(), msg);
}

static void agent_disconnect(DBusConnection *conn, void *user_data)
{
	DBG("Agent %s disconnected", agent->owner);

	agent_free(agent);
	agent = NULL;

	has_wideband = FALSE;
}

static void append_card(void *data, void *userdata)
{
	struct ofono_handsfree_card *card = data;
	struct DBusMessageIter *array = userdata;
	DBusMessageIter entry, dict;

	dbus_message_iter_open_container(array, DBUS_TYPE_STRUCT,
						NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
					&card->path);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
				OFONO_PROPERTIES_ARRAY_SIGNATURE,
				&dict);

	card_append_properties(card, &dict);

	dbus_message_iter_close_container(&entry, &dict);
	dbus_message_iter_close_container(array, &entry);
}

static DBusMessage *am_get_cards(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_OBJECT_PATH_AS_STRING
					DBUS_TYPE_ARRAY_AS_STRING
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING
					DBUS_STRUCT_END_CHAR_AS_STRING,
					&array);
	g_slist_foreach(card_list, append_card, &array);
	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static DBusMessage *am_agent_register(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	const char *sender, *path;
	unsigned char *codecs;
	DBusMessageIter iter, array;
	int length, i;
	gboolean has_cvsd = FALSE, has_msbc = FALSE;

	if (agent)
		return __ofono_error_in_use(msg);

	sender = dbus_message_get_sender(msg);

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &path);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &array);
	dbus_message_iter_get_fixed_array(&array, &codecs, &length);

	if (length == 0)
		return __ofono_error_invalid_args(msg);

	for (i = 0; i < length; i++) {
		if (codecs[i] == HFP_CODEC_CVSD)
			has_cvsd = TRUE;
		else if (codecs[i] == HFP_CODEC_MSBC)
			has_msbc = TRUE;
		else
			return __ofono_error_invalid_args(msg);
	}

	DBG("Agent %s registered with the CODECs:%s%s", sender,
		has_cvsd ? " CVSD" : "", has_msbc ? " mSBC" : "");

	if (has_msbc && transparent_sco)
		has_wideband = TRUE;
	else {
		has_wideband = FALSE;
		DBG("Wideband speech disabled: %s", has_msbc ?
			"no Transparent SCO support" : "no mSBC support");
	}

	if (has_cvsd == FALSE) {
		ofono_error("CVSD codec is mandatory");
		return __ofono_error_invalid_args(msg);
	}

	agent = g_new0(struct agent, 1);
	agent->owner = g_strdup(sender);
	agent->path = g_strdup(path);
	agent->watch = g_dbus_add_disconnect_watch(conn, sender,
						agent_disconnect, NULL, NULL);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *am_agent_unregister(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	const char *sender, *path;
	DBusMessageIter iter;

	if (agent == NULL)
		return __ofono_error_not_found(msg);

	sender = dbus_message_get_sender(msg);

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &path);

	if (strcmp(sender, agent->owner) != 0)
		return __ofono_error_not_allowed(msg);

	if (strcmp(path, agent->path) != 0)
		return __ofono_error_not_found(msg);

	agent_free(agent);
	agent = NULL;

	has_wideband = FALSE;

	DBG("Agent %s unregistered", sender);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable am_methods[] = {
	{ GDBUS_METHOD("GetCards",
			NULL, GDBUS_ARGS({"cards", "a{oa{sv}}"}),
			am_get_cards) } ,
	{ GDBUS_METHOD("Register",
			GDBUS_ARGS({"path", "o"}, {"codecs", "ay"}), NULL,
			am_agent_register) },
	{ GDBUS_METHOD("Unregister",
			GDBUS_ARGS({"path", "o"}), NULL,
			am_agent_unregister) },
	{ }
};

static const GDBusSignalTable am_signals[] = {
	{ GDBUS_SIGNAL("CardAdded",
		GDBUS_ARGS({ "path", "o" }, { "properties", "a{sv}" })) },
	{ GDBUS_SIGNAL("CardRemoved",
		GDBUS_ARGS({ "path", "o" })) },
	{ }
};

int ofono_handsfree_card_driver_register(
				const struct ofono_handsfree_card_driver *d)
{
	DBG("driver: %p", d);

	if (d->probe == NULL)
		return -EINVAL;

	drivers = g_slist_prepend(drivers, (void *) d);

	return 0;
}

void ofono_handsfree_card_driver_unregister(
				const struct ofono_handsfree_card_driver *d)
{
	DBG("driver: %p", d);

	drivers = g_slist_remove(drivers, (void *) d);
}

void ofono_handsfree_audio_ref(void)
{
	ref_count += 1;

	if (ref_count != 1)
		return;

	if (!g_dbus_register_interface(ofono_dbus_get_connection(),
					OFONO_MANAGER_PATH,
					HFP_AUDIO_MANAGER_INTERFACE,
					am_methods, am_signals, NULL,
					NULL, NULL))
		ofono_error("Unable to register Handsfree Audio Manager");
}

void ofono_handsfree_audio_unref(void)
{
	if (ref_count == 0) {
		ofono_error("Error in handsfree audio manager ref counting");
		return;
	}

	ref_count -= 1;

	if (ref_count > 0)
		return;

	g_dbus_unregister_interface(ofono_dbus_get_connection(),
					OFONO_MANAGER_PATH,
					HFP_AUDIO_MANAGER_INTERFACE);

	if (agent) {
		agent_release(agent);
		agent_free(agent);
	}
}

int __ofono_handsfree_audio_manager_init(void)
{
	return sco_init();
}

void __ofono_handsfree_audio_manager_cleanup(void)
{
	if (ref_count == 0)
		return;

	ofono_error("Handsfree Audio manager not cleaned up properly,"
			"fixing...");

	ref_count = 1;
	ofono_handsfree_audio_unref();

	if (sco_watch > 0)
		g_source_remove(sco_watch);
}
