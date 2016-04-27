/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical, Ltd. All rights reserved.
 *  Copyright (C) 2015 Ratchanan Srirattanamet.
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

#include "qcom_msim_modem.h"

static int qcom_msim_modem_init(void)
{
	DBG("");

	qcom_msim_radio_settings_init();
	qcom_msim_gprs_init();

	return 0;
}

static void qcom_msim_modem_exit(void)
{
	DBG("");

	qcom_msim_radio_settings_exit();
	qcom_msim_gprs_exit();
}

OFONO_PLUGIN_DEFINE(qcommsimmodem, "Qualcomm multi-sim modem driver", VERSION,
				OFONO_PLUGIN_PRIORITY_DEFAULT,
				qcom_msim_modem_init, qcom_msim_modem_exit)
