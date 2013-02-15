/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *                2013 Simon Busch <morphis@gravedo.de>
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
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>

#include "android-apndb.h"

#ifndef ANDROID_APN_DATABASE
#define ANDROID_APN_DATABASE		"/usr/share/android-apn-database/apns.xml"
#endif

struct apndb_data {
	const char *match_mcc;
	const char *match_mnc;
	GSList *apns;
	gboolean match_found;
	gboolean allow_duplicates;
};

void android_apndb_ap_free(struct ofono_gprs_provision_data *ap)
{
	g_free(ap->name);
	g_free(ap->apn);
	g_free(ap->username);
	g_free(ap->password);
	g_free(ap->message_proxy);
	g_free(ap->message_center);

	g_free(ap);
}

static void android_apndb_g_set_error(GMarkupParseContext *context, GError **error,
				GQuark domain, gint code, const gchar *fmt, ...)
{
	va_list ap;
	gint line_number, char_number;

	g_markup_parse_context_get_position(context, &line_number,
						&char_number);
	va_start(ap, fmt);

	*error = g_error_new_valist(domain, code, fmt, ap);

	va_end(ap);

	g_prefix_error(error, "%s:%d ", ANDROID_APN_DATABASE, line_number);
}

static enum ofono_gprs_context_type determine_apn_type(const char *types)
{
	/* The database contains entries with the following type field contents:
	 * - default
	 * - default,mms
	 * - default,supl
	 * - defualt,supl,dun
	 * - default,supl,mms
	 * - mms
	 */

	if (g_str_equal(types, "mms"))
		return OFONO_GPRS_CONTEXT_TYPE_MMS;
	else if (g_str_equal(types, "default") ||
			 g_str_equal(types, "default,supl") ||
			 g_str_equal(types, "default,supl,dun"))
		return OFONO_GPRS_CONTEXT_TYPE_INTERNET;

	return OFONO_GPRS_CONTEXT_TYPE_ANY;
}

static void toplevel_apndb_start(GMarkupParseContext *context,
					const gchar *element_name,
					const gchar **attribute_names,
					const gchar **attribute_values,
					gpointer userdata, GError **error)
{
	struct apndb_data *apndb = userdata;
	struct ofono_gprs_provision_data *ap = NULL;
	int i;
	const gchar *carrier = NULL;
	const gchar *mcc = NULL;
	const gchar *mnc = NULL;
	const gchar *apn = NULL;
	const gchar *username = NULL;
	const gchar *password = NULL;
	const gchar *types = NULL;
	const gchar *mmsproxy = NULL;
	const gchar *mmsport = NULL;
	const gchar *mmscenter = NULL;

	if (!g_str_equal(element_name, "apn"))
		return;

	for (i = 0; attribute_names[i]; i++) {
		if (g_str_equal(attribute_names[i], "carrier"))
			carrier = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mcc"))
			mcc = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mnc"))
			mnc = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "apn"))
			apn = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "username"))
			username = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "password"))
			password = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "type"))
			types = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mmsc"))
			mmscenter = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mmsproxy"))
			mmsproxy = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mmsport"))
			mmsport = attribute_values[i];
	}

	if (mcc == NULL) {
		android_apndb_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"Missing attribute: mcc");
		return;
	}

	if (mnc == NULL) {
		android_apndb_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"Missing attribute: mnc");
		return;
	}


	if (g_str_equal(mcc, apndb->match_mcc) &&
		g_str_equal(mnc, apndb->match_mnc)) {
		if (apn == NULL) {
			android_apndb_g_set_error(context, error, G_MARKUP_ERROR,
						G_MARKUP_ERROR_MISSING_ATTRIBUTE,
						"APN attribute missing");
			return;
		}

		ap = g_new0(struct ofono_gprs_provision_data, 1);

		if (carrier)
			ap->name = g_strdup(carrier);

		if (apn)
			ap->apn = g_strdup(apn);

		if (username)
			ap->username = g_strdup(username);

		if (password)
			ap->password = g_strdup(password);

		if (mmscenter)
			ap->message_center = g_strdup(mmscenter);

		if (mmsproxy) {
			if (mmsport)
				ap->message_proxy = g_strdup_printf("%s:%s", mmsproxy, mmsport);
			else
				ap->message_proxy = g_strdup(mmsproxy);
		}

		ap->type = determine_apn_type(types);

		/* there is no differentiation between IPv4 and IPv6 in the database so take it as
		 * IPv4 only */
		ap->proto = OFONO_GPRS_PROTO_IP;

		apndb->apns = g_slist_append(apndb->apns, ap);
	}
}

static void toplevel_apndb_end(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer userdata, GError **error)
{
}

static const GMarkupParser toplevel_apndb_parser = {
	toplevel_apndb_start,
	toplevel_apndb_end,
	NULL,
	NULL,
	NULL,
};

static gboolean android_apndb_parse(const GMarkupParser *parser, gpointer userdata,
				GError **error)
{
	struct stat st;
	char *db;
	int fd;
	GMarkupParseContext *context;
	gboolean ret;

	fd = open(ANDROID_APN_DATABASE, O_RDONLY);
	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"open(%s) failed: %s", ANDROID_APN_DATABASE,
				g_strerror(errno));
		return FALSE;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"fstat(%s) failed: %s", ANDROID_APN_DATABASE,
				g_strerror(errno));
		return FALSE;
	}

	db = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (db == MAP_FAILED) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"mmap(%s) failed: %s", ANDROID_APN_DATABASE,
				g_strerror(errno));
		return FALSE;
	}

	context = g_markup_parse_context_new(parser,
						G_MARKUP_TREAT_CDATA_AS_TEXT,
						userdata, NULL);

	ret = g_markup_parse_context_parse(context, db, st.st_size, error);

	if (ret == TRUE)
		g_markup_parse_context_end_parse(context, error);

	munmap(db, st.st_size);
	close(fd);
	g_markup_parse_context_free(context);

	return ret;
}

GSList *android_apndb_lookup_apn(const char *mcc, const char *mnc,
			gboolean allow_duplicates, GError **error)
{
	struct apndb_data apndb;
	GSList *l;

	memset(&apndb, 0, sizeof(apndb));
	apndb.match_mcc = mcc;
	apndb.match_mnc = mnc;
	apndb.allow_duplicates = allow_duplicates;

	if (android_apndb_parse(&toplevel_apndb_parser, &apndb, error) == FALSE) {
		for (l = apndb.apns; l; l = l->next)
			android_apndb_ap_free(l->data);

		g_slist_free(apndb.apns);
		apndb.apns = NULL;
	}

	return apndb.apns;
}
