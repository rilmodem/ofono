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

#include "huaweimodem.h"

static int huaweimodem_init(void)
{
	huawei_voicecall_init();
	huawei_audio_settings_init();
	huawei_radio_settings_init();
	huawei_gprs_context_init();

	return 0;
}

static void huaweimodem_exit(void)
{
	huawei_gprs_context_exit();
	huawei_radio_settings_exit();
	huawei_audio_settings_exit();
	huawei_voicecall_exit();
}

OFONO_PLUGIN_DEFINE(huaweimodem, "Huawei modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			huaweimodem_init, huaweimodem_exit)
