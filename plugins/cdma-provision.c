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
#include <ofono/cdma-provision.h>

#include "mbpi.h"

static int cdma_provision_get_provider_name(const char *sid, char **name)
{
	GError *error = NULL;

	DBG("Search provider name for SID %s", sid);

	*name = mbpi_lookup_cdma_provider_name(sid, &error);
	if (*name == NULL) {
		if (error != NULL) {
			ofono_error("%s", error->message);
			g_error_free(error);
		}

		return -ENOENT;
	}

	DBG("Found provider name: %s", *name);

	return 0;
}

static struct ofono_cdma_provision_driver provision_driver = {
	.name = "CDMA provisioning",
	.get_provider_name = cdma_provision_get_provider_name
};

static int cdma_provision_init(void)
{
	return ofono_cdma_provision_driver_register(&provision_driver);
}

static void cdma_provision_exit(void)
{
	ofono_cdma_provision_driver_unregister(&provision_driver);
}

OFONO_PLUGIN_DEFINE(cdma_provision, "CDMA provisioning Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			cdma_provision_init, cdma_provision_exit)
