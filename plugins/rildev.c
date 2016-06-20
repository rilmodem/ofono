/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2014 Canonical Ltd.
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

#include <errno.h>
#include <ctype.h>
#include <stdlib.h>

#include <glib.h>
#include <string.h>
#include <stdio.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

static GSList *modem_list;

static int create_rilmodem(const char *ril_type,
					int slot, ofono_bool_t is_standby)
{
	struct ofono_modem *modem;
	char dev_name[64];
	char *socket;
	int retval;

	snprintf(dev_name, sizeof(dev_name), "ril_%d", slot);

	/* Currently there is only one ril implementation, create always */
	modem = ofono_modem_create(dev_name, ril_type);
	if (modem == NULL) {
		DBG("ofono_modem_create failed for type %s", ril_type);
		return -ENODEV;
	}

	modem_list = g_slist_prepend(modem_list, modem);

	ofono_modem_set_integer(modem, "Slot", slot);
	ofono_modem_set_boolean(modem, "IsStandby", is_standby);

	/* AOSP has socket path "rild", "rild2"..., while others may differ */
	if (slot != 0)
		socket = g_strdup_printf("/dev/socket/rild%d", slot + 1);
	else
		socket = g_strdup("/dev/socket/rild");

	ofono_modem_set_string(modem, "Socket", socket);
	g_free(socket);

	/* This causes driver->probe() to be called... */
	retval = ofono_modem_register(modem);
	if (retval != 0) {
		ofono_error("%s: ofono_modem_register returned: %d",
				__func__, retval);
		return retval;
	}

	/*
	 * kickstart the modem:
	 * causes core modem code to call
	 * - set_powered(TRUE) - which in turn
	 *   calls driver->enable()
	 *
	 * - driver->pre_sim()
	 *
	 * Could also be done via:
	 *
	 * - a DBus call to SetProperties w/"Powered=TRUE" *1
	 * - sim_state_watch ( handles SIM removal? LOCKED states? **2
	 * - ofono_modem_set_powered()
	 */
	ofono_modem_reset(modem);

	return 0;
}

static int detect_init(void)
{
	const char *ril_type;
	const char *multi_sim;
	int num_slots = 1;
	int i;

	ril_type = getenv("OFONO_RIL_DEVICE");
	if (ril_type == NULL)
		return 0;

	/* Check for multi-SIM support */
	multi_sim = getenv("OFONO_RIL_NUM_SIM_SLOTS");
	if (multi_sim != NULL && *multi_sim != '\0') {
		int env_slots;
		char *endp;

		env_slots = (int) strtoul(multi_sim, &endp, 10);
		if (*endp == '\0')
			num_slots = env_slots;
	}

	ofono_info("RILDEV detected modem type %s, %d SIM slot(s)",
			ril_type, num_slots);

	for (i = 0; i < num_slots; ++i)
		create_rilmodem(ril_type, i, num_slots > 1);

	return 0;
}

static void detect_exit(void)
{
	GSList *list;

	for (list = modem_list; list; list = list->next) {
		struct ofono_modem *modem = list->data;

		ofono_modem_remove(modem);
	}

	g_slist_free(modem_list);
	modem_list = NULL;
}

OFONO_PLUGIN_DEFINE(rildev, "ril type detection", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, detect_init, detect_exit)
