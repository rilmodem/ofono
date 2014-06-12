/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2014  Canonical, Ltd. All rights reserved.
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

#include "mtkmodem.h"

static int mtkmodem_init(void)
{
	DBG("");

	mtk_voicecall_init();
	mtk_gprs_init();
	mtk_radio_settings_init();

	return 0;
}

static void mtkmodem_exit(void)
{
	DBG("");

	mtk_voicecall_exit();
	mtk_gprs_exit();
	mtk_radio_settings_exit();
}

OFONO_PLUGIN_DEFINE(mtkmodem, "MTK modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, mtkmodem_init, mtkmodem_exit)
