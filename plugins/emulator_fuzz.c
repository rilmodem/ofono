/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
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
#include <ofono/emulator.h>

#include "hfp.h"

#define EMULATOR_FUZZ_INTERFACE "org.ofono.test.EmulatorFuzz"
#define EMULATOR_FUZZ_PATH   "/test"

static void emulator_set_indicator(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	ofono_bool_t active = GPOINTER_TO_INT(data);

	ofono_emulator_set_hf_indicator_active(em,
				HFP_HF_INDICATOR_ENHANCED_SAFETY, active);
}

static void modem_set_indicators(struct ofono_modem *modem, void *user)
{
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_set_indicator,
						user);
}

static DBusMessage *set_indicator_active(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	const char *indicator;
	dbus_bool_t active;

	DBG("");

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &indicator,
						DBUS_TYPE_BOOLEAN, &active,
						DBUS_TYPE_INVALID) == FALSE)
		goto invalid;

	DBG("%s,%d", indicator, active);

	if (strcmp(indicator, "DistractedDrivingReduction"))
		goto invalid;

	__ofono_modem_foreach(modem_set_indicators, GINT_TO_POINTER(active));

	return dbus_message_new_method_return(msg);

invalid:
	return g_dbus_create_error(msg, "org.ofono.test.Error",
					"Invalid arguments in method call");
}

static const GDBusMethodTable emulator_fuzz_methods[] = {
	{ GDBUS_ASYNC_METHOD("SetIndicatorActive",
		GDBUS_ARGS({ "indicator", "s" }, { "active", "b" }),
		NULL, set_indicator_active) },
	{ },
};

static int emulator_fuzz_init(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("");

	if (!g_dbus_register_interface(conn, EMULATOR_FUZZ_PATH,
					EMULATOR_FUZZ_INTERFACE,
					emulator_fuzz_methods, NULL,
					NULL, NULL, NULL)) {
		ofono_error("Register Profile interface failed: %s",
						EMULATOR_FUZZ_PATH);
		return -EIO;
	}

	return 0;
}

static void emulator_fuzz_exit(void)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("");

	g_dbus_unregister_interface(conn, EMULATOR_FUZZ_PATH,
						EMULATOR_FUZZ_INTERFACE);
}

OFONO_PLUGIN_DEFINE(emulator_fuzz, "Emulator Fuzz",
				VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
				emulator_fuzz_init, emulator_fuzz_exit)
