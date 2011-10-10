/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>

#include "isimodem.h"

static int isimodem_init(void)
{
	isi_devinfo_init();
	isi_phonebook_init();
	isi_netreg_init();
	isi_voicecall_init();
	isi_sms_init();
	isi_cbs_init();
	isi_sim_init();
	isi_ussd_init();
	isi_call_forwarding_init();
	isi_call_settings_init();
	isi_call_barring_init();
	isi_call_meter_init();
	isi_radio_settings_init();
	isi_gprs_init();
	isi_gprs_context_init();
	isi_audio_settings_init();
	isi_uicc_init();

	return 0;
}

static void isimodem_exit(void)
{
	isi_devinfo_exit();
	isi_phonebook_exit();
	isi_netreg_exit();
	isi_voicecall_exit();
	isi_sms_exit();
	isi_cbs_exit();
	isi_sim_exit();
	isi_ussd_exit();
	isi_call_forwarding_exit();
	isi_call_settings_exit();
	isi_call_barring_exit();
	isi_call_meter_exit();
	isi_radio_settings_exit();
	isi_gprs_exit();
	isi_gprs_context_exit();
	isi_audio_settings_exit();
	isi_uicc_exit();
}

OFONO_PLUGIN_DEFINE(isimodem, "PhoNet / ISI modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, isimodem_init, isimodem_exit)
