/*
 * oFono - GSM Telephony Stack for Linux
 *
 * Copyright (C) 2008-2009 Intel Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "dbus-gsm.h"
#include "modem.h"
#include "driver.h"
#include "common.h"


#define PHONEBOOK_INTERFACE "org.ofono.Phonebook"
#define LEN_MAX 128
#define TYPE_INTERNATIONAL 145

#define PHONEBOOK_FLAG_CACHED 0x1

enum {
	TEL_TYPE_VOICE,
	TEL_TYPE_HOME,
	TEL_TYPE_MOBILE,
	TEL_TYPE_FAX,
	TEL_TYPE_WORK
};

struct phonebook_data {
	struct ofono_phonebook_ops *ops;
	DBusMessage *pending;
	int storage_index; /* go through all supported storage */
	int flags;
	GString *vcards; /* entries with vcard 3.0 format */
	GSList *merge_list; /* cache the entries that may need a merge */
};

struct phonebook_number {
	char *number;
	int type; /* international or not */
	int category; /* represent for "WORK", "HOME", etc */
	int prefer;
};

struct phonebook_person {
	GSList *number_list; /* one person may have more than one numbers */
	char *text;
	int hidden;
	char *group;
	char *secondtext;
	char *email;
	char *sip_uri;
	char *tel_uri;
};

static const char *storage_support[] = { "\"SM\"", "\"ME\"", NULL };
static void export_phonebook(struct ofono_modem *modem);

static struct phonebook_data *phonebook_create()
{
	struct phonebook_data *phonebook;
	phonebook = g_try_new0(struct phonebook_data, 1);

	if (!phonebook)
		return NULL;

	phonebook->vcards = g_string_new(NULL);

	return phonebook;
}

static void phonebook_destroy(gpointer data)
{
	struct ofono_modem *modem = data;
	struct phonebook_data *phonebook = modem->phonebook;

	g_string_free(phonebook->vcards, TRUE);

	g_free(phonebook);
	modem->phonebook = NULL;
}

/* according to RFC 2425, the output string may need folding */
static void vcard_printf(GString *str, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int len_temp, line_number, i;
	unsigned int line_delimit = 75;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	line_number = strlen(buf) / line_delimit + 1;

	for (i = 0; i < line_number; i++) {
		len_temp = MIN(line_delimit, strlen(buf) - line_delimit * i);
		g_string_append_len(str,  buf + line_delimit * i, len_temp);
		if (i != line_number - 1)
			g_string_append(str, "\r\n ");
	}

	g_string_append(str, "\r\n");
}

/* According to RFC 2426, we need escape following characters:
 *  '\n', '\r', ';', ',', '\'.
 */
static void add_slash(char *dest, const char *src, int len_max, int len)
{
	int i, j;

	for (i = 0, j = 0; i < len && j < len_max; i++, j++) {
		switch (src[i]) {
		case '\n':
			dest[j++] = '\\';
			dest[j] = 'n';
			break;
		case '\r':
			dest[j++] = '\\';
			dest[j] = 'r';
			break;
		case '\\':
		case ';':
		case ',':
			dest[j++] = '\\';
		default:
			dest[j] = src[i];
			break;
		}
	}
	dest[j] = 0;
	return;
}

static void vcard_printf_begin(GString *vcards)
{
	vcard_printf(vcards, "BEGIN:VCARD");
	vcard_printf(vcards, "VERSION:3.0");
}

static void vcard_printf_text(GString *vcards, const char *text)
{
	char field[LEN_MAX];
	add_slash(field, text, LEN_MAX, strlen(text));
	vcard_printf(vcards, "FN:%s", field);
}

static void vcard_printf_number(GString *vcards, const char *number, int type,
					int category, int prefer)
{
	char *pref = "", *intl = "", *category_string = "";
	char buf[128];

	if (!number || !strlen(number) || !type)
		return;

	if (prefer)
		pref = "PREF,";

	switch (category) {
	case TEL_TYPE_HOME:
		category_string = "HOME";
		break;
	case TEL_TYPE_MOBILE:
		category_string = "CELL";
		break;
	case TEL_TYPE_FAX:
		category_string = "FAX";
		break;
	case TEL_TYPE_WORK:
		category_string = "WORK";
		break;
	case TEL_TYPE_VOICE:
	default:
		category_string = "VOICE";
		break;
	}

	if ((type == TYPE_INTERNATIONAL) && (number[0] != '+'))
		intl = "+";

	sprintf(buf, "TEL;TYPE=\%s%s:\%s\%s", pref,
			category_string, intl, number);
	vcard_printf(vcards, buf, number);
}

static void vcard_printf_group(GString *vcards,	const char *group)
{
	int len = strlen(group);
	if (group && len) {
		char field[LEN_MAX];
		add_slash(field, group, LEN_MAX, len);
		vcard_printf(vcards, "CATEGORIES:%s", field);
	}
}

static void vcard_printf_email(GString *vcards, const char *email)
{
	int len = strlen(email);
	if (email && len) {
		char field[LEN_MAX];
		add_slash(field, email, LEN_MAX, len);
		vcard_printf(vcards,
				"EMAIL;TYPE=INTERNET:%s", field);
	}
}

static void vcard_printf_sip_uri(GString *vcards, const char *sip_uri)
{
	int len = strlen(sip_uri);
	if (sip_uri && len) {
		char field[LEN_MAX];
		add_slash(field, sip_uri, LEN_MAX, len);
		vcard_printf(vcards, "IMPP;TYPE=SIP:%s", field);
	}
}

static void vcard_printf_end(GString *vcards)
{
	vcard_printf(vcards, "END:VCARD");
	vcard_printf(vcards, "");
}

static DBusMessage *generate_export_entries_reply(struct ofono_modem *modem,
							DBusMessage *msg)
{
	struct phonebook_data *phonebook = modem->phonebook;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusConnection *conn = dbus_gsm_connection();

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
					phonebook->vcards);
	return reply;
}

static gboolean need_merge(const char *text)
{
	int len = strlen(text);
	char c = text[len-1];
	if ((text[len-2] == '/') &&
		((c == 'w') || (c == 'h') || (c == 'm') || (c == 'o')))
		return TRUE;
	return FALSE;
}

static void merge_field_generic(char **str1, const char *str2)
{
	if ((*str1 == NULL) && (str2 != NULL))
		*str1 = g_strdup(str2);
}

static void merge_field_number(GSList **l, const char *number, int type,
				char c, int prefer)
{
	struct phonebook_number *pn = g_new0(struct phonebook_number, 1);
	int category;

	pn->number = g_strdup(number);
	pn->type = type;
	switch (c) {
	case 'w':
	case 'o':
		category = TEL_TYPE_WORK;
		break;
	case 'h':
		category = TEL_TYPE_HOME;
		break;
	case 'm':
		category = TEL_TYPE_MOBILE;
		break;
	default:
		category = TEL_TYPE_VOICE;
		break;
	}
	pn->category = category;
	pn->prefer = prefer;
	*l = g_slist_append(*l, pn);
}

void ofono_phonebook_entry(struct ofono_modem *modem, int index,
				const char *number, int type,
				const char *text, int hidden,
				const char *group,
				const char *adnumber, int adtype,
				const char *secondtext, const char *email,
				const char *sip_uri, const char *tel_uri)
{
	struct phonebook_data *phonebook = modem->phonebook;
	char field[LEN_MAX];
	/*
	 * We need to collect all the entries that belong to one person,
	 * so that only one vCard will be generated at last.
	 * Entries only differ with '/w', '/h', '/m', etc. in field text
	 * are deemed as entries of one person.
	 */
	if (need_merge(text)) {
		GSList *l;
		int has_merge = 0;
		int len_text = strlen(text);
		char *text_temp = g_strndup(text, len_text - 2);
		struct phonebook_person *person;
		for (l = phonebook->merge_list; l; l = l->next) {
			person = l->data;
			if (!strcmp(text_temp, person->text)) {
				has_merge = 1;
				break;
			}
		}
		if (has_merge == 0) {
			person = g_new0(struct phonebook_person, 1);
			phonebook->merge_list = g_slist_append(
					phonebook->merge_list, person);
		}
		merge_field_generic(&(person->text), text_temp);
		merge_field_number(&(person->number_list), number, type,
					text[len_text - 1], 1);
		merge_field_number(&(person->number_list), adnumber, adtype,
					text[len_text - 1], 0);
		merge_field_generic(&(person->group), group);
		merge_field_generic(&(person->secondtext), secondtext);
		merge_field_generic(&(person->email), email);
		merge_field_generic(&(person->sip_uri), sip_uri);
		merge_field_generic(&(person->tel_uri), tel_uri);
		g_free(text_temp);
		return;
	}

	vcard_printf_begin(phonebook->vcards);
	vcard_printf_text(phonebook->vcards, text);
	vcard_printf_number(phonebook->vcards, number, type,
				TEL_TYPE_VOICE, 1);
	vcard_printf_number(phonebook->vcards, adnumber, adtype,
				TEL_TYPE_VOICE, 0);
	vcard_printf_group(phonebook->vcards, group);
	vcard_printf_email(phonebook->vcards, email);
	vcard_printf_sip_uri(phonebook->vcards, sip_uri);
	vcard_printf_end(phonebook->vcards);
}

static void export_phonebook_cb(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct phonebook_data *phonebook = modem->phonebook;
	DBusConnection *conn = dbus_gsm_connection();

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_error("export_entries_one_storage_cb with %s failed",
				storage_support[phonebook->storage_index]);

	phonebook->storage_index++;
	export_phonebook(modem);
	return;
}

static void export_phonebook(struct ofono_modem *modem)
{
	struct phonebook_data *phonebook = modem->phonebook;
	DBusMessage *reply;
	const char *pb = storage_support[phonebook->storage_index];
	GSList *l, *m;

	if (pb) {
		phonebook->ops->export_entries(modem, pb,
						export_phonebook_cb, modem);
		return;
	}

	/* convert the collected entries that are already merged to vcard */
	for (l = phonebook->merge_list; l; l = l->next) {
		struct phonebook_person *person = l->data;
		vcard_printf_begin(phonebook->vcards);
		vcard_printf_text(phonebook->vcards, person->text);
		for (m = person->number_list; m; m = m->next) {
			struct phonebook_number *pn = m->data;
			vcard_printf_number(phonebook->vcards, pn->number,
					pn->type, pn->category, pn->prefer);
		}
		vcard_printf_group(phonebook->vcards, person->group);
		vcard_printf_email(phonebook->vcards, person->email);
		vcard_printf_sip_uri(phonebook->vcards, person->sip_uri);
		vcard_printf_end(phonebook->vcards);
	}

	reply = generate_export_entries_reply(modem, phonebook->pending);

	if (!reply) {
		dbus_message_unref(phonebook->pending);
		return;
	}

	dbus_gsm_pending_reply(&phonebook->pending, reply);
	phonebook->flags |= PHONEBOOK_FLAG_CACHED;
}

static DBusMessage *import_entries(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct phonebook_data *phonebook = modem->phonebook;
	DBusMessage *reply;

	if (phonebook->pending) {
		reply = dbus_gsm_busy(phonebook->pending);
		g_dbus_send_message(conn, reply);
		return NULL;
	}

	if (phonebook->flags & PHONEBOOK_FLAG_CACHED) {
		reply = generate_export_entries_reply(modem, msg);
		g_dbus_send_message(conn, reply);
		return NULL;
	}

	g_string_set_size(phonebook->vcards, 0);
	phonebook->storage_index = 0;

	phonebook->pending = dbus_message_ref(msg);
	export_phonebook(modem);

	return NULL;
}

static GDBusMethodTable phonebook_methods[] = {
	{ "Import",	"",	"s",	import_entries,
					G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable phonebook_signals[] = {
	{ }
};

int ofono_phonebook_register(struct ofono_modem *modem,
				struct ofono_phonebook_ops *ops)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->phonebook = phonebook_create();

	if (modem->phonebook == NULL)
		return -1;

	modem->phonebook->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					PHONEBOOK_INTERFACE,
					phonebook_methods, phonebook_signals,
					NULL, modem, phonebook_destroy)) {
		ofono_error("Could not register Phonebook %s", modem->path);

		phonebook_destroy(modem->phonebook);

		return -1;
	}

	modem_add_interface(modem, PHONEBOOK_INTERFACE);
	return 0;
}

void ofono_phonebook_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (modem->phonebook == NULL)
		return;

	modem_remove_interface(modem, PHONEBOOK_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path,
					PHONEBOOK_INTERFACE);
}
