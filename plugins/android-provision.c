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

static unsigned int filter_apns(GSList **apns, GSList *mbpi_apns,
				gboolean mvno_found)
{
	GSList *l = NULL;
	GSList *l2 = NULL;
	gboolean found = FALSE;
	unsigned int ap_count = g_slist_length(*apns);
	struct apndb_provision_data *ap;

	if (mvno_found == TRUE) {

		for (l = *apns; l;) {
     			ap = l->data;
			l = l->next;

			if (ap->mvno == FALSE) {
				DBG("Removing: %s", ap->gprs_data.apn);
				*apns = g_slist_remove(*apns,
							(gconstpointer) ap);
				android_apndb_ap_free(ap);
				ap_count--;
			}
		}

		goto done;
	}

	for (l = mbpi_apns; l; l = l->next, found = FALSE) {
		struct ofono_gprs_provision_data *ap2 = l->data;

		if (ap2->apn == NULL) {
			ofono_error("%s: invalid mbpi entry - %s found",
					__func__, ap2->name);
			continue;
		}

		for (l2 = *apns; l2; l2 = l2->next) {
     			ap = l2->data;

			if (ap->gprs_data.apn != NULL &&
				ap->gprs_data.type ==
				OFONO_GPRS_CONTEXT_TYPE_INTERNET &&
				g_strcmp0(ap2->apn, ap->gprs_data.apn) == 0) {

				found = TRUE;
				break;
			}
		}

		if (found == FALSE) {
			DBG("Adding %s to apns", ap2->apn);

			ap = g_try_new0(struct apndb_provision_data, 1);
			if (ap == NULL) {
				ofono_error("%s: out-of-memory trying to"
						" provision APN - %s",
						__func__, ap2->name);
				goto done;
			}

			memcpy(&ap->gprs_data, ap2, sizeof(ap->gprs_data));
			*apns = g_slist_append(*apns, ap);
			ap_count++;
			g_free(ap2);
		} else {
			mbpi_ap_free(ap2);
		}
	}

done:
	return ap_count;
}

static int provision_get_settings(const char *mcc, const char *mnc,
				const char *spn,
				const char *imsi, const char *gid1,
				struct ofono_gprs_provision_data **settings,
				int *count)
{
	GSList *apns = NULL;
	GSList *mbpi_apns = NULL;
	GSList *l = NULL;
	GError *error = NULL;
	gboolean mvno_found = FALSE;
	unsigned int ap_count;
	unsigned int i;

	ofono_info("Provisioning for MCC %s, MNC %s, SPN '%s', IMSI '%s', "
			"GID1 '%s'", mcc, mnc, spn, imsi, gid1);

	apns = android_apndb_lookup_apn(mcc, mnc, spn, imsi, gid1,
					&mvno_found, &error);
	if (apns == NULL) {
		if (error != NULL) {
			ofono_error("%s: apndb_lookup error -%s for mcc %s"
					" mnc %s spn %s imsi %s", __func__,
					error->message, mcc, mnc, spn, imsi);
			g_error_free(error);
		}
	}

	/* If an mvno apn was found, only provision mvno apns */
	if (mvno_found == FALSE) {
		mbpi_apns = mbpi_lookup_apn(mcc, mnc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET,
					TRUE, &error);
		if (mbpi_apns == NULL) {
			if (error != NULL) {
				ofono_error("%s: MBPI error - %s for mcc %s"
						" mnc %s spn: %s", __func__,
						error->message, mcc, mnc, spn);
				g_error_free(error);
			}
		}
	}

	ap_count = filter_apns(&apns, mbpi_apns, mvno_found);
	if (ap_count == 0) {
		ofono_warn("%s: No APNs found for mcc %s mnc %s spn: %s"
				" imsi: %s", __func__, mcc, mnc, spn, imsi);

		*count = 0;
		return -ENOENT;
	}

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
		struct apndb_provision_data *ap = l->data;

		DBG("Name: '%s'", ap->gprs_data.name);
		DBG("APN: '%s'", ap->gprs_data.apn);
		DBG("Type: %s", mbpi_ap_type(ap->gprs_data.type));
		DBG("Username: '%s'", ap->gprs_data.username);
		DBG("Password: '%s'", ap->gprs_data.password);
		DBG("Message Proxy: '%s'", ap->gprs_data.message_proxy);
		DBG("Message Center: '%s'", ap->gprs_data.message_center);
		DBG("MVNO: %u", ap->mvno);

		memcpy(*settings + i, &ap->gprs_data, sizeof(ap->gprs_data));

		g_free(ap);
	}

	if (apns != NULL)
		g_slist_free(apns);

	if (mbpi_apns != NULL)
		g_slist_free(mbpi_apns);

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
