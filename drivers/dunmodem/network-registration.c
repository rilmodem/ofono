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

#include <glib.h>
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "common.h"
#include "dunmodem.h"

static const char *cops_prefix[] = { "+COPS:", NULL };

struct netreg_data {
	GAtChat *chat;
};

static void dun_registration_status(struct ofono_netreg *netreg,
				ofono_netreg_status_cb_t cb, void *data)
{
	int status = NETWORK_REGISTRATION_STATUS_REGISTERED;

	DBG("");

	CALLBACK_WITH_SUCCESS(cb, status, -1, -1, -1, data);
}

static void dun_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct ofono_network_operator op;

	DBG("");

	op.name[0] = '\0';
	op.mcc[0] = '\0';
	op.mnc[0] = '\0';
	op.status = 2;
	op.tech = -1;

	CALLBACK_WITH_SUCCESS(cb, &op, data);
}

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;

	if (!ok)
		return;

	ofono_netreg_register(netreg);
}

static int dun_netreg_probe(struct ofono_netreg *netreg,
				unsigned int vendor, void *user_data)
{
	GAtChat *chat = user_data;
	struct netreg_data *nd;

	nd = g_try_new0(struct netreg_data, 1);
	if (nd == NULL)
		return -ENOMEM;

	nd->chat = g_at_chat_clone(chat);

	ofono_netreg_set_data(netreg, nd);

	g_at_chat_send(nd->chat, "AT+COPS=0", cops_prefix,
					cops_cb, netreg, NULL);

	return 0;
}

static void dun_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	ofono_netreg_set_data(netreg, NULL);

	g_free(nd);
}

static struct ofono_netreg_driver driver = {
	.name			= "dunmodem",
	.probe			= dun_netreg_probe,
	.remove			= dun_netreg_remove,
	.registration_status	= dun_registration_status,
	.current_operator	= dun_current_operator,
};

void dun_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void dun_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
