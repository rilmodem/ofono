/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011  Nokia Corporation and/or its subsidiary(-ies).
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

#include "cdmamodem.h"

static int cdmamodem_init(void)
{
	cdma_voicecall_init();
	cdma_devinfo_init();
	cdma_connman_init();

	return 0;
}

static void cdmamodem_exit(void)
{
	cdma_voicecall_exit();
	cdma_devinfo_exit();
	cdma_connman_exit();
}

OFONO_PLUGIN_DEFINE(cdmamodem, "CDMA AT modem driver", VERSION,
	OFONO_PLUGIN_PRIORITY_DEFAULT, cdmamodem_init, cdmamodem_exit)
