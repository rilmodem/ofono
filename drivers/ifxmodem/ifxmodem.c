/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#include "ifxmodem.h"

static int ifxmodem_init(void)
{
	ifx_voicecall_init();
	ifx_audio_settings_init();
	ifx_radio_settings_init();
	ifx_gprs_context_init();
	ifx_stk_init();
	ifx_ctm_init();

	return 0;
}

static void ifxmodem_exit(void)
{
	ifx_stk_exit();
	ifx_gprs_context_exit();
	ifx_radio_settings_exit();
	ifx_audio_settings_exit();
	ifx_voicecall_exit();
	ifx_ctm_exit();
}

OFONO_PLUGIN_DEFINE(ifxmodem, "Infineon modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			ifxmodem_init, ifxmodem_exit)
