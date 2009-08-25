/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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
#include <glib.h>
#include <gdbus.h>
#include <gatchat.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include "at.h"

void dump_response(const char *func, gboolean ok, GAtResult *result)
{
	GSList *l;

	ofono_debug("%s got result: %d", func, ok);
	ofono_debug("Final response: %s", result->final_or_pdu);

	for (l = result->lines; l; l = l->next)
		ofono_debug("Response line: %s", (char *) l->data);
}

void decode_at_error(struct ofono_error *error, const char *final)
{
	if (!strcmp(final, "OK")) {
		error->type = OFONO_ERROR_TYPE_NO_ERROR;
		error->error = 0;
	} else {
		error->type = OFONO_ERROR_TYPE_FAILURE;
		error->error = 0;
	}
}

static int atmodem_init(void)
{
	at_voicecall_init();
	at_devinfo_init();
	at_call_barring_init();
	at_call_forwarding_init();
	at_call_meter_init();
	at_call_settings_init();
	at_phonebook_init();
	at_ssn_init();
	at_ussd_init();
	at_sms_init();
	at_sim_init();
	at_netreg_init();

	return 0;
}

static void atmodem_exit(void)
{
	at_sim_exit();
	at_sms_exit();
	at_ussd_exit();
	at_ssn_exit();
	at_phonebook_exit();
	at_call_settings_exit();
	at_call_meter_exit();
	at_call_forwarding_exit();
	at_call_barring_exit();
	at_netreg_exit();
	at_devinfo_exit();
	at_voicecall_exit();
}

OFONO_PLUGIN_DEFINE(atmodem, "AT modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, atmodem_init, atmodem_exit)
