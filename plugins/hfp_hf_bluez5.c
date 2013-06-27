/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013 Intel Corporation. All rights reserved.
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
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <glib.h>

#include <gdbus.h>
#include <gatchat.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/dbus.h>
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/call-volume.h>
#include <ofono/handsfree.h>
#include <ofono/handsfree-audio.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/hfpmodem/slc.h>

#include "bluetooth.h"
#include "hfp.h"
#include "bluez5.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define HFP_EXT_PROFILE_PATH   "/bluetooth/profile/hfp_hf"

#define HFP16_HF_DRIVER		"hfp16-hf-driver"

struct hfp {
	struct hfp_slc_info info;
	DBusMessage *msg;
	struct ofono_handsfree_card *card;
};

static GDBusClient *bluez = NULL;

static void hfp_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void slc_established(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct hfp *hfp = ofono_modem_get_data(modem);
	DBusMessage *msg;

	ofono_modem_set_powered(modem, TRUE);

	msg = dbus_message_new_method_return(hfp->msg);
	g_dbus_send_message(ofono_dbus_get_connection(), msg);
	dbus_message_unref(hfp->msg);
	hfp->msg = NULL;

	ofono_info("Service level connection established");

	ofono_handsfree_card_register(hfp->card);
}

static void slc_failed(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;
	DBusMessage *msg;

	msg = g_dbus_create_error(hfp->msg, BLUEZ_ERROR_INTERFACE
						".Failed",
						"HFP Handshake failed");

	g_dbus_send_message(ofono_dbus_get_connection(), msg);
	dbus_message_unref(hfp->msg);
	hfp->msg = NULL;

	ofono_error("Service level connection failed");
	ofono_modem_set_powered(modem, FALSE);

	ofono_handsfree_card_remove(hfp->card);
	hfp->card = NULL;

	g_at_chat_unref(info->chat);
	info->chat = NULL;
}

static void hfp_disconnected_cb(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;

	DBG("HFP disconnected");

	ofono_modem_set_powered(modem, FALSE);

	ofono_handsfree_card_remove(hfp->card);
	hfp->card = NULL;

	g_at_chat_unref(info->chat);
	info->chat = NULL;
}

static int service_level_connection(struct ofono_modem *modem,
						int fd, guint16 version)
{
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;
	GIOChannel *io;
	GAtSyntax *syntax;
	GAtChat *chat;

	io = g_io_channel_unix_new(fd);

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_channel_unref(io);

	if (chat == NULL)
		return -ENOMEM;

	g_at_chat_set_disconnect_function(chat, hfp_disconnected_cb, modem);

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, hfp_debug, "");

	hfp_slc_info_init(info, version);
	info->chat = chat;

	hfp_slc_establish(info, slc_established, slc_failed, modem);

	return -EINPROGRESS;
}

static struct ofono_modem *modem_register(const char *device,
				const char *device_address, const char *alias)
{
	struct ofono_modem *modem;
	char *path;

	path = g_strconcat("hfp", device, NULL);

	modem = ofono_modem_create(path, "hfp");

	g_free(path);

	if (modem == NULL)
		return NULL;

	ofono_modem_set_string(modem, "Remote", device_address);
	ofono_modem_set_string(modem, "DevicePath", device);

	ofono_modem_set_name(modem, alias);
	ofono_modem_register(modem);

	return modem;
}

static int hfp_probe(struct ofono_modem *modem)
{
	struct hfp *hfp;

	DBG("modem: %p", modem);

	hfp = g_new0(struct hfp, 1);

	ofono_modem_set_data(modem, hfp);

	return 0;
}

static void hfp_remove(struct ofono_modem *modem)
{
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;

	DBG("modem: %p", modem);

	if (hfp->msg)
		dbus_message_unref(hfp->msg);

	g_at_chat_unref(info->chat);

	g_free(hfp);

	ofono_modem_set_data(modem, NULL);
}

static void connect_cb(gboolean success, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	if (success)
		return;

	ofono_modem_set_powered(modem, FALSE);
}

/* power up hardware */
static int hfp_enable(struct ofono_modem *modem)
{
	const char *path;

	DBG("%p", modem);

	path = ofono_modem_get_string(modem, "DevicePath");

	/*
	 * We call Device1.ConnectProfile() with our UUID, and we hope for the
	 * NewConnection() method to be called, if ConnectProfile() fails we
	 * force the modem to powered off
	 */
	bt_connect_profile(ofono_dbus_get_connection(), path, HFP_AG_UUID,
							connect_cb, modem);

	return -EINPROGRESS;
}

static int hfp_disable(struct ofono_modem *modem)
{
	struct hfp *hfp = ofono_modem_get_data(modem);
	struct hfp_slc_info *info = &hfp->info;
	GIOChannel *channel;
	int fd;

	DBG("%p", modem);

	/*
	 * Instead of triggering two round trips to BlueZ (DisconnectProfile,
	 * RequestDisconnection) simply kill the connection on the RFCOMM fd
	 * we already have.  But for this we have to call shutdown().
	 */
	channel = g_at_chat_get_channel(info->chat);
	fd = g_io_channel_unix_get_fd(channel);
	shutdown(fd, SHUT_RDWR);

	ofono_handsfree_card_remove(hfp->card);
	hfp->card = NULL;

	g_at_chat_unref(info->chat);
	info->chat = NULL;

	return 0;
}

static void hfp_pre_sim(struct ofono_modem *modem)
{
	struct hfp *hfp = ofono_modem_get_data(modem);
	char *address = (char *) ofono_modem_get_string(modem, "Remote");

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "hfpmodem", address);
	ofono_voicecall_create(modem, 0, "hfpmodem", &hfp->info);
	ofono_netreg_create(modem, 0, "hfpmodem", &hfp->info);
	ofono_handsfree_create(modem, 0, "hfpmodem", &hfp->info);
	ofono_call_volume_create(modem, 0, "hfpmodem", &hfp->info);
}

static void hfp_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static struct ofono_modem_driver hfp_driver = {
	.name		= "hfp",
	.modem_type	= OFONO_MODEM_TYPE_HFP,
	.probe		= hfp_probe,
	.remove		= hfp_remove,
	.enable		= hfp_enable,
	.disable	= hfp_disable,
	.pre_sim	= hfp_pre_sim,
	.post_sim	= hfp_post_sim,
};

static void bcs_notify(GAtResult *result, gpointer user_data)
{
	struct hfp *hfp = user_data;
	struct hfp_slc_info *info = &hfp->info;
	GAtResultIter iter;
	char str[32];
	int value;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+BCS:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	if (ofono_handsfree_card_set_codec(hfp->card, value) == FALSE) {
		/* Unsupported codec, re-send our codecs */
		if (ofono_handsfree_audio_has_wideband())
			sprintf(str, "AT+BAC=%d,%d", HFP_CODEC_CVSD,
							HFP_CODEC_MSBC);
		else
			sprintf(str, "AT+BAC=%d", HFP_CODEC_CVSD);

		goto done;
	}

	/* Confirm the codec */
	sprintf(str, "AT+BCS=%d", value);

done:
	g_at_chat_send(info->chat, str, NULL, NULL, NULL, NULL);
}

static int hfp16_card_probe(struct ofono_handsfree_card *card,
					unsigned int vendor, void *data)
{
	struct hfp *hfp = data;
	struct hfp_slc_info *info = &hfp->info;

	g_at_chat_register(info->chat, "+BCS:", bcs_notify, FALSE,
								hfp, NULL);

	return 0;
}

static void hfp16_card_remove(struct ofono_handsfree_card *card)
{

}

static void bcc_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_handsfree_card_connect_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void hfp16_card_connect(struct ofono_handsfree_card *card,
					ofono_handsfree_card_connect_cb_t cb,
					void *data)
{
	struct hfp *hfp = ofono_handsfree_card_get_data(card);
	struct hfp_slc_info *info = &hfp->info;

	if (info->hf_features & HFP_HF_FEATURE_CODEC_NEGOTIATION &&
			info->ag_features & HFP_AG_FEATURE_CODEC_NEGOTIATION) {
		struct cb_data *cbd = cb_data_new(cb, data);

		g_at_chat_send(info->chat, "AT+BCC", NULL, bcc_cb, cbd, g_free);
		return;
	}

	/*
	 * If any side (remote or local) doesn't support codec negotiation,
	 * fallback to direct SCO connection. Calling connect_sco()
	 * hands the connection responsibility to the core, so no need
	 * to call the callback
	 */
	ofono_handsfree_card_connect_sco(card);
}

static struct ofono_handsfree_card_driver hfp16_hf_driver = {
	.name		= HFP16_HF_DRIVER,
	.probe		= hfp16_card_probe,
	.remove		= hfp16_card_remove,
	.connect	= hfp16_card_connect,
};

static ofono_bool_t device_path_compare(struct ofono_modem *modem,
					void *userdata)
{
	const char *path = userdata;
	const char *value = ofono_modem_get_string(modem, "DevicePath");

	if (value == NULL)
		return FALSE;

	return g_str_equal(path, value);
}

static int get_version(DBusMessageIter *iter, uint16_t *version)
{
	DBusMessageIter dict, entry, valiter;
	const char *key;
	uint16_t value;

	/* Points to dict */
	dbus_message_iter_recurse(iter, &dict);

	/* For each entry in this dict */
	while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
		/* I want to access the entry's contents */
		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			return -EINVAL;

		/* If the current key isn't "Version", keep looking */
		dbus_message_iter_get_basic(&entry, &key);
		if (!g_str_equal("Version", key)) {
			dbus_message_iter_next(&dict);
			continue;
		}

		dbus_message_iter_next(&entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			return -EINVAL;

		dbus_message_iter_recurse(&entry, &valiter);
		dbus_message_iter_get_basic(&valiter, &value);
		break;
	}

	if (version)
		*version = value;

	return 0;
}

static DBusMessage *profile_new_connection(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hfp *hfp;
	struct ofono_modem *modem;
	struct sockaddr_rc saddr;
	socklen_t optlen;
	DBusMessageIter entry;
	const char *device, *driver;
	char local[18], remote[18];
	uint16_t version = HFP_VERSION_1_5;
	int fd, err;

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
	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_ARRAY)
		goto invalid;

	if (get_version(&entry, &version) < 0)
		goto invalid;

	modem = ofono_modem_find(device_path_compare, (void *) device);
	if (modem == NULL) {
		close(fd);
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".Rejected",
					"Unknown Bluetooth device");
	}

	err = service_level_connection(modem, fd, version);
	if (err < 0 && err != -EINPROGRESS) {
		close(fd);
		return g_dbus_create_error(msg, BLUEZ_ERROR_INTERFACE
					".Rejected",
					"Not enough resources");
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

	hfp = ofono_modem_get_data(modem);
	hfp->msg = dbus_message_ref(msg);

	driver = NULL;

	if (version >= HFP_VERSION_1_6)
		driver = HFP16_HF_DRIVER;

	hfp->card = ofono_handsfree_card_create(0, driver, hfp);
	ofono_handsfree_card_set_data(hfp->card, hfp);

	ofono_handsfree_card_set_local(hfp->card, local);
	ofono_handsfree_card_set_remote(hfp->card, remote);

	return NULL;

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
	struct ofono_modem *modem;
	struct hfp *hfp;
	const char *device;
	struct hfp_slc_info *info;
	DBusMessageIter entry;

	DBG("Profile handler RequestDisconnection");

	if (dbus_message_iter_init(msg, &entry) == FALSE)
		goto error;

	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_OBJECT_PATH)
		goto error;

	dbus_message_iter_get_basic(&entry, &device);

	modem = ofono_modem_find(device_path_compare, (void *) device);
	if (modem == NULL)
		goto error;

	ofono_modem_set_powered(modem, FALSE);

	hfp = ofono_modem_get_data(modem);
	info = &hfp->info;

	ofono_handsfree_card_remove(hfp->card);
	hfp->card = NULL;

	g_at_chat_unref(info->chat);
	info->chat = NULL;

	return dbus_message_new_method_return(msg);

error:
	return g_dbus_create_error(msg,
			BLUEZ_ERROR_INTERFACE ".Rejected",
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

static void connect_handler(DBusConnection *conn, void *user_data)
{
	uint16_t features = HFP_SDP_HF_FEATURE_ECNR |
				HFP_SDP_HF_FEATURE_3WAY |
				HFP_SDP_HF_FEATURE_CLIP |
				HFP_SDP_HF_FEATURE_REMOTE_VOLUME_CONTROL;

	/*
	 * Assuming that if defer_setup is supported, then SCO transparent
	 * mode is also supported
	*/
	if (ofono_handsfree_audio_has_defer_setup())
		features |= HFP_SDP_HF_FEATURE_WIDEBAND_SPEECH;

	DBG("Registering External Profile handler ...");

	bt_register_profile(conn, HFP_HS_UUID, HFP_VERSION_1_6, "hfp_hf",
					HFP_EXT_PROFILE_PATH, NULL, features);
}

static gboolean has_hfp_ag_uuid(DBusMessageIter *array)
{
	DBusMessageIter value;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return FALSE;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
		const char *uuid;

		dbus_message_iter_get_basic(&value, &uuid);

		if (g_str_equal(uuid, HFP_AG_UUID) == TRUE)
			return TRUE;

		dbus_message_iter_next(&value);
	}

	return FALSE;
}

static void modem_removed(GDBusProxy *proxy, void *user_data)
{
	struct ofono_modem *modem = user_data;

	ofono_modem_remove(modem);
}

static void alias_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	const char *alias;
	struct ofono_modem *modem = user_data;

	if (g_str_equal("Alias", name) == FALSE)
		return;

	dbus_message_iter_get_basic(iter, &alias);
	ofono_modem_set_name(modem, alias);
}

static void modem_register_from_proxy(GDBusProxy *proxy, const char *path)
{
	const char *alias, *remote;
	DBusMessageIter iter;
	dbus_bool_t paired;
	struct ofono_modem *modem;

	if (g_dbus_proxy_get_property(proxy, "Paired", &iter) == FALSE)
		return;

	dbus_message_iter_get_basic(&iter, &paired);
	if (paired == FALSE)
		return;

	if (g_dbus_proxy_get_property(proxy, "UUIDs", &iter) == FALSE)
		return;

	if (has_hfp_ag_uuid(&iter) == FALSE)
		return;

	if (g_dbus_proxy_get_property(proxy, "Alias", &iter) == FALSE)
		return;

	dbus_message_iter_get_basic(&iter, &alias);

	if (g_dbus_proxy_get_property(proxy, "Address", &iter) == FALSE)
		return;

	dbus_message_iter_get_basic(&iter, &remote);

	modem = modem_register(path, remote, alias);
	g_dbus_proxy_set_property_watch(proxy, alias_changed, modem);
	g_dbus_proxy_set_removed_watch(proxy, modem_removed, modem);
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	if (g_str_equal(BLUEZ_DEVICE_INTERFACE, interface) == FALSE)
		return;

	modem_register_from_proxy(proxy, path);
}

static void property_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	if (g_str_equal(BLUEZ_DEVICE_INTERFACE, interface) == FALSE)
		return;

	if (g_str_equal("Paired", name) != TRUE)
		return;

	modem_register_from_proxy(proxy, path);
}

static int hfp_init(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	/* Registers External Profile handler */
	if (!g_dbus_register_interface(conn, HFP_EXT_PROFILE_PATH,
					BLUEZ_PROFILE_INTERFACE,
					profile_methods, NULL,
					NULL, NULL, NULL)) {
		ofono_error("Register Profile interface failed: %s",
						HFP_EXT_PROFILE_PATH);
		return -EIO;
	}

	err = ofono_handsfree_card_driver_register(&hfp16_hf_driver);
	if (err < 0) {
		g_dbus_unregister_interface(conn, HFP_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);
		return err;
	}

	err = ofono_modem_driver_register(&hfp_driver);
	if (err < 0) {
		g_dbus_unregister_interface(conn, HFP_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);
		return err;
	}

	bluez = g_dbus_client_new(conn, BLUEZ_SERVICE, BLUEZ_MANAGER_PATH);
	if (bluez == NULL) {
		g_dbus_unregister_interface(conn, HFP_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);
		ofono_modem_driver_unregister(&hfp_driver);
		return -ENOMEM;
	}

	g_dbus_client_set_connect_watch(bluez, connect_handler, NULL);
	g_dbus_client_set_proxy_handlers(bluez, proxy_added, NULL,
						property_changed, NULL);

	ofono_handsfree_audio_ref();

	return 0;
}

static void hfp_exit(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	bt_unregister_profile(conn, HFP_EXT_PROFILE_PATH);
	g_dbus_unregister_interface(conn, HFP_EXT_PROFILE_PATH,
						BLUEZ_PROFILE_INTERFACE);

	ofono_handsfree_card_driver_unregister(&hfp16_hf_driver);

	ofono_modem_driver_unregister(&hfp_driver);
	g_dbus_client_unref(bluez);

	ofono_handsfree_audio_unref();
}

OFONO_PLUGIN_DEFINE(hfp_bluez5, "External Hands-Free Profile Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
