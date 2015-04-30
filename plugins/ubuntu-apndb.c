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
#include <stdlib.h>
#include <unistd.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>
#include <ofono/log.h>

#include "ubuntu-apndb.h"

#ifndef SYSTEM_APNDB_PATH
#define SYSTEM_APNDB_PATH     "/system/etc/apns-conf.xml"
#define CUSTOM_APNDB_PATH     "/custom/etc/apns-conf.xml"
#endif

struct apndb_data {
	const char *match_mcc;
	const char *match_mnc;
	const char *match_imsi;
	const char *match_spn;
	const char *match_gid1;
	GSList *apns;
	gboolean allow_duplicates;
	gboolean mvno_found;
};

void ubuntu_apndb_ap_free(gpointer data)
{
	struct apndb_provision_data *ap = data;

	g_free(ap->gprs_data.name);
	g_free(ap->gprs_data.apn);
	g_free(ap->gprs_data.username);
	g_free(ap->gprs_data.password);
	g_free(ap->gprs_data.message_proxy);
	g_free(ap->gprs_data.message_center);

	g_free(ap);
}

static gboolean imsi_match(const char *imsi, const char *match)
{
	gboolean result = FALSE;
	size_t imsi_len = strlen(imsi);
	size_t match_len = strlen(match);
	unsigned int i;

	DBG("imsi %s match %s", imsi, match);

	if (match_len == 0 || imsi_len < match_len)
		goto done;

	for (i = 0; i < match_len; i++) {
		if (*(imsi + i) == *(match + i))
			continue;
		else if (*(match + i) == 'x')
			continue;
		else if (*(match + i) == 'X')
			continue;
		else
			goto done;
	}

	result = TRUE;

done:
	return result;
}

static void strip_non_mvno_apns(GSList **apns)
{
	GSList *l = NULL;
	unsigned int ap_count = g_slist_length(*apns);
	struct apndb_provision_data *ap;

	DBG("");

	for (l = *apns; l;) {
		ap = l->data;
		l = l->next;

		if (ap->mvno == FALSE) {
			DBG("Removing: %s", ap->gprs_data.apn);
			*apns = g_slist_remove(*apns,
						(gconstpointer) ap);
			ubuntu_apndb_ap_free(ap);
			ap_count--;
		}
	}
}

static GSList *merge_apn_lists(GSList *custom_apns, GSList *base_apns)
{
	GSList *l = NULL;
	GSList *l2 = NULL;
	gboolean found = FALSE;
	struct apndb_provision_data *ap;

	DBG("");

	if (custom_apns == NULL)
		return base_apns;

	for (l = custom_apns; l; l = l->next, found = FALSE) {
		struct apndb_provision_data *ap2 = l->data;

		if (ap2->gprs_data.apn == NULL) {
			ofono_error("%s: invalid custom apn entry - %s found",
					__func__, ap2->gprs_data.name);
			continue;
		}

		for (l2 = base_apns; l2; l2 = l2->next) {
			ap = l2->data;

			if (ap->gprs_data.apn != NULL &&
				ap->gprs_data.type == ap2->gprs_data.type &&
				g_strcmp0(ap2->gprs_data.apn,
						ap->gprs_data.apn) == 0) {

				found = TRUE;
				break;
			}
		}

		if (found == TRUE) {
			DBG("found=TRUE; removing '%s'", ap->gprs_data.apn);

			base_apns = g_slist_remove(base_apns, ap);
			ubuntu_apndb_ap_free(ap);
		}
	}

	custom_apns = g_slist_reverse(custom_apns);

	for (l = custom_apns; l; l = l->next) {
		struct ap2 *ap2 = l->data;

		base_apns = g_slist_prepend(base_apns, ap2);
	}

	g_slist_free(custom_apns);

	return base_apns;
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

	/* Default apns can be used for mms and ia, mms can be used for ia */
	if (types == NULL || g_strcmp0(types, "*") == 0
					|| strstr(types, "default") != NULL)
		return OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	else if (strstr(types, "mms") != NULL)
		return OFONO_GPRS_CONTEXT_TYPE_MMS;
	else if (strstr(types, "ia") != NULL)
		return OFONO_GPRS_CONTEXT_TYPE_IA;
	else
		return OFONO_GPRS_CONTEXT_TYPE_ANY;
}

static char *ubuntu_apndb_sanitize_ipv4_address(const char *address)
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
	struct apndb_provision_data *ap = NULL;
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
	const gchar *mvnomatch = NULL;
	const gchar *mvnotype = NULL;
	enum ofono_gprs_proto proto = OFONO_GPRS_PROTO_IP;
	enum ofono_gprs_context_type type;

	if (g_strcmp0(element_name, "apn") != 0)
		return;

	for (i = 0; attribute_names[i]; i++) {
		if (g_strcmp0(attribute_names[i], "carrier") == 0)
			carrier = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "mcc") == 0)
			mcc = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "mnc") == 0)
			mnc = attribute_values[i];
	}

	if (mcc == NULL) {
		ofono_error("%s: apn for %s missing 'mcc' attribute", __func__,
				carrier);
		return;
	}

	if (mnc == NULL) {
		ofono_error("%s: apn for %s missing 'mnc' attribute", __func__,
				carrier);
		return;
	}

	if (g_strcmp0(mcc, apndb->match_mcc) != 0 ||
		g_strcmp0(mnc, apndb->match_mnc) != 0)
		return;

	for (i = 0; attribute_names[i]; i++) {
		if (g_strcmp0(attribute_names[i], "apn") == 0)
			apn = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "user") == 0)
			username = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "password") == 0)
			password = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "type") == 0)
			types = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "protocol") == 0)
			protocol = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "mmsc") == 0)
			mmscenter = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "mmsproxy") == 0)
			mmsproxy = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "mmsport") == 0)
			mmsport = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "mvno_match_data") == 0)
			mvnomatch = attribute_values[i];
		else if (g_strcmp0(attribute_names[i], "mvno_type") == 0)
			mvnotype = attribute_values[i];
	}

	if (apn == NULL) {
		ofono_error("%s: apn for %s missing 'apn' attribute", __func__,
				carrier);
		return;
	}

	if (protocol != NULL) {
		if (g_strcmp0(protocol, "IP") == 0) {
			proto = OFONO_GPRS_PROTO_IP;
		} else if (g_strcmp0(protocol, "IPV6") == 0) {
			/* TODO: Use OFONO_GPRS_PROTO_IPV6 when supported */
			proto = OFONO_GPRS_PROTO_IP;
		} else if (g_strcmp0(protocol, "IPV4V6") == 0) {
			/* TODO: Use OFONO_GPRS_PROTO_IPV4V6 when supported */
			proto = OFONO_GPRS_PROTO_IP;
		} else {
			ofono_error("%s: APN %s has invalid protocol=%s"
					"attribute", __func__, carrier,
					protocol);
			return;
		}
	}

	if (mvnotype != NULL && mvnomatch != NULL) {

		if (g_strcmp0(mvnotype, "imsi") == 0) {
			DBG("APN %s is mvno_type 'imsi'", carrier);

			if (apndb->match_imsi == NULL ||
					imsi_match(apndb->match_imsi,
							mvnomatch) == FALSE) {
				DBG("Skipping MVNO 'imsi' APN %s with"
					" match_data: %s", carrier, mvnomatch);
				return;
			}
		} else if (g_strcmp0(mvnotype, "spn") == 0) {
			DBG("APN %s is mvno_type 'spn'", carrier);

			if (g_strcmp0(mvnomatch, apndb->match_spn) != 0) {
				DBG("Skipping mvno 'spn' APN %s with"
					" match_data: %s", carrier, mvnomatch);
				return;
			}

		} else if (g_strcmp0(mvnotype, "gid") == 0) {
			int match_len = strlen(mvnomatch);

			DBG("APN %s is mvno_type 'gid'", carrier);

			/* Check initial part of GID1 against match data */
			if (apndb->match_gid1 == NULL ||
					g_ascii_strncasecmp(mvnomatch,
							apndb->match_gid1,
							match_len) != 0) {
				DBG("Skipping mvno 'gid' APN %s with"
					" match_data: %s", carrier, mvnomatch);
				return;
			}
		}
	}

	type = determine_apn_type(types);

	if (type == OFONO_GPRS_CONTEXT_TYPE_ANY ||
		(type == OFONO_GPRS_CONTEXT_TYPE_MMS && mmscenter == NULL)) {
		DBG("Skipping %s context; types: %s",
					apn, types ? types : "(null)");
		return;
	}

	ap = g_try_new0(struct apndb_provision_data, 1);
	if (ap == NULL) {
		ofono_error("%s: out-of-memory trying to provision APN - %s",
				__func__, carrier);
		return;
	}

	ap->gprs_data.type = type;

	if (carrier != NULL)
		ap->gprs_data.name = g_strdup(carrier);

	if (apn != NULL)
		ap->gprs_data.apn = g_strdup(apn);

	if (username != NULL)
		ap->gprs_data.username = g_strdup(username);

	if (password != NULL)
		ap->gprs_data.password = g_strdup(password);

	if (mmscenter != NULL && strlen(mmscenter) > 0)
		ap->gprs_data.message_center = g_strdup(mmscenter);

	if (mmsproxy != NULL && strlen(mmsproxy) > 0) {
		char *tmp = ubuntu_apndb_sanitize_ipv4_address(mmsproxy);
		if (tmp != NULL)
			mmsproxy = tmp;

		if (mmsport != NULL)
			ap->gprs_data.message_proxy =
				g_strdup_printf("%s:%s", mmsproxy, mmsport);
		else
			ap->gprs_data.message_proxy = g_strdup(mmsproxy);

		g_free(tmp);
	}

	ap->gprs_data.proto = proto;

	if (mvnotype != NULL) {
		ap->mvno = TRUE;
		apndb->mvno_found = TRUE;
	}

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

static gboolean ubuntu_apndb_parse(const GMarkupParser *parser,
					gpointer userdata,
					const char *apndb_path,
					GError **error)
{
	struct stat st;
	char *db;
	int fd;
	GMarkupParseContext *context;
	gboolean ret;

	fd = open(apndb_path, O_RDONLY);
	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"open(%s) failed: %s", apndb_path,
				g_strerror(errno));
		return FALSE;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"fstat(%s) failed: %s", apndb_path,
				g_strerror(errno));
		return FALSE;
	}

	db = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (db == MAP_FAILED) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"mmap(%s) failed: %s", apndb_path,
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

GSList *ubuntu_apndb_lookup_apn(const char *mcc, const char *mnc,
			const char *spn, const char *imsi, const char *gid1,
			GError **error)
{
	struct apndb_data apndb = { NULL };
	struct apndb_data custom_apndb = { NULL };
	const char *apndb_path;
	GSList *merged_apns;

	/*
	 * Lookup /custom apns first, if mvno apns found,
	 * strip non-mvno apns from list
	 *
	 * Lookup /system next, apply same mvno logic...
	 *
	 * Merge both lists, any custom apns that match the type
	 * and apn fields of a /system apn replace it.
	 */

	custom_apndb.match_mcc = mcc;
	custom_apndb.match_mnc = mnc;
	custom_apndb.match_spn = spn;
	custom_apndb.match_imsi = imsi;
	custom_apndb.match_gid1 = gid1;

	if ((apndb_path = getenv("OFONO_CUSTOM_APNDB_PATH")) == NULL)
		apndb_path = CUSTOM_APNDB_PATH;

	if (ubuntu_apndb_parse(&toplevel_apndb_parser, &custom_apndb,
				apndb_path,
				error) == FALSE) {
		g_slist_free_full(custom_apndb.apns, ubuntu_apndb_ap_free);
		custom_apndb.apns = NULL;

		if (*error) {
			if ((*error)->domain != G_FILE_ERROR)
				ofono_error("%s: custom apn_lookup error -%s",
						__func__, (*error)->message);

			g_error_free(*error);
			*error = NULL;
		}
	}

	if (custom_apndb.apns && custom_apndb.mvno_found)
		strip_non_mvno_apns(&custom_apndb.apns);

	DBG("custom_apndb: found '%d' APNs", g_slist_length(custom_apndb.apns));

	apndb.match_mcc = mcc;
	apndb.match_mnc = mnc;
	apndb.match_spn = spn;
	apndb.match_imsi = imsi;
	apndb.match_gid1 = gid1;

	if ((apndb_path = getenv("OFONO_SYSTEM_APNDB_PATH")) == NULL)
		apndb_path = SYSTEM_APNDB_PATH;

	if (ubuntu_apndb_parse(&toplevel_apndb_parser, &apndb,
				apndb_path,
				error) == FALSE) {
		g_slist_free_full(apndb.apns, ubuntu_apndb_ap_free);
		apndb.apns = NULL;
	}

	DBG("apndb: found '%d' APNs", g_slist_length(apndb.apns));

	if (apndb.apns && apndb.mvno_found)
		strip_non_mvno_apns(&apndb.apns);

	merged_apns = merge_apn_lists(custom_apndb.apns, apndb.apns);

	return merged_apns;
}
