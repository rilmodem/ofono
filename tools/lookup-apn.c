/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <glib.h>

#ifndef PROVIDER_DATABASE
#define PROVIDER_DATABASE  "/usr/share/mobile-broadband-provider-info" \
							"serviceproviders.xml"
#endif

static gboolean match_found;
static const char *current_element;

static const char *match_mcc;
static const char *match_mnc;
static const char *match_spn;

static char *found_apn;
static char *found_username;
static char *found_password;

static void start_element_handler(GMarkupParseContext *context,
					const gchar *element_name,
					const gchar **attribute_names,
					const gchar **attribute_values,
					gpointer user_data, GError **error)
{
	if (g_str_equal(element_name, "network-id") == TRUE) {
		const char *mcc = NULL, *mnc = NULL;
		int i;

		for (i = 0; attribute_names[i]; i++) {
			if (g_str_equal(attribute_names[i], "mcc") == TRUE)
				mcc = attribute_values[i];
			if (g_str_equal(attribute_names[i], "mnc") == TRUE)
				mnc = attribute_values[i];
		}

		if (g_strcmp0(mcc, match_mcc) == 0 &&
					g_strcmp0(mnc, match_mnc) == 0)
			match_found = TRUE;
	}

	if (match_found == FALSE)
		return;

	if (g_str_equal(element_name, "apn") == TRUE) {
		const char *apn = NULL;
		int i;

		for (i = 0; attribute_names[i]; i++) {
			if (g_str_equal(attribute_names[i], "value") == TRUE)
				apn = attribute_values[i];
		}

		if (found_apn == NULL)
			found_apn = g_strdup(apn);
	}

	current_element = element_name;
}

static void end_element_handler(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer user_data, GError **error)
{
	if (g_str_equal(element_name, "apn") == TRUE ||
				g_str_equal(element_name, "gsm") == TRUE ||
				g_str_equal(element_name, "cdma") == TRUE)
		match_found = FALSE;
}

static void text_handler(GMarkupParseContext *context,
					const gchar *text, gsize text_len,
					gpointer user_data, GError **error)
{
	if (match_found == FALSE || found_apn == NULL)
		return;

	if (g_strcmp0(current_element, "username") == 0)
		found_username = g_strndup(text, text_len);
	else if (g_strcmp0(current_element, "password") == 0)
		found_password = g_strndup(text, text_len);
}

static void error_handler(GMarkupParseContext *context,
					GError *error, gpointer user_data)
{
	printf("error\n");
}

static const GMarkupParser parser = {
	start_element_handler,
	end_element_handler,
	text_handler,
	NULL,
	error_handler,
};

static void parse_database(const char *data, ssize_t size)
{
	GMarkupParseContext *context;
	gboolean result;

	match_found = FALSE;

	context = g_markup_parse_context_new(&parser,
				G_MARKUP_TREAT_CDATA_AS_TEXT, NULL, NULL);

	result = g_markup_parse_context_parse(context, data, size, NULL);
	if (result == TRUE)
		result = g_markup_parse_context_end_parse(context, NULL);

	g_markup_parse_context_free(context);
}

static int lookup_apn(const char *mcc, const char *mnc, const char *spn)
{
	struct stat st;
	char *map;
	int fd;

	if (mcc == NULL || mnc == NULL)
		return -1;

	fd = open(PROVIDER_DATABASE, O_RDONLY);
	if (fd < 0)
		return -1;

	if (fstat(fd, &st) < 0) {
		close(fd);
		return -1;
	}

	map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == NULL || map == MAP_FAILED) {
		close(fd);
		return -1;
	}

	match_mcc = mcc;
	match_mnc = mnc;
	match_spn = spn;

	found_apn = NULL;
	found_username = NULL;
	found_password = NULL;

	parse_database(map, st.st_size);

	munmap(map, st.st_size);

	close(fd);

	printf("Network: %s%s\n", match_mcc, match_mnc);
	printf("APN: %s\n", found_apn);
	printf("Username: %s\n", found_username);
	printf("Password: %s\n", found_password);

	g_free(found_apn);
	g_free(found_username);
	g_free(found_password);

	return 0;
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

	if (argc < 3) {
		fprintf(stderr, "Missing parameters\n");
		exit(1);
	}

	lookup_apn(argv[1], argv[2], NULL);

	return 0;
}
