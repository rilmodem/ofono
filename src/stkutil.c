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
#include "smsutil.h"
#include "stkutil.h"
#include "simutil.h"
#include "util.h"

enum stk_data_object_flag {
	DATAOBJ_FLAG_MANDATORY = 1,
	DATAOBJ_FLAG_MINIMUM = 2
};

typedef gboolean (*dataobj_handler)(struct comprehension_tlv_iter *, void *);

/*
 * Defined in TS 102.223 Section 8.13
 * GSM SMS PDUs are limited to 164 bytes according to 23.040
 */
struct gsm_sms_tpdu {
	unsigned int len;
	unsigned char tpdu[164];
};

/* Defined in TS 102.223 Section 8.1 */
static gboolean parse_dataobj_address(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_address *addr = user;
	const unsigned char *data;
	unsigned int len;
	char *number;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_ADDRESS)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	number = g_try_malloc(len * 2 - 1);
	if (number == NULL)
		return FALSE;

	addr->ton_npi = data[0];
	addr->number = number;
	extract_bcd_number(data + 1, len - 1, addr->number);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.2 */
static gboolean parse_dataobj_alpha_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	char **alpha_id = user;
	const unsigned char *data;
	unsigned int len;
	char *utf8;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_ALPHA_ID)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	utf8 = sim_string_to_utf8(data, len);

	if (utf8 == NULL)
		return FALSE;

	*alpha_id = utf8;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.3 */
static gboolean parse_dataobj_subaddress(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_subaddress *subaddr = user;
	const unsigned char *data;
	unsigned int len;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_SUBADDRESS)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1)
		return FALSE;

	if (len > sizeof(subaddr->subaddr))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	subaddr->len = len;
	memcpy(subaddr->subaddr, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.4 */
static gboolean parse_dataobj_ccp(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_ccp *ccp = user;
	const unsigned char *data;
	unsigned int len;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_CCP)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1)
		return FALSE;

	if (len > sizeof(ccp->ccp))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	ccp->len = len;
	memcpy(ccp->ccp, data, len);

	return TRUE;
}

/* Described in TS 102.223 Section 8.8 */
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

/* Defined in TS 102.223 Section 8.9 */
static gboolean parse_dataobj_item(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_item *item = user;
	const unsigned char *data;
	unsigned int len;
	char *utf8;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_ITEM)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* The identifier is between 0x01 and 0xFF */
	if (data[0] == 0)
		return FALSE;

	utf8 = sim_string_to_utf8(data + 1, len - 1);

	if (utf8 == NULL)
		return FALSE;

	item->id = data[0];
	item->text = utf8;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.10 */
static gboolean parse_dataobj_item_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	unsigned char *id = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_ITEM_ID)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) != 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	*id = data[0];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.11 */
static gboolean parse_dataobj_response_len(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_response_length *response_len = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_RESPONSE_LENGTH)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	response_len->min = data[0];
	response_len->max = data[1];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.12 */
static gboolean parse_dataobj_result(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_result *result = user;
	const unsigned char *data;
	unsigned int len;
	unsigned char *additional;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_RESULT)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if ((len < 2) && ((data[0] == 0x20) || (data[0] == 0x21) ||
				(data[0] == 0x26) || (data[0] == 0x38) ||
				(data[0] == 0x39) || (data[0] == 0x3a) ||
				(data[0] == 0x3c) || (data[0] == 0x3d)))
		return FALSE;

	additional = g_try_malloc(len - 1);
	if (additional == NULL)
		return FALSE;

	result->type = data[0];
	result->additional_len = len - 1;
	result->additional = additional;
	memcpy(result->additional, data + 1, len - 1);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.13 */
static gboolean parse_dataobj_gsm_sms_tpdu(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct gsm_sms_tpdu *tpdu = user;
	const unsigned char *data;
	unsigned int len;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_GSM_SMS_TPDU)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1 || len > 164)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	tpdu->len = len;
	memcpy(tpdu->tpdu, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.15 */
static gboolean parse_dataobj_text(struct comprehension_tlv_iter *iter,
					void *user)
{
	char **text = user;
	unsigned int len;
	enum stk_data_object_type tag;

	tag = comprehension_tlv_iter_get_tag(iter);

	if (tag != STK_DATA_OBJECT_TYPE_TEXT &&
			tag != STK_DATA_OBJECT_TYPE_DEFAULT_TEXT)
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
	} else {
		if (tag == STK_DATA_OBJECT_TYPE_DEFAULT_TEXT)
			return FALSE;

		*text = NULL;
	}

	return TRUE;
}

/* Defined in TS 102.223 Section 8.16 */
static gboolean parse_dataobj_tone(struct comprehension_tlv_iter *iter,
						void *user)
{
	unsigned char *tone = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_TONE)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) !=  1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	*tone = data[0];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.18 */
static gboolean parse_dataobj_file_list(struct comprehension_tlv_iter *iter,
						void *user)
{
	GSList **fl = user;
	const unsigned char *data;
	unsigned int len;
	unsigned int i;
	unsigned int start;
	struct stk_file *sf;
	unsigned char last_type;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_FILE_LIST)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 5)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* SIM EFs always start with ROOT MF, 0x3f */
	if (data[1] != 0x3f)
		return FALSE;

	start = 1;
	last_type = 0x3f;

	for (i = 3; i < len; i += 2) {
		/* Check the validity of file type.
		 * According to TS 11.11, each file id contains of two bytes,
		 * in which the first byte is the type of file. For GSM is:
		 * 0x3f: master file
		 * 0x7f: 1st level dedicated file
		 * 0x5f: 2nd level dedicated file
		 * 0x2f: elementary file under the master file
		 * 0x6f: elementary file under 1st level dedicated file
		 * 0x4f: elementary file under 2nd level dedicated file
		 */
		switch (data[i]) {
		case 0x3f:
			if ((last_type != 0x2f) && (last_type != 0x6f) &&
					(last_type != 0x4f))
				goto error;

			start = i;

			break;
		case 0x2f:
			if (last_type != 0x3f)
				goto error;
			break;
		case 0x6f:
			if (last_type != 0x7f)
				goto error;
			break;
		case 0x4f:
			if (last_type != 0x5f)
				goto error;
			break;
		case 0x7f:
			if (last_type != 0x3f)
				goto error;
			break;
		case 0x5f:
			if (last_type != 0x7f)
				goto error;
			break;
		default:
			goto error;
		}

		if ((data[i] == 0x2f) || (data[i] == 0x6f) ||
						(data[i] == 0x4f)) {
			if (i + 1 >= len)
				goto error;

			sf = g_try_new0(struct stk_file, 1);
			if (sf == NULL)
				goto error;

			sf->len = i - start + 2;
			memcpy(sf->file, data + start, i - start + 2);
			*fl = g_slist_prepend(*fl, sf);
		}

		last_type = data[i];
	}

	if ((data[len - 2] != 0x2f) && (data[len - 2] != 0x6f) &&
						(data[len - 2] != 0x4f))
		goto error;

	*fl = g_slist_reverse(*fl);
	return TRUE;

error:
	g_slist_foreach(*fl, (GFunc)g_free, NULL);
	g_slist_free(*fl);
	return FALSE;
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

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) != 0)
		return FALSE;

	*resp = TRUE;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.72 */
static gboolean parse_dataobj_text_attr(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_text_attribute *attr = user;
	const unsigned char *data;
	unsigned int len;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE)
		return FALSE;

	len = comprehension_tlv_iter_get_length(iter);

	if (len > 127)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	memcpy(attr->attributes, data, len);
	attr->len = len;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.80 */
static gboolean parse_dataobj_frame_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	unsigned char *frame_id = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_tag(iter) !=
			STK_DATA_OBJECT_TYPE_FRAME_ID)
		return FALSE;

	if (comprehension_tlv_iter_get_length(iter) != 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] >= 0x10)
		return FALSE;

	*frame_id = data[0];

	return TRUE;
}

static dataobj_handler handler_for_type(enum stk_data_object_type type)
{
	switch (type) {
	case STK_DATA_OBJECT_TYPE_ADDRESS:
		return parse_dataobj_address;
	case STK_DATA_OBJECT_TYPE_ALPHA_ID:
		return parse_dataobj_alpha_id;
	case STK_DATA_OBJECT_TYPE_SUBADDRESS:
		return parse_dataobj_subaddress;
	case STK_DATA_OBJECT_TYPE_CCP:
		return parse_dataobj_ccp;
	case STK_DATA_OBJECT_TYPE_DURATION:
		return parse_dataobj_duration;
	case STK_DATA_OBJECT_TYPE_ITEM:
		return parse_dataobj_item;
	case STK_DATA_OBJECT_TYPE_ITEM_ID:
		return parse_dataobj_item_id;
	case STK_DATA_OBJECT_TYPE_RESPONSE_LENGTH:
		return parse_dataobj_response_len;
	case STK_DATA_OBJECT_TYPE_RESULT:
		return parse_dataobj_result;
	case STK_DATA_OBJECT_TYPE_GSM_SMS_TPDU:
		return parse_dataobj_gsm_sms_tpdu;
	case STK_DATA_OBJECT_TYPE_TEXT:
	case STK_DATA_OBJECT_TYPE_DEFAULT_TEXT:
		return parse_dataobj_text;
	case STK_DATA_OBJECT_TYPE_TONE:
		return parse_dataobj_tone;
	case STK_DATA_OBJECT_TYPE_FILE_LIST:
		return parse_dataobj_file_list;
	case STK_DATA_OBJECT_TYPE_ICON_ID:
		return parse_dataobj_icon_id;
	case STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE:
		return parse_dataobj_imm_resp;
	case STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE:
		return parse_dataobj_text_attr;
	case STK_DATA_OBJECT_TYPE_FRAME_ID:
		return parse_dataobj_frame_id;
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

static void destroy_display_text(struct stk_command *command)
{
	g_free(command->display_text.text);
}

static gboolean parse_display_text(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_display_text *obj = &command->display_text;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_DISPLAY)
		return FALSE;

	obj->frame_id = 0xFF;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE, 0,
				&obj->immediate_response,
				STK_DATA_OBJECT_TYPE_DURATION, 0,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attribute,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	command->destructor = destroy_display_text;

	return TRUE;
}

static void destroy_get_inkey(struct stk_command *command)
{
	g_free(command->get_inkey.text);
}

static gboolean parse_get_inkey(struct stk_command *command,
				struct comprehension_tlv_iter *iter)
{
	struct stk_command_display_text *obj = &command->get_inkey;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	obj->frame_id = 0xFF;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE, 0,
				&obj->immediate_response,
				STK_DATA_OBJECT_TYPE_DURATION, 0,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attribute,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	command->destructor = destroy_get_inkey;

	return TRUE;
}

static void destroy_get_input(struct stk_command *command)
{
	g_free(command->get_input.text);
}

static gboolean parse_get_input(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_get_input *obj = &command->get_input;
	gboolean ret;

	obj->frame_id = 0xFF;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_RESPONSE_LENGTH,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->response_length,
				STK_DATA_OBJECT_TYPE_DEFAULT_TEXT, 0,
				&obj->default_text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attribute,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	command->destructor = destroy_get_input;

	return TRUE;
}

static void destroy_send_sms(struct stk_command *command)
{
	g_free(command->send_sms.alpha_id);
	g_free(command->send_sms.address.number);
}

static gboolean parse_send_sms(struct stk_command *command, 
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_sms *obj = &command->send_sms;
	struct gsm_sms_tpdu tpdu;
	gboolean ret;

	obj->frame_id = 0xFF;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ADDRESS, 0,
				&obj->address,
				STK_DATA_OBJECT_TYPE_GSM_SMS_TPDU, 0,
				&tpdu,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attribute,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	command->destructor = destroy_send_sms;

	if (sms_decode(tpdu.tpdu, tpdu.len, TRUE, tpdu.len, &obj->gsm_sms)
			== FALSE) {
		command->destructor(command);
		return FALSE;
	}

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
	case STK_COMMAND_TYPE_GET_INKEY:
		ok = parse_get_inkey(command, &iter);
		break;
	case STK_COMMAND_TYPE_GET_INPUT:
		ok = parse_get_input(command, &iter);
		break;
	case STK_COMMAND_TYPE_SEND_SMS:
		ok = parse_send_sms(command, &iter);
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
