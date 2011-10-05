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

#ifndef MBPI_DATABASE
#define MBPI_DATABASE  "/usr/share/mobile-broadband-provider-info/" \
							"serviceproviders.xml"
#endif

#include "mbpi.h"

enum MBPI_ERROR {
	MBPI_ERROR_DUPLICATE,
};

struct gsm_data {
	const char *match_mcc;
	const char *match_mnc;
	GSList *apns;
	gboolean match_found;
	gboolean allow_duplicates;
};

static GQuark mbpi_error_quark(void)
{
	return g_quark_from_static_string("ofono-mbpi-error-quark");
}

void mbpi_provision_data_free(struct ofono_gprs_provision_data *data)
{
	g_free(data->name);
	g_free(data->apn);
	g_free(data->username);
	g_free(data->password);
	g_free(data->message_proxy);
	g_free(data->message_center);

	g_free(data);
}

static void text_handler(GMarkupParseContext *context,
				const gchar *text, gsize text_len,
				gpointer userdata, GError **error)
{
	char **string = userdata;

	*string = g_strndup(text, text_len);
}

static const GMarkupParser text_parser = {
	NULL,
	NULL,
	text_handler,
	NULL,
	NULL,
};

static void usage_handler(GMarkupParseContext *context,
				const gchar *text, gsize text_len,
				gpointer userdata, GError **error)
{
	enum ofono_gprs_context_type *type = userdata;

	if (strncmp(text, "internet", text_len) == 0)
		*type = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	else if (strncmp(text, "mms", text_len) == 0)
		*type = OFONO_GPRS_CONTEXT_TYPE_MMS;
	else if (strncmp(text, "wap", text_len) == 0)
		*type = OFONO_GPRS_CONTEXT_TYPE_WAP;
	else
		g_set_error(error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE,
					"Unknown usage attribute: %.*s",
					(int) text_len, text);
}

static const GMarkupParser usage_parser = {
	NULL,
	NULL,
	usage_handler,
	NULL,
	NULL,
};

static void apn_start(GMarkupParseContext *context, const gchar *element_name,
			const gchar **attribute_names,
			const gchar **attribute_values,
			gpointer userdata, GError **error)
{
	struct ofono_gprs_provision_data *apn = userdata;

	if (g_str_equal(element_name, "name"))
		g_markup_parse_context_push(context, &text_parser, &apn->name);
	else if (g_str_equal(element_name, "username"))
		g_markup_parse_context_push(context, &text_parser,
						&apn->username);
	else if (g_str_equal(element_name, "password"))
		g_markup_parse_context_push(context, &text_parser,
						&apn->password);
	else if (g_str_equal(element_name, "usage"))
		g_markup_parse_context_push(context, &usage_parser,
						&apn->type);
}

static void apn_end(GMarkupParseContext *context, const gchar *element_name,
			gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "name") ||
			g_str_equal(element_name, "username") ||
			g_str_equal(element_name, "password") ||
			g_str_equal(element_name, "usage"))
		g_markup_parse_context_pop(context);
}

static void apn_error(GMarkupParseContext *context, GError *error,
			gpointer userdata)
{
	/*
	 * Note that even if the error happened in a subparser, this will
	 * be called.  So we always perform cleanup of the allocated
	 * provision data
	 */
	mbpi_provision_data_free(userdata);
}

static const GMarkupParser apn_parser = {
	apn_start,
	apn_end,
	NULL,
	NULL,
	apn_error,
};

static const GMarkupParser skip_parser = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

static void network_id_handler(struct gsm_data *gsm,
				const gchar **attribute_names,
				const gchar **attribute_values,
				GError **error)
{
	const char *mcc = NULL, *mnc = NULL;
	int i;

	for (i = 0; attribute_names[i]; i++) {
		if (g_str_equal(attribute_names[i], "mcc") == TRUE)
			mcc = attribute_values[i];
		if (g_str_equal(attribute_names[i], "mnc") == TRUE)
			mnc = attribute_values[i];
	}

	if (mcc == NULL) {
		g_set_error(error, G_MARKUP_ERROR,
				G_MARKUP_ERROR_MISSING_ATTRIBUTE,
				"Missing attribute: mcc");
		return;
	}

	if (mnc == NULL) {
		g_set_error(error, G_MARKUP_ERROR,
				G_MARKUP_ERROR_MISSING_ATTRIBUTE,
				"Missing attribute: mnc");
		return;
	}

	if (g_str_equal(mcc, gsm->match_mcc) &&
			g_str_equal(mnc, gsm->match_mnc))
		gsm->match_found = TRUE;
}

static void apn_handler(GMarkupParseContext *context, struct gsm_data *gsm,
			const gchar **attribute_names,
			const gchar **attribute_values,
			GError **error)
{
	struct ofono_gprs_provision_data *pd;
	const char *apn;
	int i;

	if (gsm->match_found == FALSE) {
		g_markup_parse_context_push(context, &skip_parser, NULL);
		return;
	}

	for (i = 0, apn = NULL; attribute_names[i]; i++) {
		if (g_str_equal(attribute_names[i], "value") == FALSE)
			continue;

		apn = attribute_values[i];
		break;
	}

	if (apn == NULL) {
		g_set_error(error, G_MARKUP_ERROR,
				G_MARKUP_ERROR_MISSING_ATTRIBUTE,
				"APN attribute missing");
		return;
	}

	pd = g_new0(struct ofono_gprs_provision_data, 1);
	pd->apn = g_strdup(apn);
	pd->type = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	pd->proto = OFONO_GPRS_PROTO_IP;

	g_markup_parse_context_push(context, &apn_parser, pd);
}

static void gsm_start(GMarkupParseContext *context, const gchar *element_name,
			const gchar **attribute_names,
			const gchar **attribute_values,
			gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "network-id")) {
		struct gsm_data *gsm = userdata;

		/*
		 * For entries with multiple network-id elements, don't bother
		 * searching if we already have a match
		 */
		if (gsm->match_found == TRUE)
			return;

		network_id_handler(userdata, attribute_names, attribute_values,
					error);
	} else if (g_str_equal(element_name, "apn"))
		apn_handler(context, userdata, attribute_names,
				attribute_values, error);
}

static void gsm_end(GMarkupParseContext *context, const gchar *element_name,
			gpointer userdata, GError **error)
{
	struct gsm_data *gsm = userdata;

	if (g_str_equal(element_name, "apn")) {
		struct ofono_gprs_provision_data *apn =
					g_markup_parse_context_pop(context);

		if (apn == NULL)
			return;

		if (gsm->allow_duplicates == FALSE) {
			GSList *l;

			for (l = gsm->apns; l; l = l->next) {
				struct ofono_gprs_provision_data *pd = l->data;

				if (pd->type != apn->type)
					continue;

				g_set_error(error, mbpi_error_quark(),
						MBPI_ERROR_DUPLICATE,
						"Duplicate context detected");

				mbpi_provision_data_free(apn);
				return;
			}
		}

		gsm->apns = g_slist_append(gsm->apns, apn);
	}
}

static const GMarkupParser gsm_parser = {
	gsm_start,
	gsm_end,
	NULL,
	NULL,
	NULL,
};

static void toplevel_start(GMarkupParseContext *context,
					const gchar *element_name,
					const gchar **atribute_names,
					const gchar **attribute_values,
					gpointer userdata, GError **error)
{
	struct gsm_data *gsm = userdata;

	if (g_str_equal(element_name, "gsm")) {
		gsm->match_found = FALSE;
		g_markup_parse_context_push(context, &gsm_parser, gsm);
	} else if (g_str_equal(element_name, "cdma"))
		g_markup_parse_context_push(context, &skip_parser, NULL);
}

static void toplevel_end(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "gsm") ||
			g_str_equal(element_name, "cdma"))
		g_markup_parse_context_pop(context);
}

static const GMarkupParser toplevel_parser = {
	toplevel_start,
	toplevel_end,
	NULL,
	NULL,
	NULL,
};

static gboolean mbpi_parse(const char *data, ssize_t size,
				struct gsm_data *gsm, GError **error)
{
	GMarkupParseContext *context;
	gboolean ret;

	context = g_markup_parse_context_new(&toplevel_parser,
						G_MARKUP_TREAT_CDATA_AS_TEXT,
						gsm, NULL);

	ret = g_markup_parse_context_parse(context, data, size, error);

	if (ret == TRUE)
		g_markup_parse_context_end_parse(context, error);

	g_markup_parse_context_free(context);

	return ret;
}

GSList *mbpi_lookup(const char *mcc, const char *mnc,
			gboolean allow_duplicates, GError **error)
{
	struct stat st;
	char *db;
	int fd;
	struct gsm_data gsm;
	GSList *l;

	fd = open(MBPI_DATABASE, O_RDONLY);
	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"open failed: %s", g_strerror(errno));
		return NULL;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"fstat failed: %s", g_strerror(errno));
		return NULL;
	}

	db = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (db == MAP_FAILED) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"mmap failed: %s", g_strerror(errno));
		return NULL;
	}

	memset(&gsm, 0, sizeof(gsm));
	gsm.match_mcc = mcc;
	gsm.match_mnc = mnc;
	gsm.allow_duplicates = allow_duplicates;

	if (mbpi_parse(db, st.st_size, &gsm, error) == FALSE) {
		for (l = gsm.apns; l; l = l->next)
			mbpi_provision_data_free(l->data);

		g_slist_free(gsm.apns);
		gsm.apns = NULL;
	}

	munmap(db, st.st_size);
	close(fd);

	return gsm.apns;
}
