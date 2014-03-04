/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical, Ltd. All rights reserved.
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
#include <gril.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/types.h>

#include "rilmodem.h"

static int rilmodem_init(void)
{
	DBG("");

	ril_devinfo_init();
	ril_sim_init();
	ril_voicecall_init();
	ril_sms_init();
	ril_netreg_init();
	ril_call_volume_init();
	ril_gprs_init();
	ril_gprs_context_init();
	ril_ussd_init();
	ril_call_settings_init();
	ril_call_forwarding_init();

	return 0;
}

static void rilmodem_exit(void)
{
	DBG("");

	ril_devinfo_exit();
	ril_sim_exit();
	ril_voicecall_exit();
	ril_sms_exit();
	ril_netreg_exit();
	ril_call_volume_exit();
	ril_gprs_exit();
	ril_gprs_context_exit();
	ril_ussd_exit();
	ril_call_settings_exit();
	ril_call_forwarding_exit();
}

OFONO_PLUGIN_DEFINE(rilmodem, "RIL modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, rilmodem_init, rilmodem_exit)
