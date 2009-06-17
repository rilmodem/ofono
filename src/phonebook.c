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

struct phonebook_data {
	struct ofono_phonebook_ops *ops;
	DBusMessage *pending;
	int storage_index; /* go through all supported storage */
	int flags;
	GString *vcards; /* entries with vcard 3.0 format */
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

static void vcard_printf_number(GString *entries_vcard_pointer, int type,
				const char *number, int prefer)
{
	char *pref = "", *intl = "";
	char buf[128];

	if (prefer)
		pref = "PREF,";

	if ((type == TYPE_INTERNATIONAL) && (number[0] != '+'))
		intl = "+";

	sprintf(buf, "TEL;TYPE=\%sVOICE:\%s\%s", pref, intl, number);
	vcard_printf(entries_vcard_pointer, buf, number);
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
	int len;

	vcard_printf(phonebook->vcards, "BEGIN:VCARD");
	vcard_printf(phonebook->vcards, "VERSION:3.0");

	add_slash(field, text, LEN_MAX, strlen(text));

	vcard_printf(phonebook->vcards, "FN:%s", field);
	vcard_printf_number(phonebook->vcards, type, number, 1);

	if (group && (len = strlen(group))) {
		add_slash(field, group, LEN_MAX, len);
		vcard_printf(phonebook->vcards, "CATEGORIES:%s", field);
	}

	if (adnumber && strlen(adnumber) && adtype != -1)
		vcard_printf_number(phonebook->vcards, adtype, adnumber, 0);

	if (email && (len = strlen(email))) {
		add_slash(field, email, LEN_MAX, len);
		vcard_printf(phonebook->vcards,
				"EMAIL;TYPE=INTERNET:%s", field);
	}

	if (sip_uri && (len = strlen(sip_uri))) {
		add_slash(field, sip_uri, LEN_MAX, len);
		vcard_printf(phonebook->vcards, "IMPP;TYPE=SIP:%s", field);
	}

	vcard_printf(phonebook->vcards, "END:VCARD");
	vcard_printf(phonebook->vcards, "");
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

	if (pb) {
		phonebook->ops->export_entries(modem, pb,
						export_phonebook_cb, modem);
		return;
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
