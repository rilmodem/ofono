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

#define _(x) case x: return (#x)

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

struct cdma_data {
	const char *match_sid;
	char *provider_name;
	gboolean match_found;
};

const char *mbpi_ap_type(enum ofono_gprs_context_type type)
{
	switch (type) {
		_(OFONO_GPRS_CONTEXT_TYPE_ANY);
		_(OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		_(OFONO_GPRS_CONTEXT_TYPE_MMS);
		_(OFONO_GPRS_CONTEXT_TYPE_WAP);
		_(OFONO_GPRS_CONTEXT_TYPE_IMS);
	}

	return "OFONO_GPRS_CONTEXT_TYPE_<UNKNOWN>";
}

static GQuark mbpi_error_quark(void)
{
	return g_quark_from_static_string("ofono-mbpi-error-quark");
}

void mbpi_ap_free(struct ofono_gprs_provision_data *ap)
{
	g_free(ap->name);
	g_free(ap->apn);
	g_free(ap->username);
	g_free(ap->password);
	g_free(ap->message_proxy);
	g_free(ap->message_center);

	g_free(ap);
}

static void mbpi_g_set_error(GMarkupParseContext *context, GError **error,
				GQuark domain, gint code, const gchar *fmt, ...)
{
	va_list ap;
	gint line_number, char_number;

	g_markup_parse_context_get_position(context, &line_number,
						&char_number);
	va_start(ap, fmt);

	*error = g_error_new_valist(domain, code, fmt, ap);

	va_end(ap);

	g_prefix_error(error, "%s:%d ", MBPI_DATABASE, line_number);
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

static void usage_start(GMarkupParseContext *context,
			const gchar **attribute_names,
			const gchar **attribute_values,
			enum ofono_gprs_context_type *type, GError **error)
{
	const char *text = NULL;
	int i;

	for (i = 0; attribute_names[i]; i++)
		if (g_str_equal(attribute_names[i], "type") == TRUE)
			text = attribute_values[i];

	if (text == NULL) {
		mbpi_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"Missing attribute: type");
		return;
	}

	if (strcmp(text, "internet") == 0)
		*type = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	else if (strcmp(text, "mms") == 0)
		*type = OFONO_GPRS_CONTEXT_TYPE_MMS;
	else if (strcmp(text, "wap") == 0)
		*type = OFONO_GPRS_CONTEXT_TYPE_WAP;
	else
		mbpi_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE,
					"Unknown usage attribute: %s", text);
}

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
		usage_start(context, attribute_names, attribute_values,
				&apn->type, error);
}

static void apn_end(GMarkupParseContext *context, const gchar *element_name,
			gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "name") ||
			g_str_equal(element_name, "username") ||
			g_str_equal(element_name, "password"))
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
	mbpi_ap_free(userdata);
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

static void network_id_handler(GMarkupParseContext *context,
				struct gsm_data *gsm,
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
		mbpi_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"Missing attribute: mcc");
		return;
	}

	if (mnc == NULL) {
		mbpi_g_set_error(context, error, G_MARKUP_ERROR,
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
	struct ofono_gprs_provision_data *ap;
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
		mbpi_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"APN attribute missing");
		return;
	}

	ap = g_new0(struct ofono_gprs_provision_data, 1);
	ap->apn = g_strdup(apn);
	ap->type = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	ap->proto = OFONO_GPRS_PROTO_IP;

	g_markup_parse_context_push(context, &apn_parser, ap);
}

static void sid_handler(GMarkupParseContext *context,
				struct cdma_data *cdma,
				const gchar **attribute_names,
				const gchar **attribute_values,
				GError **error)
{
	const char *sid = NULL;
	int i;

	for (i = 0; attribute_names[i]; i++) {
		if (g_str_equal(attribute_names[i], "value") == FALSE)
			continue;

		sid = attribute_values[i];
		break;
	}

	if (sid == NULL) {
		mbpi_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"Missing attribute: sid");
		return;
	}

	if (g_str_equal(sid, cdma->match_sid))
		cdma->match_found = TRUE;
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

		network_id_handler(context, userdata, attribute_names,
					attribute_values, error);
	} else if (g_str_equal(element_name, "apn"))
		apn_handler(context, userdata, attribute_names,
				attribute_values, error);
}

static void gsm_end(GMarkupParseContext *context, const gchar *element_name,
			gpointer userdata, GError **error)
{
	struct gsm_data *gsm;
	struct ofono_gprs_provision_data *ap;

	if (!g_str_equal(element_name, "apn"))
		return;

	gsm = userdata;

	ap = g_markup_parse_context_pop(context);
	if (ap == NULL)
		return;

	if (gsm->allow_duplicates == FALSE) {
		GSList *l;

		for (l = gsm->apns; l; l = l->next) {
			struct ofono_gprs_provision_data *pd = l->data;

			if (pd->type != ap->type)
				continue;

			mbpi_g_set_error(context, error, mbpi_error_quark(),
						MBPI_ERROR_DUPLICATE,
						"Duplicate context detected");

			mbpi_ap_free(ap);
			return;
		}
	}

	gsm->apns = g_slist_append(gsm->apns, ap);
}

static const GMarkupParser gsm_parser = {
	gsm_start,
	gsm_end,
	NULL,
	NULL,
	NULL,
};

static void cdma_start(GMarkupParseContext *context, const gchar *element_name,
			const gchar **attribute_names,
			const gchar **attribute_values,
			gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "sid")) {
		struct cdma_data *cdma = userdata;
		/*
		 * For entries with multiple sid elements, don't bother
		 * searching if we already have a match
		 */
		if (cdma->match_found == TRUE)
			return;

		sid_handler(context, cdma, attribute_names, attribute_values,
				error);
	}
}

static const GMarkupParser cdma_parser = {
	cdma_start,
	NULL,
	NULL,
	NULL,
	NULL,
};

static void provider_start(GMarkupParseContext *context,
				const gchar *element_name,
				const gchar **attribute_names,
				const gchar **attribute_values,
				gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "name")) {
		struct cdma_data *cdma = userdata;

		g_free(cdma->provider_name);
		cdma->provider_name = NULL;
		g_markup_parse_context_push(context, &text_parser,
						&cdma->provider_name);
	} else if (g_str_equal(element_name, "gsm"))
		g_markup_parse_context_push(context, &skip_parser, NULL);
	else if (g_str_equal(element_name, "cdma"))
		g_markup_parse_context_push(context, &cdma_parser, userdata);
}

static void provider_end(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "name") ||
				g_str_equal(element_name, "gsm") ||
				g_str_equal(element_name, "cdma"))
		g_markup_parse_context_pop(context);

}

static const GMarkupParser provider_parser = {
	provider_start,
	provider_end,
	NULL,
	NULL,
	NULL,
};

static void toplevel_gsm_start(GMarkupParseContext *context,
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

static void toplevel_gsm_end(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "gsm") ||
			g_str_equal(element_name, "cdma"))
		g_markup_parse_context_pop(context);
}

static const GMarkupParser toplevel_gsm_parser = {
	toplevel_gsm_start,
	toplevel_gsm_end,
	NULL,
	NULL,
	NULL,
};

static void toplevel_cdma_start(GMarkupParseContext *context,
					const gchar *element_name,
					const gchar **atribute_names,
					const gchar **attribute_values,
					gpointer userdata, GError **error)
{
	struct cdma_data *cdma = userdata;

	if (g_str_equal(element_name, "provider") == FALSE)
		return;

	if (cdma->match_found == TRUE)
		g_markup_parse_context_push(context, &skip_parser, NULL);
	else
		g_markup_parse_context_push(context, &provider_parser, cdma);
}

static void toplevel_cdma_end(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer userdata, GError **error)
{
	if (g_str_equal(element_name, "provider"))
		g_markup_parse_context_pop(context);
}

static const GMarkupParser toplevel_cdma_parser = {
	toplevel_cdma_start,
	toplevel_cdma_end,
	NULL,
	NULL,
	NULL,
};

static gboolean mbpi_parse(const GMarkupParser *parser, gpointer userdata,
				GError **error)
{
	struct stat st;
	char *db;
	int fd;
	GMarkupParseContext *context;
	gboolean ret;

	fd = open(MBPI_DATABASE, O_RDONLY);
	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"open(%s) failed: %s", MBPI_DATABASE,
				g_strerror(errno));
		return FALSE;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"fstat(%s) failed: %s", MBPI_DATABASE,
				g_strerror(errno));
		return FALSE;
	}

	db = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (db == MAP_FAILED) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"mmap(%s) failed: %s", MBPI_DATABASE,
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

GSList *mbpi_lookup_apn(const char *mcc, const char *mnc,
			gboolean allow_duplicates, GError **error)
{
	struct gsm_data gsm;
	GSList *l;

	memset(&gsm, 0, sizeof(gsm));
	gsm.match_mcc = mcc;
	gsm.match_mnc = mnc;
	gsm.allow_duplicates = allow_duplicates;

	if (mbpi_parse(&toplevel_gsm_parser, &gsm, error) == FALSE) {
		for (l = gsm.apns; l; l = l->next)
			mbpi_ap_free(l->data);

		g_slist_free(gsm.apns);
		gsm.apns = NULL;
	}

	return gsm.apns;
}

char *mbpi_lookup_cdma_provider_name(const char *sid, GError **error)
{
	struct cdma_data cdma;

	memset(&cdma, 0, sizeof(cdma));
	cdma.match_sid = sid;

	if (mbpi_parse(&toplevel_cdma_parser, &cdma, error) == FALSE) {
		g_free(cdma.provider_name);
		cdma.provider_name = NULL;
	}

	return cdma.provider_name;
}
