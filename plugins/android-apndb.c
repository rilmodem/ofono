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
#include <ofono/log.h>

#include "android-apndb.h"

/* TODO: consider reading path from an environment variable */

#ifndef ANDROID_APN_DATABASE
#define ANDROID_APN_DATABASE    "/android/system/etc/apns-conf.xml"
#endif

struct apndb_data {
	const char *match_mcc;
	const char *match_mnc;
	GSList *apns;
	gboolean match_found;
	gboolean allow_duplicates;
};

void android_apndb_ap_free(gpointer data)
{
	struct ofono_gprs_provision_data *ap = data;

	g_free(ap->name);
	g_free(ap->apn);
	g_free(ap->username);
	g_free(ap->password);
	g_free(ap->message_proxy);
	g_free(ap->message_center);

	g_free(ap);
}

static void android_apndb_g_set_error(GMarkupParseContext *context,
					GError **error,
					GQuark domain,
					gint code,
					const gchar *fmt, ...)
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
	/*
	 * The database contains entries with the following type field contents:
	 * - default
	 * - default,mms
	 * - default,supl
	 * - defualt,supl,dun
	 * - default,supl,mms
	 * - mms
	 */

	if (g_str_equal(types, "mms"))
		return OFONO_GPRS_CONTEXT_TYPE_MMS;
	else if (g_str_has_prefix(types, "default"))
		return OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	else
		return OFONO_GPRS_CONTEXT_TYPE_ANY;
}

static char *android_apndb_sanitize_ipv4_address(const char *address)
{
	char **numbers = NULL;
	char *sanitized_numbers[4];
	unsigned int count = 0;
	char *result = NULL;
	char *numeral;

	/*
	 * As src/gprs.c expects MMS proxies to always be
	 * specified using IPV4 numbers-and-dot notation,
	 * we need to strip any leading "0"s from the
	 * individual numeric components, otherwise they
	 * will be treated as octal numbers
	 * ( see 'man inet_aton' for details ).
	 */

	if (g_ascii_isdigit(*address) == FALSE)
		goto done;

	numbers = g_strsplit(address, ".", 4);

	for (; (numeral = *(numbers+count)); count++) {
		if (count > 3)
			goto done;

		for (; *numeral; numeral++) {
			if (g_ascii_isdigit(*numeral) == FALSE)
				goto done;
			else if (*numeral == '0')
				continue;
			else
				break;
		}

		if (*numeral)
			sanitized_numbers[count] = numeral;
		else
			sanitized_numbers[count] = "0";
	}

	if (count != 4)
		goto done;

	result = g_strdup_printf("%s.%s.%s.%s",
					sanitized_numbers[0],
					sanitized_numbers[1],
					sanitized_numbers[2],
					sanitized_numbers[3]);

done:
	if (numbers != NULL)
		g_strfreev(numbers);

	return result;
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
	const gchar *protocol = NULL;
	const gchar *mmsproxy = NULL;
	const gchar *mmsport = NULL;
	const gchar *mmscenter = NULL;
	enum ofono_gprs_proto proto = OFONO_GPRS_PROTO_IP;
	enum ofono_gprs_context_type type;

	if (!g_str_equal(element_name, "apn"))
		return;

	for (i = 0; attribute_names[i]; i++) {
		if (g_str_equal(attribute_names[i], "carrier"))
			carrier = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mcc"))
			mcc = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mnc"))
			mnc = attribute_values[i];
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

	if (g_str_equal(mcc, apndb->match_mcc) == FALSE ||
		g_str_equal(mnc, apndb->match_mnc) == FALSE)
		return;

	for (i = 0; attribute_names[i]; i++) {
		if (g_str_equal(attribute_names[i], "apn"))
			apn = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "user"))
			username = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "password"))
			password = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "type"))
			types = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "protocol"))
			protocol = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mmsc"))
			mmscenter = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mmsproxy"))
			mmsproxy = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "mmsport"))
			mmsport = attribute_values[i];
	}

	if (apn == NULL) {
		android_apndb_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
						"APN attribute missing");
		return;
	}

	if (types == NULL) {
		ofono_error("%s: apn for %s missing type attribute", __func__,
				carrier);
		return;
	}

	if (protocol != NULL) {
		if (g_str_equal(attribute_names[i], "IP")) {
			proto = OFONO_GPRS_PROTO_IP;
		} else if (g_str_equal(attribute_names[i], "IPV6")) {
			proto = OFONO_GPRS_PROTO_IPV6;
		} else if (g_str_equal(attribute_names[i], "IPV4V6")) {
			proto = OFONO_GPRS_PROTO_IPV4V6;
		} else {
			ofono_error("%s: APN %s has invalid protocol=%s"
					"attribute", __func__, carrier,
					protocol);
			return;
		}
	}

	type = determine_apn_type(types);

	if (type == OFONO_GPRS_CONTEXT_TYPE_ANY ||
		(type == OFONO_GPRS_CONTEXT_TYPE_MMS && mmsproxy == NULL)) {
		DBG("Skipping %s context; types: %s", apn, types);
		return;
	}

	ap = g_try_new0(struct ofono_gprs_provision_data, 1);
	if (ap == NULL) {
		ofono_error("%s: out-of-memory trying to provision APN - %s",
				__func__, carrier);
		return;
	}

	ap->type = type;

	if (carrier != NULL)
		ap->name = g_strdup(carrier);

	if (apn != NULL)
		ap->apn = g_strdup(apn);

	if (username != NULL)
		ap->username = g_strdup(username);

	if (password != NULL)
		ap->password = g_strdup(password);

	if (mmscenter != NULL)
		ap->message_center = g_strdup(mmscenter);

	if (mmsproxy != NULL) {
		char *tmp = android_apndb_sanitize_ipv4_address(mmsproxy);
		if (tmp != NULL)
			mmsproxy = tmp;

		if (mmsport != NULL)
			ap->message_proxy = g_strdup_printf("%s:%s", mmsproxy,
								mmsport);
		else
			ap->message_proxy = g_strdup(mmsproxy);

		g_free(tmp);
	}

	ap->proto = proto;

	apndb->apns = g_slist_append(apndb->apns, ap);
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

static gboolean android_apndb_parse(const GMarkupParser *parser,
					gpointer userdata,
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
	struct apndb_data apndb = { NULL };

	apndb.match_mcc = mcc;
	apndb.match_mnc = mnc;
	apndb.allow_duplicates = allow_duplicates;

	if (android_apndb_parse(&toplevel_apndb_parser, &apndb,
				error) == FALSE) {
		g_slist_free_full(apndb.apns, android_apndb_ap_free);
		apndb.apns = NULL;
	}

	return apndb.apns;
}
