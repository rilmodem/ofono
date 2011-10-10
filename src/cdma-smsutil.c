/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011  Nokia Corporation and/or its subsidiary(-ies).
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
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>

#include "cdma-smsutil.h"

#define uninitialized_var(x) x = x

enum cdma_sms_rec_flag {
	CDMA_SMS_REC_FLAG_MANDATORY =	1,
};

typedef gboolean (*rec_handler)(const guint8 *, guint8, void *);

struct simple_iter {
	guint8 max;
	const guint8 *pdu;
	guint8 pos;
	guint8 id;
	guint8 len;
	const guint8 *data;
};

static void simple_iter_init(struct simple_iter *iter,
				const guint8 *pdu, guint8 len)
{
	iter->pdu = pdu;
	iter->max = len;
	iter->pos = 0;
	iter->id = 0;
	iter->len = 0;
	iter->data = NULL;
}

static gboolean simple_iter_next(struct simple_iter *iter)
{
	const guint8 *pdu = iter->pdu + iter->pos;
	const guint8 *end = iter->pdu + iter->max;
	guint8 id;
	guint8 len;

	if (pdu == end)
		return FALSE;

	id = *pdu;
	pdu++;

	if (pdu == end)
		return FALSE;

	len = *pdu++;

	if (pdu + len > end)
		return FALSE;

	iter->id = id;
	iter->len = len;
	iter->data = pdu;

	iter->pos = pdu + len - iter->pdu;

	return TRUE;
}

static guint8 simple_iter_get_id(struct simple_iter *iter)
{
	return iter->id;
}

static guint8 simple_iter_get_length(struct simple_iter *iter)
{
	return iter->len;
}

static const guint8 *simple_iter_get_data(struct simple_iter *iter)
{
	return iter->data;
}

static inline void set_bitmap(guint32 *bitmap, guint8 pos)
{
	*bitmap = *bitmap | (1 << pos);
}

/* Unpacks the byte stream. The field has to be <= 8 bits. */
static guint8 bit_field_unpack(const guint8 *buf, guint16 offset, guint8 nbit)
{
	guint8 bit_pos;
	guint8 val = 0;
	const guint8 *pdu;

	pdu = buf + (offset >> 3);
	bit_pos = 8 - (offset & 0x7);

	/* Field to be extracted is within current byte */
	if (nbit <= bit_pos)
		return (*pdu >> (bit_pos - nbit)) & ((1 << nbit) - 1);

	/* Field to be extracted crossing two bytes */
	val = *pdu & ((1 << bit_pos) - 1);
	nbit -= bit_pos;
	pdu++;

	return (val << nbit) | (*pdu >> (8 - nbit));
}

/* Convert CDMA DTMF digits into a string */
static gboolean dtmf_to_ascii(char *buf, const guint8 *addr,
					guint8 num_fields)
{
	/*
	 * Mapping from binary DTMF code to the digit it represents.
	 * As defined in Table 2.7.1.3.2.4-4 of 3GPP2 C.S0005-E v2.0.
	 * Note, 0 is NOT a valid value and not mapped to
	 * any valid DTMF digit.
	 */
	static const char dtmf_digits[13] = {0, '1', '2', '3', '4', '5', '6',
						'7', '8', '9', '0', '*', '#'};
	guint8 index;
	guint8 value;

	for (index = 0; index < num_fields; index++) {
		if (addr[index] == 0 || addr[index] > 12)
			return FALSE;  /* Invalid digit in address field */

		value = addr[index];
		buf[index] = dtmf_digits[value];
	}

	buf[index] = 0; /* Make it NULL terminated string */

	return TRUE;
}

const char *cdma_sms_address_to_string(const struct cdma_sms_address *addr)
{
	static char buf[CDMA_SMS_MAX_ADDR_FIELDS + 1];

	/* TODO: Only support CDMA_SMS_DIGIT_MODE_4BIT_DTMF currently */
	switch (addr->digit_mode) {
	case CDMA_SMS_DIGIT_MODE_4BIT_DTMF:
		if (dtmf_to_ascii(buf, addr->address,
					addr->num_fields) == TRUE)
			return buf;
		else
			return NULL;
	case CDMA_SMS_DIGIT_MODE_8BIT_ASCII:
		return NULL;
	}

	return NULL;
}

/* Decode Teleservice ID */
static gboolean cdma_sms_decode_teleservice(const guint8 *buf, guint8 len,
								void *data)
{
	enum cdma_sms_teleservice_id *id = data;

	*id = bit_field_unpack(buf, 0, 8) << 8 |
				bit_field_unpack(buf, 8, 8);

	switch (*id) {
	case CDMA_SMS_TELESERVICE_ID_CMT91:
	case CDMA_SMS_TELESERVICE_ID_WPT:
	case CDMA_SMS_TELESERVICE_ID_WMT:
	case CDMA_SMS_TELESERVICE_ID_VMN:
	case CDMA_SMS_TELESERVICE_ID_WAP:
	case CDMA_SMS_TELESERVICE_ID_WEMT:
	case CDMA_SMS_TELESERVICE_ID_SCPT:
	case CDMA_SMS_TELESERVICE_ID_CATPT:
		return TRUE;
	}

	return FALSE; /* Invalid teleservice type */
}

/* Decode Address parameter record */
static gboolean cdma_sms_decode_addr(const guint8 *buf, guint8 len,
							void *data)
{
	struct cdma_sms_address *addr = data;
	guint16 bit_offset = 0;
	guint8  chari_len;
	guint16 total_num_bits = len * 8;
	guint8  index;

	addr->digit_mode = bit_field_unpack(buf, bit_offset, 1);
	bit_offset += 1;

	addr->number_mode = bit_field_unpack(buf, bit_offset, 1);
	bit_offset += 1;

	if (addr->digit_mode == CDMA_SMS_DIGIT_MODE_8BIT_ASCII) {
		if (addr->number_mode == CDMA_SMS_NUM_MODE_DIGIT)
			addr->digi_num_type =
				bit_field_unpack(buf, bit_offset, 3);
		else
			addr->data_nw_num_type =
				bit_field_unpack(buf, bit_offset, 3);

		bit_offset += 3;

		if (addr->number_mode == CDMA_SMS_NUM_MODE_DIGIT) {
			if (bit_offset + 4 > total_num_bits)
				return FALSE;

			addr->number_plan =
				bit_field_unpack(buf, bit_offset, 4);
			bit_offset += 4;
		}
	}

	if (bit_offset + 8 > total_num_bits)
		return FALSE;

	addr->num_fields = bit_field_unpack(buf, bit_offset, 8);
	bit_offset += 8;

	if (addr->digit_mode == CDMA_SMS_DIGIT_MODE_4BIT_DTMF)
		chari_len = 4;
	else
		chari_len = 8;

	if ((bit_offset + chari_len * addr->num_fields) > total_num_bits)
		return FALSE;

	for (index = 0; index < addr->num_fields; index++) {
		addr->address[index] = bit_field_unpack(buf,
							bit_offset,
							chari_len);
		bit_offset += chari_len;
	}

	return TRUE;
}

static char *decode_text_7bit_ascii(const struct cdma_sms_ud *ud)
{
	char *buf;

	buf = g_new(char, ud->num_fields + 1);
	if (buf == NULL)
		return NULL;

	memcpy(buf, ud->chari, ud->num_fields);
	buf[ud->num_fields] = 0; /* Make it NULL terminated string */

	return buf;
}

char *cdma_sms_decode_text(const struct cdma_sms_ud *ud)
{
	switch (ud->msg_encoding) {
	case CDMA_SMS_MSG_ENCODING_OCTET:
	case CDMA_SMS_MSG_ENCODING_EXTENDED_PROTOCOL_MSG:
		return NULL; /* TODO */
	case CDMA_SMS_MSG_ENCODING_7BIT_ASCII:
		return decode_text_7bit_ascii(ud);
	case CDMA_SMS_MSG_ENCODING_IA5:
	case CDMA_SMS_MSG_ENCODING_UNICODE:
	case CDMA_SMS_MSG_ENCODING_SHIFT_JIS:
	case CDMA_SMS_MSG_ENCODING_KOREAN:
	case CDMA_SMS_MSG_ENCODING_LATIN_HEBREW:
	case CDMA_SMS_MSG_ENCODING_LATIN:
	case CDMA_SMS_MSG_ENCODING_GSM_7BIT:
	case CDMA_SMS_MSG_ENCODING_GSM_DATA_CODING:
		return NULL; /* TODO */
	}

	return NULL;
}

/* Decode User Data */
static gboolean cdma_sms_decode_ud(const guint8 *buf, guint8 len, void *data)
{
	guint16 bit_offset = 0;
	guint8  chari_len = 0;
	guint16 total_num_bits = len * 8;
	guint8  index;
	enum cdma_sms_msg_encoding  msg_encoding;
	struct cdma_sms_ud *ud = data;

	if (total_num_bits < 13)
		return FALSE;

	msg_encoding = bit_field_unpack(buf, bit_offset, 5);
	ud->msg_encoding =  msg_encoding;
	bit_offset += 5;

	if (msg_encoding == CDMA_SMS_MSG_ENCODING_EXTENDED_PROTOCOL_MSG ||
		msg_encoding == CDMA_SMS_MSG_ENCODING_GSM_DATA_CODING) {
		/*
		 * Skip message type field for now.
		 * TODO: Add support for message type field.
		 */
		bit_offset += 8;
	}

	if (bit_offset + 8 > total_num_bits)
		return FALSE;

	ud->num_fields = bit_field_unpack(buf, bit_offset, 8);
	bit_offset += 8;

	switch (msg_encoding) {
	case CDMA_SMS_MSG_ENCODING_OCTET:
		chari_len = 8;
		break;
	case CDMA_SMS_MSG_ENCODING_EXTENDED_PROTOCOL_MSG:
		return FALSE; /* TODO */
	case CDMA_SMS_MSG_ENCODING_7BIT_ASCII:
	case CDMA_SMS_MSG_ENCODING_IA5:
		chari_len = 7;
		break;
	case CDMA_SMS_MSG_ENCODING_UNICODE:
	case CDMA_SMS_MSG_ENCODING_SHIFT_JIS:
	case CDMA_SMS_MSG_ENCODING_KOREAN:
		return FALSE; /* TODO */
	case CDMA_SMS_MSG_ENCODING_LATIN_HEBREW:
	case CDMA_SMS_MSG_ENCODING_LATIN:
		chari_len = 8;
		break;
	case CDMA_SMS_MSG_ENCODING_GSM_7BIT:
		chari_len = 7;
		break;
	case CDMA_SMS_MSG_ENCODING_GSM_DATA_CODING:
		return FALSE; /* TODO */
	}

	/* TODO: Add support for all other encoding types */
	if (chari_len == 0)
		return FALSE;

	if (bit_offset + chari_len * ud->num_fields > total_num_bits)
		return FALSE;

	for (index = 0; index < ud->num_fields; index++) {
		ud->chari[index] = bit_field_unpack(buf,
						bit_offset,
						chari_len);
		bit_offset += chari_len;
	}

	return TRUE;
}

/* Decode Message Identifier */
static gboolean cdma_sms_decode_message_id(const guint8 *buf, guint8 len,
						void *data)
{
	struct cdma_sms_identifier *id = data;

	if (len != 3)
		return FALSE;

	id->msg_type = bit_field_unpack(buf, 0, 4);

	if (id->msg_type <= 0 ||
			id->msg_type > CDMA_SMS_MSG_TYPE_SUBMIT_REPORT)
		return FALSE; /* Invalid message type */

	id->msg_id = (bit_field_unpack(buf, 4, 8) << 8) |
			bit_field_unpack(buf, 12, 8);

	id->header_ind = bit_field_unpack(buf, 20, 1);

	return TRUE;
}

static gboolean find_and_decode(struct simple_iter *iter, guint8 rec_id,
					rec_handler handler, void *data)
{
	guint8 id;
	guint8 len;
	const guint8 *buf;

	while (simple_iter_next(iter) == TRUE) {
		id = simple_iter_get_id(iter);
		if (id != rec_id)
			continue;

		len = simple_iter_get_length(iter);
		buf = simple_iter_get_data(iter);

		return handler(buf, len, data);
	}

	return FALSE;
}

static rec_handler subparam_handler_for_id(enum cdma_sms_subparam_id id)
{
	switch (id) {
	case CDMA_SMS_SUBPARAM_ID_MESSAGE_ID:
		return cdma_sms_decode_message_id;
	case CDMA_SMS_SUBPARAM_ID_USER_DATA:
		return cdma_sms_decode_ud;
	case CDMA_SMS_SUBPARAM_ID_USER_RESPONSE_CODE:
	case CDMA_SMS_SUBPARAM_ID_MC_TIME_STAMP:
	case CDMA_SMS_SUBPARAM_ID_VALIDITY_PERIOD_ABSOLUTE:
	case CDMA_SMS_SUBPARAM_ID_VALIDITY_PERIOD_RELATIVE:
	case CDMA_SMS_SUBPARAM_ID_DEFERRED_DELIVERY_TIME_ABSOLUTE:
	case CDMA_SMS_SUBPARAM_ID_DEFERRED_DELIVERY_TIME_RELATIVE:
	case CDMA_SMS_SUBPARAM_ID_PRIORITY_INDICATOR:
	case CDMA_SMS_SUBPARAM_ID_PRIVACY_INDICATOR:
	case CDMA_SMS_SUBPARAM_ID_REPLY_OPTION:
	case CDMA_SMS_SUBPARAM_ID_NUMBER_OF_MESSAGES:
	case CDMA_SMS_SUBPARAM_ID_ALERT_ON_MESSAGE_DELIVERY:
	case CDMA_SMS_SUBPARAM_ID_LANGUAGE_INDICATOR:
	case CDMA_SMS_SUBPARAM_ID_CALL_BACK_NUMBER:
	case CDMA_SMS_SUBPARAM_ID_MESSAGE_DISPLAY_MODE:
	case CDMA_SMS_SUBPARAM_ID_MULTIPLE_ENCODING_USER_DATA:
	case CDMA_SMS_SUBPARAM_ID_MESSAGE_DEPOSIT_INDEX:
	case CDMA_SMS_SUBPARAM_ID_SERVICE_CATEGORY_PROGRAM_DATA:
	case CDMA_SMS_SUBPARAM_ID_SERVICE_CATEGORY_PROGRAM_RESULT:
	case CDMA_SMS_SUBPARAM_ID_MESSAGE_STATUS:
	case CDMA_SMS_SUBPARAM_ID_TP_FAILURE_CAUSE:
	case CDMA_SMS_SUBPARAM_ID_ENHANCED_VMN:
	case CDMA_SMS_SUBPARAM_ID_ENHANCED_VMN_ACK:
		return NULL; /* TODO */
	}

	return NULL;
}

struct subparam_handler_entry {
	enum cdma_sms_subparam_id id;
	int flags;
	gboolean found;
	void *data;
};

static gboolean decode_subparams(struct simple_iter *iter, guint32 *bitmap,
					void *data, ...)
{
	GSList *entries = NULL;
	GSList *l;
	va_list args;
	gboolean decode_result = TRUE;

	va_start(args, data);

	while (data != NULL) {
		struct subparam_handler_entry *entry;

		entry = g_new0(struct subparam_handler_entry, 1);

		entry->data = data;
		entry->id = va_arg(args, enum cdma_sms_subparam_id);
		entry->flags = va_arg(args, int);

		data = va_arg(args, void *);
		entries = g_slist_prepend(entries, entry);
	}

	va_end(args);

	entries = g_slist_reverse(entries);

	l = entries;
	while (simple_iter_next(iter) == TRUE) {
		rec_handler handler;
		struct subparam_handler_entry *entry;
		guint8 subparam_len;
		const guint8 *subparam_buf;
		GSList *l2;

		for (l2 = l; l2; l2 = l2->next) {
			entry = l2->data;

			if (simple_iter_get_id(iter) == entry->id)
				break;
		}

		/* Ignore unexpected subparameter record */
		if (l2 == NULL)
			continue;

		entry->found = TRUE;

		subparam_len = simple_iter_get_length(iter);
		subparam_buf = simple_iter_get_data(iter);

		handler = subparam_handler_for_id(entry->id);

		decode_result = handler(subparam_buf,
					subparam_len,
					entry->data);
		if (decode_result == FALSE)
			break; /* Stop if decoding failed */

		set_bitmap(bitmap, entry->id);
	}

	for (; l; l = l->next) {
		struct subparam_handler_entry *entry = l->data;

		if ((entry->flags & CDMA_SMS_REC_FLAG_MANDATORY) &&
			(entry->found == FALSE)) {
			decode_result = FALSE;
			break;
		}
	}

	g_slist_foreach(entries, (GFunc) g_free, NULL);
	g_slist_free(entries);

	return decode_result;
}

/* Decode WMT */
static gboolean cdma_sms_decode_wmt(struct simple_iter *iter,
					struct cdma_sms_bearer_data *bd)
{
	switch (bd->id.msg_type) {
	case CDMA_SMS_MSG_TYPE_RESERVED:
		return FALSE; /* Invalid */
	case CDMA_SMS_MSG_TYPE_DELIVER:
		/*
		 * WMT DELIVER, table 4.3.4-1 of C.S0015-B v2.0
		 * TODO: Not all optional subparameters supported.
		 */
		return decode_subparams(iter,
					&bd->subparam_bitmap,
					&bd->wmt_deliver.ud,
					CDMA_SMS_SUBPARAM_ID_USER_DATA,
					0,
					NULL);
		break;
	case CDMA_SMS_MSG_TYPE_SUBMIT:
	case CDMA_SMS_MSG_TYPE_CANCEL:
		return FALSE; /* Invalid for MT WMT */
	case CDMA_SMS_MSG_TYPE_DELIVER_ACK:
	case CDMA_SMS_MSG_TYPE_USER_ACK:
	case CDMA_SMS_MSG_TYPE_READ_ACK:
		return FALSE; /* TODO: Not supported yet */
	case CDMA_SMS_MSG_TYPE_DELIVER_REPORT:
	case CDMA_SMS_MSG_TYPE_SUBMIT_REPORT:
		return FALSE; /* Invalid for MT WMT */
	}

	return FALSE;
}

static gboolean p2p_decode_bearer_data(const guint8 *buf, guint8 len,
					enum cdma_sms_teleservice_id tele_id,
					struct cdma_sms_bearer_data *bd)
{
	struct simple_iter iter;

	simple_iter_init(&iter, buf, len);

	/* Message Identifier is mandatory, * Section 4 of C.S0015-B v2.0 */
	if (find_and_decode(&iter,
				CDMA_SMS_SUBPARAM_ID_MESSAGE_ID,
				cdma_sms_decode_message_id,
				&bd->id) != TRUE)
		return FALSE;

	set_bitmap(&bd->subparam_bitmap, CDMA_SMS_SUBPARAM_ID_MESSAGE_ID);

	simple_iter_init(&iter, buf, len);

	switch (tele_id) {
	case CDMA_SMS_TELESERVICE_ID_CMT91:
	case CDMA_SMS_TELESERVICE_ID_WPT:
		return FALSE; /* TODO */
	case CDMA_SMS_TELESERVICE_ID_WMT:
		return cdma_sms_decode_wmt(&iter, bd);
	case CDMA_SMS_TELESERVICE_ID_VMN:
	case CDMA_SMS_TELESERVICE_ID_WAP:
	case CDMA_SMS_TELESERVICE_ID_WEMT:
	case CDMA_SMS_TELESERVICE_ID_SCPT:
	case CDMA_SMS_TELESERVICE_ID_CATPT:
		return FALSE; /* TODO */
	}

	return FALSE;
}

/* Decode Bearer Data */
static gboolean cdma_sms_decode_bearer_data(const guint8 *buf, guint8 len,
								void *data)
{
	struct cdma_sms *msg = data;

	switch (msg->type) {
	case CDMA_SMS_TP_MSG_TYPE_P2P:
		return p2p_decode_bearer_data(buf, len,
						msg->p2p_msg.teleservice_id,
						&msg->p2p_msg.bd);
	case CDMA_SMS_TP_MSG_TYPE_BCAST:
		return FALSE; /* TODO */
	case CDMA_SMS_TP_MSG_TYPE_ACK:
		return FALSE; /* Invalid */
	}

	return FALSE;
}

static rec_handler param_handler_for_id(enum cdma_sms_param_id id,
						struct cdma_sms *incoming,
						void **data)
{
	if (incoming->type != CDMA_SMS_TP_MSG_TYPE_P2P)
		return NULL; /* TODO: Other types not supported yet */

	switch (id) {
	case CDMA_SMS_PARAM_ID_TELESERVICE_IDENTIFIER:
		*data = &incoming->p2p_msg.teleservice_id;
		return cdma_sms_decode_teleservice;
	case CDMA_SMS_PARAM_ID_SERVICE_CATEGORY:
		return NULL; /* TODO */
	case CDMA_SMS_PARAM_ID_ORIGINATING_ADDRESS:
		*data = &incoming->p2p_msg.oaddr;
		return cdma_sms_decode_addr;
	case CDMA_SMS_PARAM_ID_ORIGINATING_SUBADDRESS:
	case CDMA_SMS_PARAM_ID_DESTINATION_ADDRESS:
	case CDMA_SMS_PARAM_ID_DESTINATION_SUBADDRESS:
	case CDMA_SMS_PARAM_ID_BEARER_REPLY_OPTION:
	case CDMA_SMS_PARAM_ID_CAUSE_CODE:
		return NULL; /* TODO */
	case CDMA_SMS_PARAM_ID_BEARER_DATA:
		*data = incoming;
		return cdma_sms_decode_bearer_data;
	}

	return NULL;
}

static gboolean cdma_sms_p2p_decode(const guint8 *pdu, guint8 len,
					struct cdma_sms *incoming)
{
	struct simple_iter iter;

	simple_iter_init(&iter, pdu, len);

	/*
	 * Teleservice Identifier is mandatory,
	 * Table 3.4.2.1-1 of C.S0015-B v2.0
	 */
	if (find_and_decode(&iter,
				CDMA_SMS_PARAM_ID_TELESERVICE_IDENTIFIER,
				cdma_sms_decode_teleservice,
				&incoming->p2p_msg.teleservice_id) != TRUE)
		return FALSE;

	set_bitmap(&incoming->p2p_msg.param_bitmap,
			CDMA_SMS_PARAM_ID_TELESERVICE_IDENTIFIER);

	simple_iter_init(&iter, pdu, len);

	while (simple_iter_next(&iter) == TRUE) {
		rec_handler handler;
		enum cdma_sms_param_id rec_id;
		guint8 rec_len;
		const guint8 *rec_buf;
		void *uninitialized_var(dataobj);

		rec_id = simple_iter_get_id(&iter);
		if (rec_id == CDMA_SMS_PARAM_ID_TELESERVICE_IDENTIFIER)
			continue;

		rec_len = simple_iter_get_length(&iter);
		rec_buf = simple_iter_get_data(&iter);

		handler = param_handler_for_id(rec_id, incoming, &dataobj);
		if (handler != NULL) {
			if (handler(rec_buf, rec_len, dataobj) == FALSE)
				return FALSE;

			set_bitmap(&incoming->p2p_msg.param_bitmap, rec_id);
		}
	}

	/*
	 * Originating Address is mandatory field,
	 * Table 3.4.2.1-1 of C.S0015-B v2.0
	 */
	if ((incoming->p2p_msg.param_bitmap &
			(1 << CDMA_SMS_PARAM_ID_ORIGINATING_ADDRESS)) == 0)
		return FALSE;

	return TRUE;
}

gboolean cdma_sms_decode(const guint8 *pdu, guint8 len,
				struct cdma_sms *incoming)
{
	incoming->type = bit_field_unpack(pdu, 0, 8);
	pdu += 1;
	len -= 1;

	switch (incoming->type) {
	case CDMA_SMS_TP_MSG_TYPE_P2P:
		return cdma_sms_p2p_decode(pdu, len, incoming);
	case CDMA_SMS_TP_MSG_TYPE_BCAST:
	case CDMA_SMS_TP_MSG_TYPE_ACK:
		/* TODO: Not supported yet */
		return FALSE;
	}

	return FALSE;
}
