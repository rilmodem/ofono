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

#include "hsomodem.h"

static int hsomodem_init(void)
{
	hso_gprs_context_init();
	hso_radio_settings_init();

	return 0;
}

static void hsomodem_exit(void)
{
	hso_gprs_context_exit();
	hso_radio_settings_exit();
}

OFONO_PLUGIN_DEFINE(hsomodem, "HSO modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			hsomodem_init, hsomodem_exit)
