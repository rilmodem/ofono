/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *                2013 Simon Busch <morphis@gravedo.de>
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
#include <string.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>

#include "android-apndb.h"
#include "mbpi.h"

static int provision_get_settings(const char *mcc, const char *mnc,
				const char *spn,
				struct ofono_gprs_provision_data **settings,
				int *count)
{
	GSList *l;
	GSList *apns;
	GError *error = NULL;
	int ap_count;
	int i;

	DBG("Provisioning for MCC %s, MNC %s, SPN '%s'", mcc, mnc, spn);

	apns = android_apndb_lookup_apn(mcc, mnc, FALSE, &error);
	if (apns == NULL) {
		if (error != NULL) {
			ofono_error("%s: %s", __func__, error->message);
			g_error_free(error);
		}

		*count = 0;
		return -ENOENT;
	}

	ap_count = g_slist_length(apns);

	DBG("Found %d APs", ap_count);

	*settings = g_try_new0(struct ofono_gprs_provision_data, ap_count);
	if (*settings == NULL) {
		ofono_error("%s: provisioning failed: %s", __func__,
				g_strerror(errno));

		g_slist_free_full(apns, android_apndb_ap_free);

		*count = 0;
		return -ENOMEM;
	}

	*count = ap_count;

	for (l = apns, i = 0; l; l = l->next, i++) {
		struct ofono_gprs_provision_data *ap = l->data;

		DBG("Name: '%s'", ap->name);
		DBG("APN: '%s'", ap->apn);
		DBG("Type: %s", mbpi_ap_type(ap->type));
		DBG("Username: '%s'", ap->username);
		DBG("Password: '%s'", ap->password);
		DBG("Message Proxy: '%s'", ap->message_proxy);
		DBG("Message Center: '%s'", ap->message_center);

		memcpy(*settings + i, ap, sizeof(*ap));

		g_free(ap);
	}

	g_slist_free(apns);

	return 0;
}

static struct ofono_gprs_provision_driver android_provision_driver = {
	.name		= "Android APN database Provisioning",
	.get_settings	= provision_get_settings
};

static int android_provision_init(void)
{
	return ofono_gprs_provision_driver_register(&android_provision_driver);
}

static void android_provision_exit(void)
{
	ofono_gprs_provision_driver_unregister(&android_provision_driver);
}

OFONO_PLUGIN_DEFINE(android_provision,
			"Android APN database Provisioning Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			android_provision_init, android_provision_exit)
