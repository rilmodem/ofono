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

#include <glib.h>

#include "ofono.h"
#include "gatserver.h"

struct ofono_emulator {
	struct ofono_atom *atom;
	GAtServer *server;
};

static void emulator_debug(const char *str, void *data)
{
	ofono_info("%s: %s\n", (char *)data, str);
}

static void emulator_disconnect(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	DBG("%p", em);

	ofono_emulator_remove(em);
}

static void emulator_unregister(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	DBG("%p", em);

	g_at_server_unref(em->server);
	em->server = NULL;
}

void ofono_emulator_register(struct ofono_emulator *em, int fd)
{
	GIOChannel *io;

	DBG("%p, %d", em, fd);

	if (fd < 0)
		return;

	io = g_io_channel_unix_new(fd);

	em->server = g_at_server_new(io);
	if (em->server == NULL)
		return;

	g_at_server_set_debug(em->server, emulator_debug, "Server");
	g_at_server_set_disconnect_function(em->server,
						emulator_disconnect, em);

	__ofono_atom_register(em->atom, emulator_unregister);
}

static void emulator_remove(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	g_free(em);
}

struct ofono_emulator *ofono_emulator_create(struct ofono_modem *modem,
						enum ofono_emulator_type type)
{
	struct ofono_emulator *em;

	DBG("modem: %p, type: %d", modem, type);

	em = g_try_new0(struct ofono_emulator, 1);

	if (em == NULL)
		return NULL;

	em->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_EMULATOR_DUN,
						emulator_remove, em);

	return em;
}

void ofono_emulator_remove(struct ofono_emulator *em)
{
	__ofono_atom_free(em->atom);
}
