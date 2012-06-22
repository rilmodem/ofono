/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>

#include "qmimodem.h"

static int qmimodem_init(void)
{
	qmi_devinfo_init();
	qmi_sim_legacy_init();

	return 0;
}

static void qmimodem_exit(void)
{
	qmi_sim_legacy_exit();
	qmi_devinfo_exit();
}

OFONO_PLUGIN_DEFINE(qmimodem, "Qualcomm QMI modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, qmimodem_init, qmimodem_exit)
