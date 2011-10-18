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

#include <stdio.h>
#include <stdlib.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>

#include "plugins/mbpi.h"

static void lookup_apn(const char *match_mcc, const char *match_mnc)
{
	GSList *l;
	GSList *apns;
	GError *error = NULL;

	printf("Searching for info for network: %s%s\n", match_mcc, match_mnc);

	apns = mbpi_lookup(match_mcc, match_mnc, TRUE, &error);

	if (apns == NULL) {
		if (error != NULL) {
			g_printerr("Lookup failed: %s\n", error->message);
			g_error_free(error);
		}

		return;
	}

	for (l = apns; l; l = l->next) {
		struct ofono_gprs_provision_data *apn = l->data;

		printf("\n");
		printf("Name: %s\n", apn->name);
		printf("APN: %s\n", apn->apn);
		printf("Type: %s\n", mbpi_ap_type(apn->type));
		printf("Username: %s\n", apn->username);
		printf("Password: %s\n", apn->password);

		mbpi_provision_data_free(apn);
	}

	g_slist_free(apns);
}

static gboolean option_version = FALSE;

static GOptionEntry options[] = {
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &option_version,
				"Show version information and exit" },
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *error = NULL;

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &error) == FALSE) {
		if (error != NULL) {
			g_printerr("%s\n", error->message);
			g_error_free(error);
		} else
			g_printerr("An unknown error occurred\n");
		exit(1);
	}

	g_option_context_free(context);

	if (option_version == TRUE) {
		printf("%s\n", VERSION);
		exit(0);
	}

	if (argc < 2) {
		fprintf(stderr, "Missing parameters\n");
		exit(1);
	}

	lookup_apn(argv[1], argv[2]);

	return 0;
}
