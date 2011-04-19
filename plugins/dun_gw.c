/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Intel Corporation. All rights reserved.
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
#include <errno.h>
#include <glib.h>
#include <ofono.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <gdbus.h>

#include "bluetooth.h"

#define DUN_GW_CHANNEL	1

static struct server *server;
static guint modemwatch_id;
static GList *modems;

static const gchar *dun_record =
"<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
"<record>\n"
"  <attribute id=\"0x0001\">\n"
"    <sequence>\n"
"      <uuid value=\"0x1103\"/>\n"
"    </sequence>\n"
"  </attribute>\n"
"\n"
"  <attribute id=\"0x0004\">\n"
"    <sequence>\n"
"      <sequence>\n"
"        <uuid value=\"0x0100\"/>\n"
"      </sequence>\n"
"      <sequence>\n"
"        <uuid value=\"0x0003\"/>\n"
"        <uint8 value=\"1\" name=\"channel\"/>\n"
"      </sequence>\n"
"    </sequence>\n"
"  </attribute>\n"
"\n"
"  <attribute id=\"0x0009\">\n"
"    <sequence>\n"
"      <sequence>\n"
"        <uuid value=\"0x1103\"/>\n"
"        <uint16 value=\"0x0100\" name=\"version\"/>\n"
"      </sequence>\n"
"    </sequence>\n"
"  </attribute>\n"
"\n"
"  <attribute id=\"0x0100\">\n"
"    <text value=\"Dial-up Networking\" name=\"name\"/>\n"
"  </attribute>\n"
"</record>\n";

static void dun_gw_connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	struct ofono_modem *modem;
	int fd;

	DBG("");

	if (err) {
		DBG("%s", err->message);
		g_io_channel_shutdown(io, TRUE, NULL);
		return;
	}

	/* Pick the first powered modem */
	modem = modems->data;
	DBG("Picked modem %p for emulator", modem);

	em = ofono_emulator_create(modem, OFONO_EMULATOR_TYPE_DUN);
	if (em == NULL) {
		g_io_channel_shutdown(io, TRUE, NULL);
		return;
	}

	fd = g_io_channel_unix_get_fd(io);
	g_io_channel_set_close_on_unref(io, FALSE);

	ofono_emulator_register(em, fd);
}

static void gprs_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_modem *modem = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		modems = g_list_append(modems, modem);

		if (modems->next == NULL)
			server = bluetooth_register_server(DUN_GW_CHANNEL,
					dun_record,
					dun_gw_connect_cb,
					NULL);
	} else {
		modems = g_list_remove(modems, modem);
		if (modems == NULL && server != NULL) {
			bluetooth_unregister_server(server);
			server = NULL;
		}
	}
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	DBG("modem: %p, added: %d", modem, added);

	if (added == FALSE)
		return;

	__ofono_modem_add_atom_watch(modem, OFONO_ATOM_TYPE_GPRS,
						gprs_watch, modem, NULL);
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int dun_gw_init(void)
{
	DBG("");

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);

	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void dun_gw_exit(void)
{
	__ofono_modemwatch_remove(modemwatch_id);
	g_list_free(modems);

	if (server) {
		bluetooth_unregister_server(server);
		server = NULL;
	}
}

OFONO_PLUGIN_DEFINE(dun_gw, "Dial-up Networking Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, dun_gw_init, dun_gw_exit)
