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
#include <string.h>

#include <gdbus.h>

#include "ofono.h"

#define HFP_AUDIO_MANAGER_INTERFACE		OFONO_SERVICE ".HandsfreeAudioManager"
#define HFP_AUDIO_AGENT_INTERFACE		OFONO_SERVICE ".HandsfreeAudioAgent"

/* Supported agent codecs */
enum hfp_codec {
	HFP_CODEC_CVSD = 0x01,
	HFP_CODEC_MSBC = 0x02,
};

struct agent {
	char *owner;
	char *path;
	unsigned char *codecs;
	int codecs_len;
};

static struct agent *agent = NULL;

static void agent_free(struct agent *agent)
{
	g_free(agent->owner);
	g_free(agent->path);
	g_free(agent->codecs);
	g_free(agent);
}

static void agent_release(struct agent *agent)
{
	DBusMessage *msg;

	msg = dbus_message_new_method_call(agent->owner, agent->path,
					HFP_AUDIO_AGENT_INTERFACE, "Release");

	g_dbus_send_message(ofono_dbus_get_connection(), msg);
}

static DBusMessage *am_get_cards(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	return __ofono_error_not_implemented(msg);
}

static DBusMessage *am_agent_register(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	const char *sender, *path;
	unsigned char *codecs;
	DBusMessageIter iter, array;
	int length, i;

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
		if (codecs[i] != HFP_CODEC_CVSD &&
				codecs[i] != HFP_CODEC_MSBC)
			return __ofono_error_invalid_args(msg);
	}

	agent = g_new0(struct agent, 1);
	agent->owner = g_strdup(sender);
	agent->path = g_strdup(path);
	agent->codecs = g_memdup(codecs, length);
	agent->codecs_len = length;

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

int __ofono_handsfree_audio_manager_init(void)
{
	if (!g_dbus_register_interface(ofono_dbus_get_connection(),
					"/", HFP_AUDIO_MANAGER_INTERFACE,
					am_methods, NULL, NULL, NULL, NULL)) {
		return -EIO;
	}

	return 0;
}

void __ofono_handsfree_audio_manager_cleanup(void)
{
	g_dbus_unregister_interface(ofono_dbus_get_connection(), "/",
						HFP_AUDIO_MANAGER_INTERFACE);

	if (agent) {
		agent_release(agent);
		agent_free(agent);
	}
}
