/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include "mbpi.h"

static int provision_get_settings(const char *mcc, const char *mnc,
				const char *spn,
				const char *imsi, const char *gid1,
				struct ofono_gprs_provision_data **settings,
				int *count)
{
	GSList *l;
	GSList *apns;
	GError *error = NULL;
	int ap_count;
	int i;

	ofono_info("Provisioning for MCC %s, MNC %s, SPN '%s', IMSI '%s', "
			"GID1 '%s'", mcc, mnc, spn, imsi, gid1);

	/*
	 * TODO: review with upstream.  Default behavior was to
	 * disallow duplicate APN entries, which unfortunately exist
	 * in the mobile-broadband-provider-info db.
	 */
	apns = mbpi_lookup_apn(mcc, mnc, OFONO_GPRS_CONTEXT_TYPE_INTERNET,
				TRUE, &error);
	if (apns == NULL) {
		if (error != NULL) {
			ofono_error("%s", error->message);
			g_error_free(error);
		}

		return -ENOENT;
	}

	ap_count = g_slist_length(apns);

	ofono_info("GPRS Provisioning found %d matching APNs for "
		   "SPN: %s MCC: %s MNC: %s",
		   ap_count, spn, mcc, mnc);
	/*
	 * Only keep the first APN found.
	 *
	 * This allows auto-provisioning to work most of the time vs.
	 * passing FALSE to mbpi_lookup_apn() which would return an
	 * an empty list if duplicates are found.
	 */
	if (ap_count > 1)
		ap_count = 1;

	*settings = g_try_new0(struct ofono_gprs_provision_data, ap_count);
	if (*settings == NULL) {
		ofono_error("Provisioning failed: %s", g_strerror(errno));

		for (l = apns; l; l = l->next)
			mbpi_ap_free(l->data);

		g_slist_free(apns);

		return -ENOMEM;
	}

	*count = ap_count;

	for (l = apns, i = 0; l; l = l->next, i++) {
		struct ofono_gprs_provision_data *ap = l->data;

		/*
		 * Only create a data context for the first matching APN.
		 * See comment above that restricts restricts apn_count.
		 */
		if (i == 0) {
			ofono_info("Name: '%s'", ap->name);
			ofono_info("APN: '%s'", ap->apn);
			ofono_info("Type: %s", mbpi_ap_type(ap->type));
			ofono_info("Username: '%s'", ap->username);
			ofono_info("Password: '%s'", ap->password);

			memcpy(*settings + i, ap,
				sizeof(struct ofono_gprs_provision_data));
		}

		g_free(ap);
	}

	g_slist_free(apns);

	return 0;
}

static struct ofono_gprs_provision_driver provision_driver = {
	.name		= "Provisioning",
	.get_settings	= provision_get_settings
};

static int provision_init(void)
{
	return ofono_gprs_provision_driver_register(&provision_driver);
}

static void provision_exit(void)
{
	ofono_gprs_provision_driver_unregister(&provision_driver);
}

OFONO_PLUGIN_DEFINE(provision, "Provisioning Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			provision_init, provision_exit)
