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

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/emulator.h>

#include "ofono.h"

#define DUN_PORT 12346
#define HFP_PORT 12347

static unsigned int modemwatch_id;
guint server_watch;
static GList *modems;

static gboolean on_socket_connected(GIOChannel *chan, GIOCondition cond,
							gpointer user)
{
	struct sockaddr saddr;
	unsigned int len = sizeof(saddr);
	int fd;
	struct ofono_emulator *em;
	struct ofono_modem *modem;

	if (cond != G_IO_IN)
		return FALSE;

	fd = accept(g_io_channel_unix_get_fd(chan), &saddr, &len);
	if (fd == -1)
		return FALSE;

	/* Pick the first powered modem */
	modem = modems->data;
	DBG("Picked modem %p for emulator", modem);

	em = ofono_emulator_create(modem, GPOINTER_TO_INT(user));
	if (em == NULL)
		close(fd);
	else
		ofono_emulator_register(em, fd);

	return TRUE;
}

static gboolean create_tcp(short port, enum ofono_emulator_type type)
{
	struct sockaddr_in addr;
	int sk;
	int reuseaddr = 1;
	GIOChannel *server;

	sk = socket(PF_INET, SOCK_STREAM, 0);
	if (sk < 0)
		return FALSE;

	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));

	if (bind(sk, (struct sockaddr *) &addr, sizeof(struct sockaddr)) < 0)
		goto err;

	if (listen(sk, 1) < 0)
		goto err;

	server = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(server, TRUE);

	server_watch = g_io_add_watch_full(server, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, GINT_TO_POINTER(type),
				NULL);

	g_io_channel_unref(server);

	DBG("Created server_watch: %u", server_watch);

	return TRUE;

err:
	close(sk);
	return FALSE;
}

static void powered_watch(struct ofono_modem *modem, gboolean powered,
				void *user)
{
	if (powered == FALSE) {
		DBG("Removing modem %p from the list", modem);
		modems = g_list_remove(modems, modem);

		if (modems == NULL && server_watch > 0) {
			DBG("Removing server watch: %u", server_watch);
			g_source_remove(server_watch);
			server_watch = 0;
		}
	} else {
		DBG("Adding modem %p to the list", modem);
		modems = g_list_append(modems, modem);

		if (modems->next == NULL) {
			create_tcp(DUN_PORT, OFONO_EMULATOR_TYPE_DUN);
			create_tcp(HFP_PORT, OFONO_EMULATOR_TYPE_HFP);
		}
	}
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	DBG("modem: %p, added: %d", modem, added);

	if (added == FALSE) {
		DBG("Removing modem %p from the list", modem);
		modems = g_list_remove(modems, modem);
		return;
	}

	if (ofono_modem_get_powered(modem) == TRUE) {
		DBG("Adding modem %p to the list", modem);
		modems = g_list_append(modems, modem);

		if (modems->next == NULL) {
			create_tcp(DUN_PORT, OFONO_EMULATOR_TYPE_DUN);
			create_tcp(HFP_PORT, OFONO_EMULATOR_TYPE_HFP);
		}
	}

	__ofono_modem_add_powered_watch(modem, powered_watch, NULL, NULL);
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int example_emulator_init(void)
{
	DBG("");

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);

	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void example_emulator_exit(void)
{
	DBG("");

	__ofono_modemwatch_remove(modemwatch_id);

	g_list_free(modems);

	if (server_watch)
		g_source_remove(server_watch);
}

OFONO_PLUGIN_DEFINE(example_emulator, "Example AT Modem Emulator Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			example_emulator_init, example_emulator_exit)
