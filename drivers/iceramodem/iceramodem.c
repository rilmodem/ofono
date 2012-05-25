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
#include <gatchat.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/types.h>

#include "iceramodem.h"

static int iceramodem_init(void)
{
	icera_gprs_context_init();
	icera_radio_settings_init();

	return 0;
}

static void iceramodem_exit(void)
{
	icera_gprs_context_exit();
	icera_radio_settings_exit();
}

OFONO_PLUGIN_DEFINE(iceramodem, "Icera modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			iceramodem_init, iceramodem_exit)
