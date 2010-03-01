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

#include <string.h>
#include <stdlib.h>

#include <glib.h>

#include <ofono/types.h>
#include "stkutil.h"
#include "smsutil.h"
#include "simutil.h"
#include "util.h"

enum stk_data_object_flag {
	DATAOBJ_FLAG_MANDATORY = 1,
	DATAOBJ_FLAG_MINIMUM = 2
};

typedef gboolean (*dataobj_handler)(struct comprehension_tlv_iter *, void *);

static gboolean parse_dataobj_duration(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_duration *duration = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_DURATION)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] > 0x02)
		return FALSE;

	if (data[1] == 0)
		return FALSE;

	duration->unit = data[0];
	duration->interval = data[1];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.15 */
static gboolean parse_dataobj_text(struct comprehension_tlv_iter *iter,
					void *user)
{
	char **text = user;
	unsigned int len;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_TEXT)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);

	/* DCS followed by some text, cannot be 1 */
	if (len == 1)
		return FALSE;

	if (len > 0) {
		const unsigned char *data =
			comprehension_tlv_iter_get_data(iter);
		unsigned char dcs = data[0];
		char *utf8;

		switch (dcs) {
		case 0x00:
		{
			long written;
			unsigned long max_to_unpack = (len - 1) * 8 / 7;
			unsigned char *unpacked = unpack_7bit(data + 1, len - 1,
								0, FALSE,
								max_to_unpack,
								&written, 0);
			if (unpacked == NULL)
				return FALSE;

			utf8 = convert_gsm_to_utf8(unpacked, written,
							NULL, NULL, 0);
			g_free(unpacked);
			break;
		}
		case 0x04:
			utf8 = convert_gsm_to_utf8(data + 1, len - 1,
							NULL, NULL, 0);
			break;
		case 0x08:
			utf8 = g_convert((const gchar *) data + 1, len - 1,
						"UTF-8//TRANSLIT", "UCS-2BE",
						NULL, NULL, NULL);
			break;
		default:
			return FALSE;;
		}

		if (utf8 == NULL)
			return FALSE;

		*text = utf8;
	} else
		*text = NULL;

	return TRUE;
}

static void destroy_display_text(struct stk_command *command)
{
	g_free(command->display_text.text);
}

/* Defined in TS 102.223 Section 8.31 */
static gboolean parse_dataobj_icon_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_icon_identifier *id = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_ICON_ID)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	id->qualifier = data[0];
	id->id = data[1];

	return TRUE;
}

/* Defined in 102.223 Section 8.43 */
static gboolean parse_dataobj_imm_resp(struct comprehension_tlv_iter *iter,
					void *user)
{
	gboolean *resp = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) != 0)
		return FALSE;

	*resp = TRUE;

	return TRUE;
}

static dataobj_handler handler_for_type(enum stk_data_object_type type)
{
	switch (type) {
	case STK_DATA_OBJECT_TYPE_TEXT:
		return parse_dataobj_text;
	case STK_DATA_OBJECT_TYPE_ICON_ID:
		return parse_dataobj_icon_id;
	case STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE:
		return parse_dataobj_imm_resp;
	case STK_DATA_OBJECT_TYPE_DURATION:
		return parse_dataobj_duration;
	default:
		return NULL;
	};
}

struct dataobj_handler_entry {
	enum stk_data_object_type type;
	int flags;
	void *data;
	gboolean parsed;
};

static gboolean parse_dataobj(struct comprehension_tlv_iter *iter,
				enum stk_data_object_type type, ...)
{
	GSList *entries = NULL;
	GSList *l;
	va_list args;
	gboolean minimum_set = TRUE;

	va_start(args, type);

	while (type != STK_DATA_OBJECT_TYPE_INVALID) {
		struct dataobj_handler_entry *entry;

		entry = g_new0(struct dataobj_handler_entry, 1);

		entry->type = type;
		entry->flags = va_arg(args, int);
		entry->data = va_arg(args, void *);

		type = va_arg(args, enum stk_data_object_type);
		entries = g_slist_prepend(entries, entry);
	}

	entries = g_slist_reverse(entries);

	for (l = entries; l; l = l->next) {
		gboolean ret;
		dataobj_handler handler;
		struct dataobj_handler_entry *entry = l->data;

		handler = handler_for_type(entry->type);
		if (handler == NULL)
			continue;

		ret = handler(iter, entry->data);
		entry->parsed = ret;

		if (ret && comprehension_tlv_iter_next(iter) == FALSE)
			break;
	}

	for (l = entries; l; l = l->next) {
		struct dataobj_handler_entry *entry = l->data;

		if ((entry->flags & DATAOBJ_FLAG_MINIMUM) &&
				entry->parsed == FALSE)
			minimum_set = TRUE;
	}

	g_slist_foreach(entries, (GFunc)g_free, NULL);
	g_slist_free(entries);

	return minimum_set;
}

static gboolean parse_display_text(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	gboolean ret;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&command->display_text.text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&command->display_text.icon_id,
				STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE, 0,
				&command->display_text.immediate_response,
				STK_DATA_OBJECT_TYPE_DURATION, 0,
				&command->display_text.duration,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	command->destructor = destroy_display_text;

	return TRUE;
}

struct stk_command *stk_command_new_from_pdu(const unsigned char *pdu,
						unsigned int len)
{
	struct ber_tlv_iter ber;
	struct comprehension_tlv_iter iter;
	const unsigned char *data;
	struct stk_command *command;
	gboolean ok;

	ber_tlv_iter_init(&ber, pdu, len);

	if (ber_tlv_iter_next(&ber) != TRUE)
		return NULL;

	/* We should be wrapped in a Proactive UICC Command Tag 0xD0 */
	if (ber_tlv_iter_get_short_tag(&ber) != 0xD0)
		return NULL;

	ber_tlv_iter_recurse_comprehension(&ber, &iter);

	/*
	 * Now parse actual command details, they come in order with
	 * Command Details TLV first, followed by Device Identities TLV
	 */
	if (comprehension_tlv_iter_next(&iter) != TRUE)
		return NULL;

	if (comprehension_tlv_iter_get_tag(&iter) !=
			STK_DATA_OBJECT_TYPE_COMMAND_DETAILS)
		return NULL;

	if (comprehension_tlv_iter_get_length(&iter) != 0x03)
		return NULL;

	data = comprehension_tlv_iter_get_data(&iter);

	command = g_new0(struct stk_command, 1);

	command->number = data[0];
	command->type = data[1];
	command->qualifier = data[2];

	if (comprehension_tlv_iter_next(&iter) != TRUE)
		goto fail;

	if (comprehension_tlv_iter_get_tag(&iter) !=
			STK_DATA_OBJECT_TYPE_DEVICE_IDENTITIES)
		goto fail;

	if (comprehension_tlv_iter_get_length(&iter) != 0x02)
		goto fail;

	data = comprehension_tlv_iter_get_data(&iter);

	command->src = data[0];
	command->dst = data[1];

	if (comprehension_tlv_iter_next(&iter) != TRUE)
		return FALSE;

	switch (command->type) {
	case STK_COMMAND_TYPE_DISPLAY_TEXT:
		ok = parse_display_text(command, &iter);
		break;
	default:
		ok = FALSE;
		break;
	};

	if (ok)
		return command;

fail:
	if (command->destructor)
		command->destructor(command);

	g_free(command);

	return NULL;
}

void stk_command_free(struct stk_command *command)
{
	if (command->destructor)
		command->destructor(command);

	g_free(command);
}
