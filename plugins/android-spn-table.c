/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C)  2014 Canonical Ltd.
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
#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/spn-table.h>

/* TODO: consider reading path from an environment variable */
#define ANDROID_SPN_DATABASE "/system/etc/spn-conf.xml"

static GHashTable *android_spn_table;

static void android_spndb_g_set_error(GMarkupParseContext *context,
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

	g_prefix_error(error, "%s:%d ", ANDROID_SPN_DATABASE, line_number);
}

static void toplevel_spndb_start(GMarkupParseContext *context,
					const gchar *element_name,
					const gchar **attribute_names,
					const gchar **attribute_values,
					gpointer userdata, GError **error)
{
	GHashTable *spn_table = userdata;
	int i;
	const gchar *numeric = NULL;
	const gchar *spn = NULL;
	char *numeric_dup;
	char *spn_dup;

	if (!g_str_equal(element_name, "spnOverride"))
		return;

	for (i = 0; attribute_names[i]; ++i) {
		if (g_str_equal(attribute_names[i], "numeric"))
			numeric = attribute_values[i];
		else if (g_str_equal(attribute_names[i], "spn"))
			spn = attribute_values[i];
	}

	if (numeric == NULL) {
		android_spndb_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"Missing attribute: numeric");
		return;
	}

	if (spn == NULL) {
		android_spndb_g_set_error(context, error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_MISSING_ATTRIBUTE,
					"Missing attribute: spn");
		return;
	}

	numeric_dup = g_malloc(strlen(numeric) + 1);
	strcpy(numeric_dup, numeric);
	spn_dup = g_malloc(strlen(spn) + 1);
	strcpy(spn_dup, spn);

	g_hash_table_insert(spn_table, numeric_dup, spn_dup);
}

static void toplevel_spndb_end(GMarkupParseContext *context,
				const gchar *element_name,
				gpointer userdata, GError **error)
{
}

static const GMarkupParser toplevel_spndb_parser = {
	toplevel_spndb_start,
	toplevel_spndb_end,
	NULL,
	NULL,
	NULL,
};

static gboolean android_spndb_parse(const GMarkupParser *parser,
					gpointer userdata,
					GError **error)
{
	struct stat st;
	char *db;
	int fd;
	GMarkupParseContext *context;
	gboolean ret;

	fd = open(ANDROID_SPN_DATABASE, O_RDONLY);
	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"open(%s) failed: %s", ANDROID_SPN_DATABASE,
				g_strerror(errno));
		return FALSE;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"fstat(%s) failed: %s", ANDROID_SPN_DATABASE,
				g_strerror(errno));
		return FALSE;
	}

	db = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (db == MAP_FAILED) {
		close(fd);
		g_set_error(error, G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"mmap(%s) failed: %s", ANDROID_SPN_DATABASE,
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

static const char *android_get_spn(const char *numeric)
{
	return g_hash_table_lookup(android_spn_table, numeric);
}

static struct ofono_spn_table_driver android_spn_table_driver = {
	.name	 = "Android SPN table",
	.get_spn = android_get_spn
};

static int android_spn_table_init(void)
{
	GError *error = NULL;

	android_spn_table = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);

	if (android_spndb_parse(&toplevel_spndb_parser, android_spn_table,
				&error) == FALSE) {
		g_hash_table_destroy(android_spn_table);
		android_spn_table = NULL;
		return -EINVAL;
	}

	return ofono_spn_table_driver_register(&android_spn_table_driver);
}

static void android_spn_table_exit(void)
{
	ofono_spn_table_driver_unregister(&android_spn_table_driver);

	g_hash_table_destroy(android_spn_table);
}

OFONO_PLUGIN_DEFINE(androidspntable, "Android SPN table Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			android_spn_table_init, android_spn_table_exit)
