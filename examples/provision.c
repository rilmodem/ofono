/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
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

#include <string.h>
#include <glib.h>

#include <errno.h>

#define OFONO_API_SUBJECT_TO_CHANGE

#include <ofono/modem.h>
#include <ofono/gprs-provision.h>
#include <ofono/types.h>
#include <ofono/plugin.h>
#include <ofono/log.h>

static int example_provision_get_settings(const char *mcc, const char *mnc,
				const char *spn,
				struct ofono_gprs_provision_data **settings,
				int *count)
{
	ofono_debug("Provisioning...");
	*count = 0;
	*settings = NULL;

	ofono_debug("Finding settings for MCC %s, MNC %s, SPN '%s'",
			mcc, mnc, spn);

	if (strcmp(mcc, "246") != 0 || strcmp(mnc, "81") != 0 ||
						g_strcmp0(spn, "oFono") != 0)
		return -ENOENT;

	ofono_debug("Creating example settings for phonesim");

	*settings = g_try_new0(struct ofono_gprs_provision_data, 2);
	if (*settings == NULL)
		return -ENOMEM;

	*count = 2;

	/* Internet context settings */
	(*settings)[0].proto = OFONO_GPRS_PROTO_IP;
	(*settings)[0].type = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	(*settings)[0].name = g_strdup("Phonesim Internet");
	(*settings)[0].apn = g_strdup("internetapn");

	/* MMS context settings */
	(*settings)[1].proto = OFONO_GPRS_PROTO_IP;
	(*settings)[1].type = OFONO_GPRS_CONTEXT_TYPE_MMS;
	(*settings)[1].name = g_strdup("Phonesim MMS");
	(*settings)[1].apn = g_strdup("mmsapn");
	(*settings)[1].username = g_strdup("mmsuser");
	(*settings)[1].password = g_strdup("mmspass");
	(*settings)[1].message_proxy = g_strdup("10.11.12.13:8080");
	(*settings)[1].message_center = g_strdup("http://mms.example.com:8000");

	return 0;
}

static struct ofono_gprs_provision_driver example_driver = {
	.name		= "Example GPRS context provisioning",
	.priority       = OFONO_PLUGIN_PRIORITY_LOW,
	.get_settings	= example_provision_get_settings,
};

static int example_provision_init(void)
{
	return ofono_gprs_provision_driver_register(&example_driver);
}

static void example_provision_exit(void)
{
	ofono_gprs_provision_driver_unregister(&example_driver);
}

OFONO_PLUGIN_DEFINE(example_provision, "Example Provisioning Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			example_provision_init,
			example_provision_exit)
