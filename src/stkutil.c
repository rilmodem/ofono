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
#include <stdint.h>

#include <glib.h>

#include <ofono/types.h>
#include "smsutil.h"
#include "stkutil.h"
#include "simutil.h"
#include "util.h"

enum stk_data_object_flag {
	DATAOBJ_FLAG_MANDATORY =	1,
	DATAOBJ_FLAG_MINIMUM =		2,
	DATAOBJ_FLAG_CR =		4,
	DATAOBJ_FLAG_LIST =		8,
};

struct stk_file_iter {
	const unsigned char *start;
	unsigned int pos;
	unsigned int max;
	unsigned char len;
	const unsigned char *file;
};

struct stk_tlv_builder {
	struct comprehension_tlv_builder ctlv;
	unsigned char *value;
	unsigned int len;
	unsigned int max_len;
};

typedef gboolean (*dataobj_handler)(struct comprehension_tlv_iter *, void *);
typedef gboolean (*dataobj_writer)(struct stk_tlv_builder *,
					const void *, gboolean);

/*
 * Defined in TS 102.223 Section 8.13
 * The type of gsm sms can be SMS-COMMAND AND SMS-SUBMIT. According to 23.040,
 * the maximum length is 164 bytes. But for SMS-SUBMIT, sms may be packed by
 * ME. Thus the maximum length of messsage could be 160 bytes, instead of 140
 * bytes. So the total maximum length could be 184 bytes. Refer TS 31.111,
 * section 6.4.10 for details.
 */
struct gsm_sms_tpdu {
	unsigned int len;
	unsigned char tpdu[184];
};

#define CHECK_TEXT_AND_ICON(text, icon_id)			\
	if (status != STK_PARSE_RESULT_OK)			\
		return status;					\
								\
	if ((text == NULL || text[0] == '\0') && icon_id != 0)	\
		status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;	\

static char *decode_text(unsigned char dcs, int len, const unsigned char *data)
{
	char *utf8;
	enum sms_charset charset;

	if (sms_dcs_decode(dcs, NULL, &charset, NULL, NULL) == FALSE)
		return NULL;

	switch (charset) {
	case SMS_CHARSET_7BIT:
	{
		long written;
		unsigned long max_to_unpack = len * 8 / 7;
		unsigned char *unpacked = unpack_7bit(data, len, 0, FALSE,
							max_to_unpack,
							&written, 0);
		if (unpacked == NULL)
			return NULL;

		utf8 = convert_gsm_to_utf8(unpacked, written,
						NULL, NULL, 0);
		g_free(unpacked);
		break;
	}
	case SMS_CHARSET_8BIT:
		utf8 = convert_gsm_to_utf8(data, len, NULL, NULL, 0);
		break;
	case SMS_CHARSET_UCS2:
		utf8 = g_convert((const gchar *) data, len,
					"UTF-8//TRANSLIT", "UCS-2BE",
					NULL, NULL, NULL);
		break;
	default:
		utf8 = NULL;
	}

	return utf8;
}

/* For data object only to indicate its existence */
static gboolean parse_dataobj_common_bool(struct comprehension_tlv_iter *iter,
						gboolean *out)
{
	if (comprehension_tlv_iter_get_length(iter) != 0)
		return FALSE;

	*out = TRUE;

	return TRUE;
}

/* For data object that only has one byte */
static gboolean parse_dataobj_common_byte(struct comprehension_tlv_iter *iter,
						unsigned char *out)
{
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	*out = data[0];

	return TRUE;
}

/* For data object that only has text terminated by '\0' */
static gboolean parse_dataobj_common_text(struct comprehension_tlv_iter *iter,
						char **text)
{
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	*text = g_try_malloc(len + 1);
	if (*text == NULL)
		return FALSE;

	memcpy(*text, data, len);
	(*text)[len] = '\0';

	return TRUE;
}

/* For data object that only has a byte array with undetermined length */
static gboolean parse_dataobj_common_byte_array(
			struct comprehension_tlv_iter *iter,
			struct stk_common_byte_array *array)
{
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	array->len = len;

	array->array = g_try_malloc(len);
	if (array->array == NULL)
		return FALSE;

	memcpy(array->array, data, len);

	return TRUE;
}

static void stk_file_iter_init(struct stk_file_iter *iter,
				const unsigned char *start, unsigned int len)
{
	iter->start = start;
	iter->max = len;
	iter->pos = 0;
}

static gboolean stk_file_iter_next(struct stk_file_iter *iter)
{
	unsigned int pos = iter->pos;
	const unsigned int max = iter->max;
	const unsigned char *start = iter->start;
	unsigned int i;
	unsigned char last_type;

	/* SIM EFs always start with ROOT MF, 0x3f */
	if (start[iter->pos] != 0x3f)
		return FALSE;

	if (pos + 2 >= max)
		return FALSE;

	last_type = 0x3f;

	for (i = pos + 2; i < max; i += 2) {
		/*
		 * Check the validity of file type.
		 * According to TS 11.11, each file id contains of two bytes,
		 * in which the first byte is the type of file. For GSM is:
		 * 0x3f: master file
		 * 0x7f: 1st level dedicated file
		 * 0x5f: 2nd level dedicated file
		 * 0x2f: elementary file under the master file
		 * 0x6f: elementary file under 1st level dedicated file
		 * 0x4f: elementary file under 2nd level dedicated file
		 */
		switch (start[i]) {
		case 0x2f:
			if (last_type != 0x3f)
				return FALSE;
			break;
		case 0x6f:
			if (last_type != 0x7f)
				return FALSE;
			break;
		case 0x4f:
			if (last_type != 0x5f)
				return FALSE;
			break;
		case 0x7f:
			if (last_type != 0x3f)
				return FALSE;
			break;
		case 0x5f:
			if (last_type != 0x7f)
				return FALSE;
			break;
		default:
			return FALSE;
		}

		if ((start[i] == 0x2f) || (start[i] == 0x6f) ||
						(start[i] == 0x4f)) {
			if (i + 1 >= max)
				return FALSE;

			iter->file = start + pos;
			iter->len = i - pos + 2;
			iter->pos = i + 2;

			return TRUE;
		}

		last_type = start[i];
	}

	return FALSE;
}

/* Defined in TS 102.223 Section 8.1 */
static gboolean parse_dataobj_address(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_address *addr = user;
	const unsigned char *data;
	unsigned int len;
	char *number;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	number = g_try_malloc(len * 2 - 1);
	if (number == NULL)
		return FALSE;

	addr->ton_npi = data[0];
	addr->number = number;
	sim_extract_bcd_number(data + 1, len - 1, addr->number);

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

	len = comprehension_tlv_iter_get_length(iter);
	if (len == 0) {
		*alpha_id = NULL;
		return TRUE;
	}

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

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1)
		return FALSE;

	if (len > sizeof(subaddr->subaddr))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	subaddr->len = len;
	memcpy(subaddr->subaddr, data, len);

	subaddr->has_subaddr = TRUE;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.4 */
static gboolean parse_dataobj_ccp(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_ccp *ccp = user;
	const unsigned char *data;
	unsigned int len;

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

/* Defined in TS 31.111 Section 8.5 */
static gboolean parse_dataobj_cbs_page(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_cbs_page *cp = user;
	const unsigned char *data;
	unsigned int len;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1)
		return FALSE;

	if (len > sizeof(cp->page))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	cp->len = len;
	memcpy(cp->page, data, len);

	return TRUE;
}

/* Described in TS 102.223 Section 8.8 */
static gboolean parse_dataobj_duration(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_duration *duration = user;
	const unsigned char *data;

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

	len = comprehension_tlv_iter_get_length(iter);

	if (len == 0)
		return TRUE;

	if (len == 1)
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

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 1 || len > sizeof(tpdu->tpdu))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	tpdu->len = len;
	memcpy(tpdu->tpdu, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.14 */
static gboolean parse_dataobj_ss(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_ss *ss = user;
	const unsigned char *data;
	unsigned int len;
	char *s;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	s = g_try_malloc(len * 2 - 1);
	if (s == NULL)
		return FALSE;

	ss->ton_npi = data[0];
	ss->ss = s;
	sim_extract_bcd_number(data + 1, len - 1, ss->ss);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.15 */
static gboolean parse_dataobj_text(struct comprehension_tlv_iter *iter,
					void *user)
{
	char **text = user;
	unsigned int len = comprehension_tlv_iter_get_length(iter);
	const unsigned char *data;
	char *utf8;

	if (len <= 1) {
		*text = g_try_malloc0(1);
		return TRUE;
	}

	data = comprehension_tlv_iter_get_data(iter);

	utf8 = decode_text(data[0], len - 1, data + 1);

	if (utf8 == NULL)
		return FALSE;

	*text = utf8;
	return TRUE;
}

/* Defined in TS 102.223 Section 8.16 */
static gboolean parse_dataobj_tone(struct comprehension_tlv_iter *iter,
					void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.17 */
static gboolean parse_dataobj_ussd(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_ussd_string *us = user;
	unsigned int len = comprehension_tlv_iter_get_length(iter);
	const unsigned char *data = comprehension_tlv_iter_get_data(iter);

	if (len <= 1 || len > 161)
		return FALSE;

	us->dcs = data[0];
	us->len = len - 1;
	memcpy(us->string, data + 1, us->len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.18 */
static gboolean parse_dataobj_file_list(struct comprehension_tlv_iter *iter,
					void *user)
{
	GSList **fl = user;
	const unsigned char *data;
	unsigned int len;
	struct stk_file *sf;
	struct stk_file_iter sf_iter;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 5)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	stk_file_iter_init(&sf_iter, data + 1, len - 1);

	while (stk_file_iter_next(&sf_iter)) {
		sf = g_try_new0(struct stk_file, 1);
		if (sf == NULL)
			goto error;

		sf->len = sf_iter.len;
		memcpy(sf->file, sf_iter.file, sf_iter.len);
		*fl = g_slist_prepend(*fl, sf);
	}

	if (sf_iter.pos != sf_iter.max)
		goto error;

	*fl = g_slist_reverse(*fl);
	return TRUE;

error:
	g_slist_foreach(*fl, (GFunc) g_free, NULL);
	g_slist_free(*fl);
	return FALSE;
}

/* Defined in TS 102.223 Section 8.19 */
static gboolean parse_dataobj_location_info(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_location_info *li = user;
	const unsigned char *data;
	unsigned int len;

	len = comprehension_tlv_iter_get_length(iter);
	if ((len != 5) && (len != 7) && (len != 9))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	sim_parse_mcc_mnc(data, li->mcc, li->mnc);
	li->lac_tac = (data[3] << 8) + data[4];

	if (len >= 7) {
		li->has_ci = TRUE;
		li->ci = (data[5] << 8) + data[6];
	}

	if (len == 9) {
		li->has_ext_ci = TRUE;
		li->ext_ci = (data[7] << 8) + data[8];
	}

	return TRUE;
}

/*
 * Defined in TS 102.223 Section 8.20.
 *
 * According to 3GPP TS 24.008, Section 10.5.1.4, IMEI is composed of
 * 15 digits and totally 8 bytes are used to represent it.
 *
 * Bits 1-3 of first byte represent the type of identity, and they
 * are 0 1 0 separately for IMEI. Bit 4 of first byte is the odd/even
 * indication, and it's 1 to indicate IMEI has odd number of digits (15).
 * The rest bytes are coded using BCD coding.
 *
 * For example, if the IMEI is "123456789012345", then it's coded as
 * "1A 32 54 76 98 10 32 54".
 */
static gboolean parse_dataobj_imei(struct comprehension_tlv_iter *iter,
					void *user)
{
	char *imei = user;
	const unsigned char *data;
	unsigned int len;
	static const char digit_lut[] = "0123456789*#abc\0";

	len = comprehension_tlv_iter_get_length(iter);
	if (len != 8)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if ((data[0] & 0x0f) != 0x0a)
		return FALSE;

	/* Assume imei is at least 16 bytes long (15 for imei + null) */
	imei[0] = digit_lut[(data[0] & 0xf0) >> 4];
	extract_bcd_number(data + 1, 7, imei + 1);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.21 */
static gboolean parse_dataobj_help_request(struct comprehension_tlv_iter *iter,
						void *user)
{
	gboolean *ret = user;
	return parse_dataobj_common_bool(iter, ret);
}

/* Defined in TS 102.223 Section 8.22 */
static gboolean parse_dataobj_network_measurement_results(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *nmr = user;
	const unsigned char *data;
	unsigned int len;

	len = comprehension_tlv_iter_get_length(iter);
	if (len != 0x10)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* Assume network measurement result is 16 bytes long */
	memcpy(nmr, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.23 */
static gboolean parse_dataobj_default_text(struct comprehension_tlv_iter *iter,
						void *user)
{
	char **text = user;
	unsigned int len = comprehension_tlv_iter_get_length(iter);
	const unsigned char *data = comprehension_tlv_iter_get_data(iter);
	char *utf8;

	/* DCS followed by some text, cannot be 1 */
	if (len <= 1)
		return FALSE;

	utf8 = decode_text(data[0], len - 1, data + 1);

	if (utf8 == NULL)
		return FALSE;

	*text = utf8;
	return TRUE;
}

/* Defined in TS 102.223 Section 8.24 */
static gboolean parse_dataobj_items_next_action_indicator(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_items_next_action_indicator *inai = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len < 1) || (len > sizeof(inai->list)))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	inai->len = len;
	memcpy(inai->list, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.25 */
static gboolean parse_dataobj_event_list(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_event_list *el = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len == 0)
		return TRUE;

	if (len > sizeof(el->list))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	el->len = len;
	memcpy(el->list, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.26 */
static gboolean parse_dataobj_cause(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_cause *cause = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len == 1) || (len > sizeof(cause->cause)))
		return FALSE;

	cause->has_cause = TRUE;

	if (len == 0)
		return TRUE;

	data = comprehension_tlv_iter_get_data(iter);
	cause->len = len;
	memcpy(cause->cause, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.27 */
static gboolean parse_dataobj_location_status(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;

	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.28 */
static gboolean parse_dataobj_transaction_id(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_transaction_id *ti = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len < 1) || (len > sizeof(ti->list)))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	ti->len = len;
	memcpy(ti->list, data, len);

	return TRUE;
}

/* Defined in TS 31.111 Section 8.29 */
static gboolean parse_dataobj_bcch_channel_list(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_bcch_channel_list *bcl = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);
	unsigned int i;

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	bcl->num = len * 8 / 10;

	for (i = 0; i < bcl->num; i++) {
		unsigned int index = i * 10 / 8;
		unsigned int occupied = i * 10 % 8;

		bcl->channels[i] = (data[index] << (2 + occupied)) +
					(data[index + 1] >> (6 - occupied));
	}

	bcl->has_list = TRUE;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.30 */
static gboolean parse_dataobj_call_control_requested_action(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_common_byte_array *array = user;

	return parse_dataobj_common_byte_array(iter, array);
}

/* Defined in TS 102.223 Section 8.31 */
static gboolean parse_dataobj_icon_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_icon_id *id = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	id->qualifier = data[0];
	id->id = data[1];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.32 */
static gboolean parse_dataobj_item_icon_id_list(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_item_icon_id_list *iiil = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len < 2) || (len > 127))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	iiil->qualifier = data[0];
	iiil->len = len - 1;
	memcpy(iiil->list, data + 1, iiil->len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.33 */
static gboolean parse_dataobj_card_reader_status(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;

	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.34 */
static gboolean parse_dataobj_card_atr(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_card_atr *ca = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len < 1) || (len > sizeof(ca->atr)))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	ca->len = len;
	memcpy(ca->atr, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.35 */
static gboolean parse_dataobj_c_apdu(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_c_apdu *ca = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);
	unsigned int pos;

	if ((len < 4) || (len > 241))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	ca->cla = data[0];
	ca->ins = data[1];
	ca->p1 = data[2];
	ca->p2 = data[3];

	pos = 4;

	/*
	 * lc is 0 has the same meaning as lc is absent. But le is 0 means
	 * the maximum number of bytes expected in the response data field
	 * is 256. So we need to rely on has_le to know if it presents.
	 */
	if (len > 5) {
		ca->lc = data[4];
		if (ca->lc > sizeof(ca->data))
			return FALSE;

		pos += ca->lc + 1;

		if (len - pos > 1)
			return FALSE;

		memcpy(ca->data, data+5, ca->lc);
	}

	if (len - pos > 0) {
		ca->le = data[len - 1];
		ca->has_le = TRUE;
	}

	return TRUE;
}

/* Defined in TS 102.223 Section 8.36 */
static gboolean parse_dataobj_r_apdu(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_r_apdu *ra = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len < 2) || (len > 239))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	ra->sw1 = data[len-2];
	ra->sw2 = data[len-1];

	if (len > 2) {
		ra->len = len - 2;
		memcpy(ra->data, data, ra->len);
	} else
		ra->len = 0;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.37 */
static gboolean parse_dataobj_timer_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	unsigned char *byte = user;

	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.38 */
static gboolean parse_dataobj_timer_value(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_timer_value *tv = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 3)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	tv->hour = sms_decode_semi_octet(data[0]);
	tv->minute = sms_decode_semi_octet(data[1]);
	tv->second = sms_decode_semi_octet(data[2]);
	tv->has_value = TRUE;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.39 */
static gboolean parse_dataobj_datetime_timezone(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct sms_scts *scts = user;
	const unsigned char *data;
	int offset = 0;

	if (comprehension_tlv_iter_get_length(iter) != 7)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	sms_decode_scts(data, 7, &offset, scts);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.40 */
static gboolean parse_dataobj_at_command(struct comprehension_tlv_iter *iter,
						void *user)
{
	char **command = user;
	return parse_dataobj_common_text(iter, command);
}

/* Defined in TS 102.223 Section 8.41 */
static gboolean parse_dataobj_at_response(struct comprehension_tlv_iter *iter,
						void *user)
{
	char **response = user;
	return parse_dataobj_common_text(iter, response);
}

/* Defined in TS 102.223 Section 8.42 */
static gboolean parse_dataobj_bc_repeat_indicator(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_bc_repeat *bc_repeat = user;

	if (parse_dataobj_common_byte(iter, &bc_repeat->value) != TRUE)
		return FALSE;

	bc_repeat->has_bc_repeat = TRUE;
	return TRUE;
}

/* Defined in 102.223 Section 8.43 */
static gboolean parse_dataobj_imm_resp(struct comprehension_tlv_iter *iter,
					void *user)
{
	gboolean *ret = user;
	return parse_dataobj_common_bool(iter, ret);
}

/* Defined in 102.223 Section 8.44 */
static gboolean parse_dataobj_dtmf_string(struct comprehension_tlv_iter *iter,
						void *user)
{
	char **dtmf = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	*dtmf = g_try_malloc(len * 2 + 1);
	if (*dtmf == NULL)
		return FALSE;

	sim_extract_bcd_number(data, len, *dtmf);

	return TRUE;
}

/* Defined in 102.223 Section 8.45 */
static gboolean parse_dataobj_language(struct comprehension_tlv_iter *iter,
					void *user)
{
	char *lang = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/*
	 * This is a 2 character pair as defined in ISO 639, coded using
	 * GSM default 7 bit alphabet with bit 8 set to 0.  Since the english
	 * letters have the same mapping in GSM as ASCII, no conversion
	 * is required here
	 */
	memcpy(lang, data, len);
	lang[len] = '\0';

	return TRUE;
}

/* Defined in 31.111 Section 8.46 */
static gboolean parse_dataobj_timing_advance(
			struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_timing_advance *ta = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	ta->has_value = TRUE;
	ta->status = data[0];
	ta->advance = data[1];

	return TRUE;
}

/* Defined in 102.223 Section 8.47 */
static gboolean parse_dataobj_browser_id(struct comprehension_tlv_iter *iter,
						void *user)
{
	unsigned char *byte = user;

	if (parse_dataobj_common_byte(iter, byte) == FALSE || *byte > 4)
		return FALSE;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.48 */
static gboolean parse_dataobj_url(struct comprehension_tlv_iter *iter,
					void *user)
{
	char **url = user;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len == 0) {
		*url = NULL;
		return TRUE;
	}

	return parse_dataobj_common_text(iter, url);
}

/* Defined in TS 102.223 Section 8.49 */
static gboolean parse_dataobj_bearer(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_common_byte_array *array = user;
	return parse_dataobj_common_byte_array(iter, array);
}

/* Defined in TS 102.223 Section 8.50 */
static gboolean parse_dataobj_provisioning_file_reference(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_file *f = user;
	const unsigned char *data;
	struct stk_file_iter sf_iter;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len < 1) || (len > 8))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	stk_file_iter_init(&sf_iter, data, len);
	stk_file_iter_next(&sf_iter);

	if (sf_iter.pos != sf_iter.max)
		return FALSE;

	f->len = len;
	memcpy(f->file, data, len);

	return TRUE;
}

/* Defined in 102.223 Section 8.51 */
static gboolean parse_dataobj_browser_termination_cause(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.52 */
static gboolean parse_dataobj_bearer_description(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_bearer_description *bd = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	bd->type = data[0];

	/* Parse only the packet data service bearer parameters */
	if (bd->type != STK_BEARER_TYPE_GPRS_UTRAN)
		return FALSE;

	if (len < 7)
		return FALSE;

	bd->gprs.precedence = data[1];
	bd->gprs.delay = data[2];
	bd->gprs.reliability = data[3];
	bd->gprs.peak = data[4];
	bd->gprs.mean = data[5];
	bd->gprs.pdp_type = data[6];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.53 */
static gboolean parse_dataobj_channel_data(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_common_byte_array *array = user;
	return parse_dataobj_common_byte_array(iter, array);
}

/* Defined in TS 102.223 Section 8.54 */
static gboolean parse_dataobj_channel_data_length(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.55 */
static gboolean parse_dataobj_buffer_size(struct comprehension_tlv_iter *iter,
						void *user)
{
	unsigned short *size = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	*size = (data[0] << 8) + data[1];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.56 */
static gboolean parse_dataobj_channel_status(
			struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *status = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* Assume channel status is 2 bytes long */
	memcpy(status, data, 2);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.57 */
static gboolean parse_dataobj_card_reader_id(
			struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_card_reader_id *cr_id = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	cr_id->len = len;
	memcpy(cr_id->id, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.58 */
static gboolean parse_dataobj_other_address(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_other_address *oa = user;
	const unsigned char *data;
	unsigned char len = comprehension_tlv_iter_get_length(iter);

	if (len == 0) {
		oa->type = STK_ADDRESS_AUTO;
		return TRUE;
	}

	if ((len != 5) && (len != 17))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] != STK_ADDRESS_IPV4 && data[0] != STK_ADDRESS_IPV6)
		return FALSE;

	oa->type = data[0];

	if (oa->type == STK_ADDRESS_IPV4)
		memcpy(&oa->addr.ipv4, data + 1, 4);
	else
		memcpy(&oa->addr.ipv6, data + 1, 16);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.59 */
static gboolean parse_dataobj_uicc_te_interface(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_uicc_te_interface *uti = user;
	const unsigned char *data;
	unsigned char len = comprehension_tlv_iter_get_length(iter);

	if (len != 3)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	uti->protocol = data[0];
	uti->port = (data[1] << 8) + data[2];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.60 */
static gboolean parse_dataobj_aid(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_aid *aid = user;
	const unsigned char *data;
	unsigned char len = comprehension_tlv_iter_get_length(iter);

	if ((len > 16) || (len < 12))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	aid->len = len;
	memcpy(aid->aid, data, len);

	return TRUE;
}

/*
 * Defined in TS 102.223 Section 8.61. According to it, the technology field
 * can have at most 127 bytes. However, all the defined values are only 1 byte,
 * so we just use 1 byte to represent it.
 */
static gboolean parse_dataobj_access_technology(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.62 */
static gboolean parse_dataobj_display_parameters(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_display_parameters *dp = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 3)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	dp->height = data[0];
	dp->width = data[1];
	dp->effects = data[2];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.63 */
static gboolean parse_dataobj_service_record(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_service_record *sr = user;
	const unsigned char *data;
	unsigned int len;

	len = comprehension_tlv_iter_get_length(iter);
	if (len < 3)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	sr->tech_id = data[0];
	sr->serv_id = data[1];
	sr->len = len - 2;

	sr->serv_rec = g_try_malloc(sr->len);
	if (sr->serv_rec == NULL)
		return FALSE;

	memcpy(sr->serv_rec, data + 2, sr->len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.64 */
static gboolean parse_dataobj_device_filter(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_device_filter *df = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* According to TS 102.223, everything except BT & IRDA is RFU */
	if (data[0] != STK_TECHNOLOGY_BLUETOOTH &&
			data[0] != STK_TECHNOLOGY_IRDA)
		return FALSE;

	df->tech_id = data[0];
	df->len = len - 1;

	df->dev_filter = g_try_malloc(df->len);
	if (df->dev_filter == NULL)
		return FALSE;

	memcpy(df->dev_filter, data + 1, df->len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.65 */
static gboolean parse_dataobj_service_search(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_service_search *ss = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* According to TS 102.223, everything except BT & IRDA is RFU */
	if (data[0] != STK_TECHNOLOGY_BLUETOOTH &&
			data[0] != STK_TECHNOLOGY_IRDA)
		return FALSE;

	ss->tech_id = data[0];
	ss->len = len - 1;

	ss->ser_search = g_try_malloc(ss->len);
	if (ss->ser_search == NULL)
		return FALSE;

	memcpy(ss->ser_search, data + 1, ss->len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.66 */
static gboolean parse_dataobj_attribute_info(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_attribute_info *ai = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* According to TS 102.223, everything except BT & IRDA is RFU */
	if (data[0] != STK_TECHNOLOGY_BLUETOOTH &&
			data[0] != STK_TECHNOLOGY_IRDA)
		return FALSE;

	ai->tech_id = data[0];
	ai->len = len - 1;

	ai->attr_info = g_try_malloc(ai->len);
	if (ai->attr_info == NULL)
		return FALSE;

	memcpy(ai->attr_info, data + 1, ai->len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.67 */
static gboolean parse_dataobj_service_availability(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_common_byte_array *array = user;
	return parse_dataobj_common_byte_array(iter, array);
}

/* Defined in TS 102.223 Section 8.68 */
static gboolean parse_dataobj_remote_entity_address(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_remote_entity_address *rea = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	data = comprehension_tlv_iter_get_data(iter);

	switch (data[0]) {
	case 0x00:
		if (len != 7)
			return FALSE;
		break;
	case 0x01:
		if (len != 5)
			return FALSE;
		break;
	default:
		return FALSE;
	}

	rea->has_address = TRUE;
	rea->coding_type = data[0];
	memcpy(&rea->addr, data + 1, len - 1);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.69 */
static gboolean parse_dataobj_esn(struct comprehension_tlv_iter *iter,
					void *user)
{
	unsigned char *esn = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len != 4)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* Assume esn is 4 bytes long */
	memcpy(esn, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.70 */
static gboolean parse_dataobj_network_access_name(
					struct comprehension_tlv_iter *iter,
					void *user)
{
	char **apn = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);
	unsigned char label_size;
	unsigned char offset = 0;
	char decoded_apn[100];

	if (len == 0 || len > 100)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/*
	 * As specified in TS 23 003 Section 9
	 * The APN consists of one or more labels. Each label is coded as
	 * a one octet length field followed by that number of octets coded
	 * as 8 bit ASCII characters
	 */
	while (len) {
		label_size = *data;

		if (label_size == 0 || label_size > (len - 1))
			return FALSE;

		memcpy(decoded_apn + offset, data + 1, label_size);

		data += label_size + 1;
		offset += label_size;
		len -= label_size + 1;

		if (len)
			decoded_apn[offset++] = '.';
	}

	decoded_apn[offset] = '\0';
	*apn = g_strdup(decoded_apn);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.71 */
static gboolean parse_dataobj_cdma_sms_tpdu(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_common_byte_array *array = user;
	return parse_dataobj_common_byte_array(iter, array);
}

/* Defined in TS 102.223 Section 8.72 */
static gboolean parse_dataobj_text_attr(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_text_attribute *attr = user;
	const unsigned char *data;
	unsigned int len;

	len = comprehension_tlv_iter_get_length(iter);

	if (len > sizeof(attr->attributes))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	memcpy(attr->attributes, data, len);
	attr->len = len;

	return TRUE;
}

/* Defined in TS 31.111 Section 8.72 */
static gboolean parse_dataobj_pdp_act_par(
			struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_pdp_act_par *pcap = user;
	const unsigned char *data;
	unsigned int len;

	len = comprehension_tlv_iter_get_length(iter);

	if (len > sizeof(pcap->par))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	memcpy(pcap->par, data, len);
	pcap->len = len;

	return TRUE;
}

/* Defined in TS 102.223 Section 8.73 */
static gboolean parse_dataobj_item_text_attribute_list(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_item_text_attribute_list *ital = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if ((len > sizeof(ital->list)) || (len % 4 != 0))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	memcpy(ital->list, data, len);
	ital->len = len;

	return TRUE;
}

/* Defined in TS 31.111 Section 8.73 */
static gboolean parse_dataobj_utran_meas_qualifier(
			struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
}

/*
 * Defined in TS 102.223 Section 8.74.
 *
 * According to 3GPP TS 24.008, Section 10.5.1.4, IMEISV is composed of
 * 16 digits and totally 9 bytes are used to represent it.
 *
 * Bits 1-3 of first byte represent the type of identity, and they
 * are 0 1 1 separately for IMEISV. Bit 4 of first byte is the odd/even
 * indication, and it's 0 to indicate IMEISV has odd number of digits (16).
 * The rest bytes are coded using BCD coding.
 *
 * For example, if the IMEISV is "1234567890123456", then it's coded as
 * "13 32 54 76 98 10 32 54 F6".
 */
static gboolean parse_dataobj_imeisv(struct comprehension_tlv_iter *iter,
					void *user)
{
	char *imeisv = user;
	const unsigned char *data;
	unsigned int len;
	static const char digit_lut[] = "0123456789*#abc\0";

	len = comprehension_tlv_iter_get_length(iter);
	if (len != 9)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if ((data[0] & 0x0f) != 0x03)
		return FALSE;

	if (data[8] >> 4 != 0x0f)
		return FALSE;

	/* Assume imeisv is at least 17 bytes long (16 for imeisv + null) */
	imeisv[0] = digit_lut[data[0] >> 4];
	extract_bcd_number(data + 1, 7, imeisv + 1);
	imeisv[15] = digit_lut[data[8] & 0x0f];
	imeisv[16] = '\0';

	return TRUE;
}

/* Defined in TS 102.223 Section 8.75 */
static gboolean parse_dataobj_network_search_mode(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.76 */
static gboolean parse_dataobj_battery_state(struct comprehension_tlv_iter *iter,
						void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
}

/* Defined in TS 102.223 Section 8.77 */
static gboolean parse_dataobj_browsing_status(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_common_byte_array *array = user;
	return parse_dataobj_common_byte_array(iter, array);
}

/* Defined in TS 102.223 Section 8.78 */
static gboolean parse_dataobj_frame_layout(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_frame_layout *fl = user;
	const unsigned char *data;
	unsigned char len = comprehension_tlv_iter_get_length(iter);

	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] != STK_LAYOUT_HORIZONTAL &&
			data[0] != STK_LAYOUT_VERTICAL)
		return FALSE;

	fl->layout = data[0];
	fl->len = len - 1;
	memcpy(fl->size, data + 1, fl->len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.79 */
static gboolean parse_dataobj_frames_info(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_frames_info *fi = user;
	const unsigned char *data;
	unsigned char len = comprehension_tlv_iter_get_length(iter);
	unsigned int i;

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] > 0x0f)
		return FALSE;

	if ((len == 1 && data[0] != 0) || (len > 1 && data[0] == 0))
		return FALSE;

	if (len % 2 == 0)
		return FALSE;

	if (len == 1)
		return TRUE;

	fi->id = data[0];
	fi->len = (len - 1) / 2;
	for (i = 0; i < len; i++) {
		fi->list[i].height = data[i * 2 + 1] & 0x1f;
		fi->list[i].width = data[i * 2 + 2] & 0x7f;
	}

	return TRUE;
}

/* Defined in TS 102.223 Section 8.80 */
static gboolean parse_dataobj_frame_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_frame_id *fi = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] >= 0x10)
		return FALSE;

	fi->has_id = TRUE;
	fi->id = data[0];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.81 */
static gboolean parse_dataobj_meid(struct comprehension_tlv_iter *iter,
					void *user)
{
	unsigned char *meid = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 8)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	/* Assume meid is 8 bytes long */
	memcpy(meid, data, 8);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.82 */
static gboolean parse_dataobj_mms_reference(struct comprehension_tlv_iter *iter,
						void *user)
{
	struct stk_mms_reference *mr = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	mr->len = len;
	memcpy(mr->ref, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.83 */
static gboolean parse_dataobj_mms_id(struct comprehension_tlv_iter *iter,
					void *user)
{
	struct stk_mms_id *mi = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	mi->len = len;
	memcpy(mi->id, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.84 */
static gboolean parse_dataobj_mms_transfer_status(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_mms_transfer_status *mts = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	mts->len = len;
	memcpy(mts->status, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.85 */
static gboolean parse_dataobj_mms_content_id(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_mms_content_id *mci = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	mci->len = len;
	memcpy(mci->id, data, len);

	return TRUE;
}

/* Defined in TS 102.223 Section 8.86 */
static gboolean parse_dataobj_mms_notification(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_common_byte_array *array = user;
	return parse_dataobj_common_byte_array(iter, array);
}

/* Defined in TS 102.223 Section 8.87 */
static gboolean parse_dataobj_last_envelope(struct comprehension_tlv_iter *iter,
						void *user)
{
	gboolean *ret = user;
	return parse_dataobj_common_bool(iter, ret);
}

/* Defined in TS 102.223 Section 8.88 */
static gboolean parse_dataobj_registry_application_data(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_registry_application_data *rad = user;
	const unsigned char *data;
	char *utf8;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 5)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	utf8 = decode_text(data[2], len - 4, data + 4);

	if (utf8 == NULL)
		return FALSE;

	rad->name = utf8;
	rad->port = (data[0] << 8) + data[1];
	rad->type = data[3];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.89 */
static gboolean parse_dataobj_activate_descriptor(
		struct comprehension_tlv_iter *iter, void *user)
{
	unsigned char *byte = user;
	const unsigned char *data;

	if (comprehension_tlv_iter_get_length(iter) != 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] != 0x01)
		return FALSE;

	*byte = data[0];

	return TRUE;
}

/* Defined in TS 102.223 Section 8.90 */
static gboolean parse_dataobj_broadcast_network_info(
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_broadcast_network_information *bni = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len < 2)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] > 0x03)
		return FALSE;

	bni->tech = data[0];
	bni->len = len - 1;
	memcpy(bni->loc_info, data + 1, bni->len);

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
	case STK_DATA_OBJECT_TYPE_CBS_PAGE:
		return parse_dataobj_cbs_page;
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
	case STK_DATA_OBJECT_TYPE_SS_STRING:
		return parse_dataobj_ss;
	case STK_DATA_OBJECT_TYPE_TEXT:
		return parse_dataobj_text;
	case STK_DATA_OBJECT_TYPE_TONE:
		return parse_dataobj_tone;
	case STK_DATA_OBJECT_TYPE_USSD_STRING:
		return parse_dataobj_ussd;
	case STK_DATA_OBJECT_TYPE_FILE_LIST:
		return parse_dataobj_file_list;
	case STK_DATA_OBJECT_TYPE_LOCATION_INFO:
		return parse_dataobj_location_info;
	case STK_DATA_OBJECT_TYPE_IMEI:
		return parse_dataobj_imei;
	case STK_DATA_OBJECT_TYPE_HELP_REQUEST:
		return parse_dataobj_help_request;
	case STK_DATA_OBJECT_TYPE_NETWORK_MEASUREMENT_RESULTS:
		return parse_dataobj_network_measurement_results;
	case STK_DATA_OBJECT_TYPE_DEFAULT_TEXT:
		return parse_dataobj_default_text;
	case STK_DATA_OBJECT_TYPE_ITEMS_NEXT_ACTION_INDICATOR:
		return parse_dataobj_items_next_action_indicator;
	case STK_DATA_OBJECT_TYPE_EVENT_LIST:
		return parse_dataobj_event_list;
	case STK_DATA_OBJECT_TYPE_CAUSE:
		return parse_dataobj_cause;
	case STK_DATA_OBJECT_TYPE_LOCATION_STATUS:
		return parse_dataobj_location_status;
	case STK_DATA_OBJECT_TYPE_TRANSACTION_ID:
		return parse_dataobj_transaction_id;
	case STK_DATA_OBJECT_TYPE_BCCH_CHANNEL_LIST:
		return parse_dataobj_bcch_channel_list;
	case STK_DATA_OBJECT_TYPE_CALL_CONTROL_REQUESTED_ACTION:
		return parse_dataobj_call_control_requested_action;
	case STK_DATA_OBJECT_TYPE_ICON_ID:
		return parse_dataobj_icon_id;
	case STK_DATA_OBJECT_TYPE_ITEM_ICON_ID_LIST:
		return parse_dataobj_item_icon_id_list;
	case STK_DATA_OBJECT_TYPE_CARD_READER_STATUS:
		return parse_dataobj_card_reader_status;
	case STK_DATA_OBJECT_TYPE_CARD_ATR:
		return parse_dataobj_card_atr;
	case STK_DATA_OBJECT_TYPE_C_APDU:
		return parse_dataobj_c_apdu;
	case STK_DATA_OBJECT_TYPE_R_APDU:
		return parse_dataobj_r_apdu;
	case STK_DATA_OBJECT_TYPE_TIMER_ID:
		return parse_dataobj_timer_id;
	case STK_DATA_OBJECT_TYPE_TIMER_VALUE:
		return parse_dataobj_timer_value;
	case STK_DATA_OBJECT_TYPE_DATETIME_TIMEZONE:
		return parse_dataobj_datetime_timezone;
	case STK_DATA_OBJECT_TYPE_AT_COMMAND:
		return parse_dataobj_at_command;
	case STK_DATA_OBJECT_TYPE_AT_RESPONSE:
		return parse_dataobj_at_response;
	case STK_DATA_OBJECT_TYPE_BC_REPEAT_INDICATOR:
		return parse_dataobj_bc_repeat_indicator;
	case STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE:
		return parse_dataobj_imm_resp;
	case STK_DATA_OBJECT_TYPE_DTMF_STRING:
		return parse_dataobj_dtmf_string;
	case STK_DATA_OBJECT_TYPE_LANGUAGE:
		return parse_dataobj_language;
	case STK_DATA_OBJECT_TYPE_BROWSER_ID:
		return parse_dataobj_browser_id;
	case STK_DATA_OBJECT_TYPE_TIMING_ADVANCE:
		return parse_dataobj_timing_advance;
	case STK_DATA_OBJECT_TYPE_URL:
		return parse_dataobj_url;
	case STK_DATA_OBJECT_TYPE_BEARER:
		return parse_dataobj_bearer;
	case STK_DATA_OBJECT_TYPE_PROVISIONING_FILE_REF:
		return parse_dataobj_provisioning_file_reference;
	case STK_DATA_OBJECT_TYPE_BROWSER_TERMINATION_CAUSE:
		return parse_dataobj_browser_termination_cause;
	case STK_DATA_OBJECT_TYPE_BEARER_DESCRIPTION:
		return parse_dataobj_bearer_description;
	case STK_DATA_OBJECT_TYPE_CHANNEL_DATA:
		return parse_dataobj_channel_data;
	case STK_DATA_OBJECT_TYPE_CHANNEL_DATA_LENGTH:
		return parse_dataobj_channel_data_length;
	case STK_DATA_OBJECT_TYPE_BUFFER_SIZE:
		return parse_dataobj_buffer_size;
	case STK_DATA_OBJECT_TYPE_CHANNEL_STATUS:
		return parse_dataobj_channel_status;
	case STK_DATA_OBJECT_TYPE_CARD_READER_ID:
		return parse_dataobj_card_reader_id;
	case STK_DATA_OBJECT_TYPE_OTHER_ADDRESS:
		return parse_dataobj_other_address;
	case STK_DATA_OBJECT_TYPE_UICC_TE_INTERFACE:
		return parse_dataobj_uicc_te_interface;
	case STK_DATA_OBJECT_TYPE_AID:
		return parse_dataobj_aid;
	case STK_DATA_OBJECT_TYPE_ACCESS_TECHNOLOGY:
		return parse_dataobj_access_technology;
	case STK_DATA_OBJECT_TYPE_DISPLAY_PARAMETERS:
		return parse_dataobj_display_parameters;
	case STK_DATA_OBJECT_TYPE_SERVICE_RECORD:
		return parse_dataobj_service_record;
	case STK_DATA_OBJECT_TYPE_DEVICE_FILTER:
		return parse_dataobj_device_filter;
	case STK_DATA_OBJECT_TYPE_SERVICE_SEARCH:
		return parse_dataobj_service_search;
	case STK_DATA_OBJECT_TYPE_ATTRIBUTE_INFO:
		return parse_dataobj_attribute_info;
	case STK_DATA_OBJECT_TYPE_SERVICE_AVAILABILITY:
		return parse_dataobj_service_availability;
	case STK_DATA_OBJECT_TYPE_REMOTE_ENTITY_ADDRESS:
		return parse_dataobj_remote_entity_address;
	case STK_DATA_OBJECT_TYPE_ESN:
		return parse_dataobj_esn;
	case STK_DATA_OBJECT_TYPE_NETWORK_ACCESS_NAME:
		return parse_dataobj_network_access_name;
	case STK_DATA_OBJECT_TYPE_CDMA_SMS_TPDU:
		return parse_dataobj_cdma_sms_tpdu;
	case STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE:
		return parse_dataobj_text_attr;
	case STK_DATA_OBJECT_TYPE_PDP_ACTIVATION_PARAMETER:
		return parse_dataobj_pdp_act_par;
	case STK_DATA_OBJECT_TYPE_ITEM_TEXT_ATTRIBUTE_LIST:
		return parse_dataobj_item_text_attribute_list;
	case STK_DATA_OBJECT_TYPE_UTRAN_MEASUREMENT_QUALIFIER:
		return parse_dataobj_utran_meas_qualifier;
	case STK_DATA_OBJECT_TYPE_IMEISV:
		return parse_dataobj_imeisv;
	case STK_DATA_OBJECT_TYPE_NETWORK_SEARCH_MODE:
		return parse_dataobj_network_search_mode;
	case STK_DATA_OBJECT_TYPE_BATTERY_STATE:
		return parse_dataobj_battery_state;
	case STK_DATA_OBJECT_TYPE_BROWSING_STATUS:
		return parse_dataobj_browsing_status;
	case STK_DATA_OBJECT_TYPE_FRAME_LAYOUT:
		return parse_dataobj_frame_layout;
	case STK_DATA_OBJECT_TYPE_FRAMES_INFO:
		return parse_dataobj_frames_info;
	case STK_DATA_OBJECT_TYPE_FRAME_ID:
		return parse_dataobj_frame_id;
	case STK_DATA_OBJECT_TYPE_MEID:
		return parse_dataobj_meid;
	case STK_DATA_OBJECT_TYPE_MMS_REFERENCE:
		return parse_dataobj_mms_reference;
	case STK_DATA_OBJECT_TYPE_MMS_ID:
		return parse_dataobj_mms_id;
	case STK_DATA_OBJECT_TYPE_MMS_TRANSFER_STATUS:
		return parse_dataobj_mms_transfer_status;
	case STK_DATA_OBJECT_TYPE_MMS_CONTENT_ID:
		return parse_dataobj_mms_content_id;
	case STK_DATA_OBJECT_TYPE_MMS_NOTIFICATION:
		return parse_dataobj_mms_notification;
	case STK_DATA_OBJECT_TYPE_LAST_ENVELOPE:
		return parse_dataobj_last_envelope;
	case STK_DATA_OBJECT_TYPE_REGISTRY_APPLICATION_DATA:
		return parse_dataobj_registry_application_data;
	case STK_DATA_OBJECT_TYPE_ACTIVATE_DESCRIPTOR:
		return parse_dataobj_activate_descriptor;
	case STK_DATA_OBJECT_TYPE_BROADCAST_NETWORK_INFO:
		return parse_dataobj_broadcast_network_info;
	default:
		return NULL;
	}
}

static void destroy_stk_item(struct stk_item *item)
{
	g_free(item->text);
	g_free(item);
}

static gboolean parse_item_list(struct comprehension_tlv_iter *iter,
				void *data)
{
	GSList **out = data;
	unsigned short tag = STK_DATA_OBJECT_TYPE_ITEM;
	struct comprehension_tlv_iter iter_old;
	struct stk_item item;
	GSList *list = NULL;
	unsigned int count = 0;
	gboolean has_empty = FALSE;

	do {
		comprehension_tlv_iter_copy(iter, &iter_old);
		memset(&item, 0, sizeof(item));
		count++;

		if (parse_dataobj_item(iter, &item) == TRUE) {
			if (item.id == 0) {
				has_empty = TRUE;
				continue;
			}

			list = g_slist_prepend(list,
						g_memdup(&item, sizeof(item)));
		}
	} while (comprehension_tlv_iter_next(iter) == TRUE &&
			comprehension_tlv_iter_get_tag(iter) == tag);

	comprehension_tlv_iter_copy(&iter_old, iter);

	if (!has_empty) {
		*out = g_slist_reverse(list);
		return TRUE;
	}

	if (count == 1)
		return TRUE;

	g_slist_foreach(list, (GFunc) destroy_stk_item, NULL);
	g_slist_free(list);
	return FALSE;

}

static gboolean parse_provisioning_list(struct comprehension_tlv_iter *iter,
					void *data)
{
	GSList **out = data;
	unsigned short tag = STK_DATA_OBJECT_TYPE_PROVISIONING_FILE_REF;
	struct comprehension_tlv_iter iter_old;
	struct stk_file file;
	GSList *list = NULL;

	do {
		comprehension_tlv_iter_copy(iter, &iter_old);
		memset(&file, 0, sizeof(file));

		if (parse_dataobj_provisioning_file_reference(iter, &file)
									== TRUE)
			list = g_slist_prepend(list,
						g_memdup(&file, sizeof(file)));
	} while (comprehension_tlv_iter_next(iter) == TRUE &&
			comprehension_tlv_iter_get_tag(iter) == tag);

	comprehension_tlv_iter_copy(&iter_old, iter);
	*out = g_slist_reverse(list);

	return TRUE;
}

static dataobj_handler list_handler_for_type(enum stk_data_object_type type)
{
	switch (type) {
	case STK_DATA_OBJECT_TYPE_ITEM:
		return parse_item_list;
	case STK_DATA_OBJECT_TYPE_PROVISIONING_FILE_REF:
		return parse_provisioning_list;
	default:
		return NULL;
	}
}

struct dataobj_handler_entry {
	enum stk_data_object_type type;
	int flags;
	void *data;
};

static enum stk_command_parse_result parse_dataobj(
					struct comprehension_tlv_iter *iter,
					enum stk_data_object_type type, ...)
{
	GSList *entries = NULL;
	GSList *l;
	va_list args;
	gboolean minimum_set = TRUE;
	gboolean parse_error = FALSE;

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

	va_end(args);

	entries = g_slist_reverse(entries);

	l = entries;
	while (comprehension_tlv_iter_next(iter) == TRUE) {
		dataobj_handler handler;
		struct dataobj_handler_entry *entry;
		GSList *l2;

		for (l2 = l; l2; l2 = l2->next) {
			entry = l2->data;

			if (comprehension_tlv_iter_get_tag(iter) == entry->type)
				break;

			/* Can't skip over mandatory objects */
			if (entry->flags & DATAOBJ_FLAG_MANDATORY) {
				l2 = NULL;
				break;
			}
		}

		if (l2 == NULL) {
			if (comprehension_tlv_get_cr(iter) == TRUE)
				parse_error = TRUE;

			continue;
		}

		if (entry->flags & DATAOBJ_FLAG_LIST)
			handler = list_handler_for_type(entry->type);
		else
			handler = handler_for_type(entry->type);

		if (handler(iter, entry->data) == FALSE)
			parse_error = TRUE;

		l = l2->next;
	}

	for (; l; l = l->next) {
		struct dataobj_handler_entry *entry = l->data;

		if (entry->flags & DATAOBJ_FLAG_MANDATORY)
			minimum_set = FALSE;
	}

	g_slist_foreach(entries, (GFunc) g_free, NULL);
	g_slist_free(entries);

	if (minimum_set == FALSE)
		return STK_PARSE_RESULT_MISSING_VALUE;
	if (parse_error == TRUE)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static void destroy_display_text(struct stk_command *command)
{
	g_free(command->display_text.text);
}

static enum stk_command_parse_result parse_display_text(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_display_text *obj = &command->display_text;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_DISPLAY)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_display_text;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE, 0,
				&obj->immediate_response,
				STK_DATA_OBJECT_TYPE_DURATION, 0,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->text, obj->icon_id.id);

	return status;
}

static void destroy_get_inkey(struct stk_command *command)
{
	g_free(command->get_inkey.text);
}

static enum stk_command_parse_result parse_get_inkey(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_get_inkey *obj = &command->get_inkey;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_get_inkey;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_DURATION, 0,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->text, obj->icon_id.id);

	return status;
}

static void destroy_get_input(struct stk_command *command)
{
	g_free(command->get_input.text);
	g_free(command->get_input.default_text);
}

static enum stk_command_parse_result parse_get_input(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_get_input *obj = &command->get_input;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_get_input;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_RESPONSE_LENGTH,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->resp_len,
				STK_DATA_OBJECT_TYPE_DEFAULT_TEXT, 0,
				&obj->default_text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->text, obj->icon_id.id);

	return status;
}

static enum stk_command_parse_result parse_more_time(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static void destroy_play_tone(struct stk_command *command)
{
	g_free(command->play_tone.alpha_id);
}

static enum stk_command_parse_result parse_play_tone(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_play_tone *obj = &command->play_tone;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_EARPIECE)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_play_tone;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_TONE, 0,
				&obj->tone,
				STK_DATA_OBJECT_TYPE_DURATION, 0,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static enum stk_command_parse_result parse_poll_interval(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_poll_interval *obj = &command->poll_interval;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_DURATION,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_setup_menu(struct stk_command *command)
{
	g_free(command->setup_menu.alpha_id);
	g_slist_foreach(command->setup_menu.items,
				(GFunc) destroy_stk_item, NULL);
	g_slist_free(command->setup_menu.items);
}

static enum stk_command_parse_result parse_setup_menu(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_menu *obj = &command->setup_menu;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_setup_menu;

	status = parse_dataobj(iter,
			STK_DATA_OBJECT_TYPE_ALPHA_ID,
			DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
			&obj->alpha_id,
			STK_DATA_OBJECT_TYPE_ITEM,
			DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM |
			DATAOBJ_FLAG_LIST, &obj->items,
			STK_DATA_OBJECT_TYPE_ITEMS_NEXT_ACTION_INDICATOR, 0,
			&obj->next_act,
			STK_DATA_OBJECT_TYPE_ICON_ID, 0,
			&obj->icon_id,
			STK_DATA_OBJECT_TYPE_ITEM_ICON_ID_LIST, 0,
			&obj->item_icon_id_list,
			STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
			&obj->text_attr,
			STK_DATA_OBJECT_TYPE_ITEM_TEXT_ATTRIBUTE_LIST, 0,
			&obj->item_text_attr_list,
			STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_select_item(struct stk_command *command)
{
	g_free(command->select_item.alpha_id);
	g_slist_foreach(command->select_item.items,
				(GFunc) destroy_stk_item, NULL);
	g_slist_free(command->select_item.items);
}

static enum stk_command_parse_result parse_select_item(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_select_item *obj = &command->select_item;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	status = parse_dataobj(iter,
			STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
			&obj->alpha_id,
			STK_DATA_OBJECT_TYPE_ITEM,
			DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM |
			DATAOBJ_FLAG_LIST, &obj->items,
			STK_DATA_OBJECT_TYPE_ITEMS_NEXT_ACTION_INDICATOR, 0,
			&obj->next_act,
			STK_DATA_OBJECT_TYPE_ITEM_ID, 0,
			&obj->item_id,
			STK_DATA_OBJECT_TYPE_ICON_ID, 0,
			&obj->icon_id,
			STK_DATA_OBJECT_TYPE_ITEM_ICON_ID_LIST, 0,
			&obj->item_icon_id_list,
			STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
			&obj->text_attr,
			STK_DATA_OBJECT_TYPE_ITEM_TEXT_ATTRIBUTE_LIST, 0,
			&obj->item_text_attr_list,
			STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
			&obj->frame_id,
			STK_DATA_OBJECT_TYPE_INVALID);

	command->destructor = destroy_select_item;

	if (status == STK_PARSE_RESULT_OK && obj->items == NULL)
		status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_send_sms(struct stk_command *command)
{
	g_free(command->send_sms.alpha_id);
	g_free(command->send_sms.cdma_sms.array);
}

static enum stk_command_parse_result parse_send_sms(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_sms *obj = &command->send_sms;
	enum stk_command_parse_result status;
	struct gsm_sms_tpdu gsm_tpdu;
	struct stk_address sc_address = { 0, NULL };

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	memset(&gsm_tpdu, 0, sizeof(gsm_tpdu));
	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ADDRESS, 0,
				&sc_address,
				STK_DATA_OBJECT_TYPE_GSM_SMS_TPDU, 0,
				&gsm_tpdu,
				STK_DATA_OBJECT_TYPE_CDMA_SMS_TPDU, 0,
				&obj->cdma_sms,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	command->destructor = destroy_send_sms;

	if (status != STK_PARSE_RESULT_OK)
		goto out;

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	if (status != STK_PARSE_RESULT_OK)
		goto out;

	if (gsm_tpdu.len == 0 && obj->cdma_sms.len == 0) {
		status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		goto out;
	}

	if (gsm_tpdu.len > 0 && obj->cdma_sms.len > 0) {
		status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		goto out;
	}

	/* We don't process CDMA pdus for now */
	if (obj->cdma_sms.len > 0)
		goto out;

	/* packing is needed */
	if (command->qualifier & 0x01) {
		if (sms_decode_unpacked_stk_pdu(gsm_tpdu.tpdu, gsm_tpdu.len,
							&obj->gsm_sms) !=
				TRUE) {
			status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
			goto out;
		}

		goto set_addr;
	}

	if (sms_decode(gsm_tpdu.tpdu, gsm_tpdu.len, TRUE,
				gsm_tpdu.len, &obj->gsm_sms) == FALSE) {
		status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		goto out;
	}

	if (obj->gsm_sms.type != SMS_TYPE_SUBMIT &&
			obj->gsm_sms.type != SMS_TYPE_COMMAND) {
		status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		goto out;
	}

set_addr:
	if (sc_address.number == NULL)
		goto out;

	if (strlen(sc_address.number) > 20) {
		status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		goto out;
	}

	strcpy(obj->gsm_sms.sc_addr.address, sc_address.number);
	obj->gsm_sms.sc_addr.numbering_plan = sc_address.ton_npi & 15;
	obj->gsm_sms.sc_addr.number_type = (sc_address.ton_npi >> 4) & 7;

out:
	g_free(sc_address.number);

	return status;
}

static void destroy_send_ss(struct stk_command *command)
{
	g_free(command->send_ss.alpha_id);
	g_free(command->send_ss.ss.ss);
}

static enum stk_command_parse_result parse_send_ss(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_ss *obj = &command->send_ss;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_send_ss;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_SS_STRING,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->ss,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_send_ussd(struct stk_command *command)
{
	g_free(command->send_ussd.alpha_id);
}

static enum stk_command_parse_result parse_send_ussd(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_ussd *obj = &command->send_ussd;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_send_ussd;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_USSD_STRING,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->ussd_string,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_setup_call(struct stk_command *command)
{
	g_free(command->setup_call.alpha_id_usr_cfm);
	g_free(command->setup_call.addr.number);
	g_free(command->setup_call.alpha_id_call_setup);
}

static enum stk_command_parse_result parse_setup_call(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_call *obj = &command->setup_call;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_setup_call;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id_usr_cfm,
				STK_DATA_OBJECT_TYPE_ADDRESS,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->addr,
				STK_DATA_OBJECT_TYPE_CCP, 0,
				&obj->ccp,
				STK_DATA_OBJECT_TYPE_SUBADDRESS, 0,
				&obj->subaddr,
				STK_DATA_OBJECT_TYPE_DURATION, 0,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id_usr_cfm,
				STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id_call_setup,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id_call_setup,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr_usr_cfm,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr_call_setup,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id_usr_cfm, obj->icon_id_usr_cfm.id);
	CHECK_TEXT_AND_ICON(obj->alpha_id_call_setup,
						obj->icon_id_call_setup.id);

	return status;
}

static void destroy_refresh(struct stk_command *command)
{
	g_slist_foreach(command->refresh.file_list, (GFunc) g_free, NULL);
	g_slist_free(command->refresh.file_list);
	g_free(command->refresh.alpha_id);
}

static enum stk_command_parse_result parse_refresh(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_refresh *obj = &command->refresh;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_refresh;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_FILE_LIST, 0,
				&obj->file_list,
				STK_DATA_OBJECT_TYPE_AID, 0,
				&obj->aid,
				STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static enum stk_command_parse_result parse_polling_off(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static enum stk_command_parse_result parse_provide_local_info(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static enum stk_command_parse_result parse_setup_event_list(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_event_list *obj = &command->setup_event_list;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_EVENT_LIST,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->event_list,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static enum stk_command_parse_result parse_perform_card_apdu(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_perform_card_apdu *obj = &command->perform_card_apdu;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_C_APDU,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->c_apdu,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static enum stk_command_parse_result parse_power_off_card(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static enum stk_command_parse_result parse_power_on_card(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static enum stk_command_parse_result parse_get_reader_status(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	switch (command->qualifier) {
	case STK_QUALIFIER_TYPE_CARD_READER_STATUS:
		if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
			return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		break;
	case STK_QUALIFIER_TYPE_CARD_READER_ID:
		if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
				(command->dst >
					STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
			return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		break;
	default:
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
	}

	return STK_PARSE_RESULT_OK;
}

static enum stk_command_parse_result parse_timer_mgmt(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_timer_mgmt *obj = &command->timer_mgmt;
	enum stk_data_object_flag value_flags = 0;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if ((command->qualifier & 3) == 0) /* Start a timer */
		value_flags = DATAOBJ_FLAG_MANDATORY;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TIMER_ID,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->timer_id,
				STK_DATA_OBJECT_TYPE_TIMER_VALUE, value_flags,
				&obj->timer_value,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_setup_idle_mode_text(struct stk_command *command)
{
	g_free(command->setup_idle_mode_text.text);
}

static enum stk_command_parse_result parse_setup_idle_mode_text(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_idle_mode_text *obj =
					&command->setup_idle_mode_text;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_setup_idle_mode_text;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->text, obj->icon_id.id);

	return status;
}

static void destroy_run_at_command(struct stk_command *command)
{
	g_free(command->run_at_command.alpha_id);
	g_free(command->run_at_command.at_command);
}

static enum stk_command_parse_result parse_run_at_command(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_run_at_command *obj = &command->run_at_command;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_run_at_command;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_AT_COMMAND,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->at_command,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_send_dtmf(struct stk_command *command)
{
	g_free(command->send_dtmf.alpha_id);
	g_free(command->send_dtmf.dtmf);
}

static enum stk_command_parse_result parse_send_dtmf(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_dtmf *obj = &command->send_dtmf;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_send_dtmf;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_DTMF_STRING,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->dtmf,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static enum stk_command_parse_result parse_language_notification(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_language_notification *obj =
					&command->language_notification;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_LANGUAGE, 0,
				&obj->language,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_launch_browser(struct stk_command *command)
{
	g_free(command->launch_browser.url);
	g_free(command->launch_browser.bearer.array);
	g_slist_foreach(command->launch_browser.prov_file_refs,
				(GFunc) g_free, NULL);
	g_slist_free(command->launch_browser.prov_file_refs);
	g_free(command->launch_browser.text_gateway_proxy_id);
	g_free(command->launch_browser.alpha_id);
	g_free(command->launch_browser.network_name.array);
	g_free(command->launch_browser.text_usr);
	g_free(command->launch_browser.text_passwd);
}

static enum stk_command_parse_result parse_launch_browser(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_launch_browser *obj = &command->launch_browser;

	if (command->qualifier > 3 || command->qualifier == 1)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_launch_browser;

	return parse_dataobj(iter,
				STK_DATA_OBJECT_TYPE_BROWSER_ID, 0,
				&obj->browser_id,
				STK_DATA_OBJECT_TYPE_URL,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->url,
				STK_DATA_OBJECT_TYPE_BEARER, 0,
				&obj->bearer,
				STK_DATA_OBJECT_TYPE_PROVISIONING_FILE_REF,
				DATAOBJ_FLAG_LIST,
				&obj->prov_file_refs,
				STK_DATA_OBJECT_TYPE_TEXT, 0,
				&obj->text_gateway_proxy_id,
				STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_NETWORK_ACCESS_NAME, 0,
				&obj->network_name,
				STK_DATA_OBJECT_TYPE_TEXT, 0,
				&obj->text_usr,
				STK_DATA_OBJECT_TYPE_TEXT, 0,
				&obj->text_passwd,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_open_channel(struct stk_command *command)
{
	g_free(command->open_channel.alpha_id);
	g_free(command->open_channel.apn);
	g_free(command->open_channel.text_usr);
	g_free(command->open_channel.text_passwd);
}

static enum stk_command_parse_result parse_open_channel(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_open_channel *obj = &command->open_channel;
	enum stk_command_parse_result status;

	if (command->qualifier >= 0x08)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_open_channel;

	/*
	 * parse the Open Channel data objects related to packet data service
	 * bearer
	 */
	status = parse_dataobj(iter,
				STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_BEARER_DESCRIPTION,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->bearer_desc,
				STK_DATA_OBJECT_TYPE_BUFFER_SIZE,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->buf_size,
				STK_DATA_OBJECT_TYPE_NETWORK_ACCESS_NAME, 0,
				&obj->apn,
				STK_DATA_OBJECT_TYPE_OTHER_ADDRESS, 0,
				&obj->local_addr,
				STK_DATA_OBJECT_TYPE_TEXT, 0,
				&obj->text_usr,
				STK_DATA_OBJECT_TYPE_TEXT, 0,
				&obj->text_passwd,
				STK_DATA_OBJECT_TYPE_UICC_TE_INTERFACE, 0,
				&obj->uti,
				STK_DATA_OBJECT_TYPE_OTHER_ADDRESS, 0,
				&obj->data_dest_addr,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_close_channel(struct stk_command *command)
{
	g_free(command->close_channel.alpha_id);
}

static enum stk_command_parse_result parse_close_channel(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_close_channel *obj = &command->close_channel;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CHANNEL_1) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CHANNEL_7))
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_close_channel;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_receive_data(struct stk_command *command)
{
	g_free(command->receive_data.alpha_id);
}

static enum stk_command_parse_result parse_receive_data(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_receive_data *obj = &command->receive_data;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CHANNEL_1) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CHANNEL_7))
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_receive_data;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_CHANNEL_DATA_LENGTH,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->data_len,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_send_data(struct stk_command *command)
{
	g_free(command->send_data.alpha_id);
	g_free(command->send_data.data.array);
}

static enum stk_command_parse_result parse_send_data(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_data *obj = &command->send_data;
	enum stk_command_parse_result status;

	if (command->qualifier > STK_SEND_DATA_IMMEDIATELY)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CHANNEL_1) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CHANNEL_7))
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_send_data;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_CHANNEL_DATA,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->data,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static enum stk_command_parse_result parse_get_channel_status(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static void destroy_service_search(struct stk_command *command)
{
	g_free(command->service_search.alpha_id);
	g_free(command->service_search.serv_search.ser_search);
	g_free(command->service_search.dev_filter.dev_filter);
}

static enum stk_command_parse_result parse_service_search(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_service_search *obj = &command->service_search;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_service_search;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_SERVICE_SEARCH,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->serv_search,
				STK_DATA_OBJECT_TYPE_DEVICE_FILTER, 0,
				&obj->dev_filter,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_get_service_info(struct stk_command *command)
{
	g_free(command->get_service_info.alpha_id);
	g_free(command->get_service_info.attr_info.attr_info);
}

static enum stk_command_parse_result parse_get_service_info(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_get_service_info *obj = &command->get_service_info;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_get_service_info;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_ATTRIBUTE_INFO,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->attr_info,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static void destroy_declare_service(struct stk_command *command)
{
	g_free(command->declare_service.serv_rec.serv_rec);
}

static enum stk_command_parse_result parse_declare_service(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_declare_service *obj = &command->declare_service;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_declare_service;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_SERVICE_RECORD,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->serv_rec,
				STK_DATA_OBJECT_TYPE_UICC_TE_INTERFACE, 0,
				&obj->intf,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static enum stk_command_parse_result parse_set_frames(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_set_frames *obj = &command->set_frames;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_FRAME_ID,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_FRAME_LAYOUT, 0,
				&obj->frame_layout,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id_default,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static enum stk_command_parse_result parse_get_frames_status(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return STK_PARSE_RESULT_OK;
}

static void destroy_retrieve_mms(struct stk_command *command)
{
	g_free(command->retrieve_mms.alpha_id);
	g_slist_foreach(command->retrieve_mms.mms_rec_files,
						(GFunc) g_free, NULL);
	g_slist_free(command->retrieve_mms.mms_rec_files);
}

static enum stk_command_parse_result parse_retrieve_mms(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_retrieve_mms *obj = &command->retrieve_mms;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_retrieve_mms;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_MMS_REFERENCE,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->mms_ref,
				STK_DATA_OBJECT_TYPE_FILE_LIST,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->mms_rec_files,
				STK_DATA_OBJECT_TYPE_MMS_CONTENT_ID,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->mms_content_id,
				STK_DATA_OBJECT_TYPE_MMS_ID, 0,
				&obj->mms_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_submit_mms(struct stk_command *command)
{
	g_free(command->submit_mms.alpha_id);
	g_slist_foreach(command->submit_mms.mms_subm_files,
						(GFunc) g_free, NULL);
	g_slist_free(command->submit_mms.mms_subm_files);
}

static enum stk_command_parse_result parse_submit_mms(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_submit_mms *obj = &command->submit_mms;
	enum stk_command_parse_result status;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_submit_mms;

	status = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_FILE_LIST,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->mms_subm_files,
				STK_DATA_OBJECT_TYPE_MMS_ID, 0,
				&obj->mms_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	CHECK_TEXT_AND_ICON(obj->alpha_id, obj->icon_id.id);

	return status;
}

static void destroy_display_mms(struct stk_command *command)
{
	g_slist_foreach(command->display_mms.mms_subm_files,
						(GFunc) g_free, NULL);
	g_slist_free(command->display_mms.mms_subm_files);
}

static enum stk_command_parse_result parse_display_mms(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_display_mms *obj = &command->display_mms;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	command->destructor = destroy_display_mms;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_FILE_LIST,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->mms_subm_files,
				STK_DATA_OBJECT_TYPE_MMS_ID,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->mms_id,
				STK_DATA_OBJECT_TYPE_IMMEDIATE_RESPONSE, 0,
				&obj->imd_resp,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static enum stk_command_parse_result parse_activate(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_activate *obj = &command->activate;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;

	return parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ACTIVATE_DESCRIPTOR,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->actv_desc,
				STK_DATA_OBJECT_TYPE_INVALID);
}

static enum stk_command_parse_result parse_command_body(
					struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	switch (command->type) {
	case STK_COMMAND_TYPE_DISPLAY_TEXT:
		return parse_display_text(command, iter);
	case STK_COMMAND_TYPE_GET_INKEY:
		return parse_get_inkey(command, iter);
	case STK_COMMAND_TYPE_GET_INPUT:
		return parse_get_input(command, iter);
	case STK_COMMAND_TYPE_MORE_TIME:
		return parse_more_time(command, iter);
	case STK_COMMAND_TYPE_PLAY_TONE:
		return parse_play_tone(command, iter);
	case STK_COMMAND_TYPE_POLL_INTERVAL:
		return parse_poll_interval(command, iter);
	case STK_COMMAND_TYPE_SETUP_MENU:
		return parse_setup_menu(command, iter);
	case STK_COMMAND_TYPE_SELECT_ITEM:
		return parse_select_item(command, iter);
	case STK_COMMAND_TYPE_SEND_SMS:
		return parse_send_sms(command, iter);
	case STK_COMMAND_TYPE_SEND_SS:
		return parse_send_ss(command, iter);
	case STK_COMMAND_TYPE_SEND_USSD:
		return parse_send_ussd(command, iter);
	case STK_COMMAND_TYPE_SETUP_CALL:
		return parse_setup_call(command, iter);
	case STK_COMMAND_TYPE_REFRESH:
		return parse_refresh(command, iter);
	case STK_COMMAND_TYPE_POLLING_OFF:
		return parse_polling_off(command, iter);
	case STK_COMMAND_TYPE_PROVIDE_LOCAL_INFO:
		return parse_provide_local_info(command, iter);
	case STK_COMMAND_TYPE_SETUP_EVENT_LIST:
		return parse_setup_event_list(command, iter);
	case STK_COMMAND_TYPE_PERFORM_CARD_APDU:
		return parse_perform_card_apdu(command, iter);
	case STK_COMMAND_TYPE_POWER_OFF_CARD:
		return parse_power_off_card(command, iter);
	case STK_COMMAND_TYPE_POWER_ON_CARD:
		return parse_power_on_card(command, iter);
	case STK_COMMAND_TYPE_GET_READER_STATUS:
		return parse_get_reader_status(command, iter);
	case STK_COMMAND_TYPE_TIMER_MANAGEMENT:
		return parse_timer_mgmt(command, iter);
	case STK_COMMAND_TYPE_SETUP_IDLE_MODE_TEXT:
		return parse_setup_idle_mode_text(command, iter);
	case STK_COMMAND_TYPE_RUN_AT_COMMAND:
		return parse_run_at_command(command, iter);
	case STK_COMMAND_TYPE_SEND_DTMF:
		return parse_send_dtmf(command, iter);
	case STK_COMMAND_TYPE_LANGUAGE_NOTIFICATION:
		return parse_language_notification(command, iter);
	case STK_COMMAND_TYPE_LAUNCH_BROWSER:
		return parse_launch_browser(command, iter);
	case STK_COMMAND_TYPE_OPEN_CHANNEL:
		return parse_open_channel(command, iter);
	case STK_COMMAND_TYPE_CLOSE_CHANNEL:
		return parse_close_channel(command, iter);
	case STK_COMMAND_TYPE_RECEIVE_DATA:
		return parse_receive_data(command, iter);
	case STK_COMMAND_TYPE_SEND_DATA:
		return parse_send_data(command, iter);
	case STK_COMMAND_TYPE_GET_CHANNEL_STATUS:
		return parse_get_channel_status(command, iter);
	case STK_COMMAND_TYPE_SERVICE_SEARCH:
		return parse_service_search(command, iter);
	case STK_COMMAND_TYPE_GET_SERVICE_INFO:
		return parse_get_service_info(command, iter);
	case STK_COMMAND_TYPE_DECLARE_SERVICE:
		return parse_declare_service(command, iter);
	case STK_COMMAND_TYPE_SET_FRAMES:
		return parse_set_frames(command, iter);
	case STK_COMMAND_TYPE_GET_FRAMES_STATUS:
		return parse_get_frames_status(command, iter);
	case STK_COMMAND_TYPE_RETRIEVE_MMS:
		return parse_retrieve_mms(command, iter);
	case STK_COMMAND_TYPE_SUBMIT_MMS:
		return parse_submit_mms(command, iter);
	case STK_COMMAND_TYPE_DISPLAY_MMS:
		return parse_display_mms(command, iter);
	case STK_COMMAND_TYPE_ACTIVATE:
		return parse_activate(command, iter);
	default:
		return STK_PARSE_RESULT_TYPE_NOT_UNDERSTOOD;
	};
}

struct stk_command *stk_command_new_from_pdu(const unsigned char *pdu,
						unsigned int len)
{
	struct ber_tlv_iter ber;
	struct comprehension_tlv_iter iter;
	const unsigned char *data;
	struct stk_command *command;

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

	if (comprehension_tlv_iter_next(&iter) != TRUE) {
		command->status = STK_PARSE_RESULT_MISSING_VALUE;
		goto out;
	}

	if (comprehension_tlv_iter_get_tag(&iter) !=
			STK_DATA_OBJECT_TYPE_DEVICE_IDENTITIES) {
		command->status = STK_PARSE_RESULT_MISSING_VALUE;
		goto out;
	}

	if (comprehension_tlv_iter_get_length(&iter) != 0x02) {
		command->status = STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD;
		goto out;
	}

	data = comprehension_tlv_iter_get_data(&iter);

	command->src = data[0];
	command->dst = data[1];

	command->status = parse_command_body(command, &iter);

out:
	return command;
}

void stk_command_free(struct stk_command *command)
{
	if (command->destructor)
		command->destructor(command);

	g_free(command);
}

static gboolean stk_tlv_builder_init(struct stk_tlv_builder *iter,
						unsigned char *pdu,
						unsigned int size)
{
	iter->value = NULL;
	iter->len = 0;

	return comprehension_tlv_builder_init(&iter->ctlv, pdu, size);
}

static gboolean stk_tlv_builder_recurse(struct stk_tlv_builder *iter,
					struct ber_tlv_builder *btlv,
					unsigned char tag)
{
	iter->value = NULL;
	iter->len = 0;

	if (ber_tlv_builder_next(btlv, tag >> 6, (tag >> 5) & 1,
					tag & 0x1f) != TRUE)
		return FALSE;

	return ber_tlv_builder_recurse_comprehension(btlv, &iter->ctlv);
}

static gboolean stk_tlv_builder_open_container(struct stk_tlv_builder *iter,
						gboolean cr,
						unsigned char shorttag,
						gboolean relocatable)
{
	if (comprehension_tlv_builder_next(&iter->ctlv, cr, shorttag) != TRUE)
		return FALSE;

	iter->len = 0;
	iter->max_len = relocatable ? 0xff : 0x7f;
	if (comprehension_tlv_builder_set_length(&iter->ctlv, iter->max_len) !=
			TRUE)
		return FALSE;

	iter->value = comprehension_tlv_builder_get_data(&iter->ctlv);

	return TRUE;
}

static gboolean stk_tlv_builder_close_container(struct stk_tlv_builder *iter)
{
	return comprehension_tlv_builder_set_length(&iter->ctlv, iter->len);
}

static unsigned int stk_tlv_builder_get_length(struct stk_tlv_builder *iter)
{
	return comprehension_tlv_builder_get_data(&iter->ctlv) -
		iter->ctlv.pdu + iter->len;
}

static gboolean stk_tlv_builder_append_byte(struct stk_tlv_builder *iter,
						unsigned char num)
{
	if (iter->len >= iter->max_len)
		return FALSE;

	iter->value[iter->len++] = num;
	return TRUE;
}

static gboolean stk_tlv_builder_append_short(struct stk_tlv_builder *iter,
						unsigned short num)
{
	if (iter->len + 2 > iter->max_len)
		return FALSE;

	iter->value[iter->len++] = num >> 8;
	iter->value[iter->len++] = num & 0xff;
	return TRUE;
}

static gboolean stk_tlv_builder_append_gsm_packed(struct stk_tlv_builder *iter,
							const char *text)
{
	unsigned int len;
	unsigned char *gsm;
	long written = 0;

	if (text == NULL)
		return TRUE;

	len = strlen(text);

	gsm = convert_utf8_to_gsm(text, len, NULL, &written, 0);
	if (gsm == NULL && len > 0)
		return FALSE;

	if (iter->len + (written * 7 + 7) / 8 >= iter->max_len) {
		g_free(gsm);
		return FALSE;
	}

	pack_7bit_own_buf(gsm, len, 0, FALSE, &written, 0,
				iter->value + iter->len + 1);
	g_free(gsm);

	if (written < 1 && len > 0)
		return FALSE;

	iter->value[iter->len++] = 0x00;
	iter->len += written;

	return TRUE;
}

static gboolean stk_tlv_builder_append_gsm_unpacked(
						struct stk_tlv_builder *iter,
						const char *text)
{
	unsigned int len;
	unsigned char *gsm;
	long written = 0;

	if (text == NULL)
		return TRUE;

	len = strlen(text);

	gsm = convert_utf8_to_gsm(text, len, NULL, &written, 0);
	if (gsm == NULL && len > 0)
		return FALSE;

	if (iter->len + written >= iter->max_len) {
		g_free(gsm);
		return FALSE;
	}

	iter->value[iter->len++] = 0x04;
	memcpy(iter->value + iter->len, gsm, written);
	iter->len += written;

	g_free(gsm);

	return TRUE;
}

static gboolean stk_tlv_builder_append_ucs2(struct stk_tlv_builder *iter,
						const char *text)
{
	unsigned char *ucs2;
	gsize gwritten;

	ucs2 = (unsigned char *) g_convert((const gchar *) text, -1,
						"UCS-2BE", "UTF-8//TRANSLIT",
						NULL, &gwritten, NULL);
	if (ucs2 == NULL)
		return FALSE;

	if (iter->len + gwritten >= iter->max_len) {
		g_free(ucs2);
		return FALSE;
	}

	iter->value[iter->len++] = 0x08;

	memcpy(iter->value + iter->len, ucs2, gwritten);
	iter->len += gwritten;

	g_free(ucs2);

	return TRUE;
}

static gboolean stk_tlv_builder_append_text(struct stk_tlv_builder *iter,
						int dcs, const char *text)
{
	gboolean ret;

	switch (dcs) {
	case 0x00:
		return stk_tlv_builder_append_gsm_packed(iter, text);
	case 0x04:
		return stk_tlv_builder_append_gsm_unpacked(iter, text);
	case 0x08:
		return stk_tlv_builder_append_ucs2(iter, text);
	case -1:
		ret = stk_tlv_builder_append_gsm_unpacked(iter, text);

		if (ret == TRUE)
			return ret;

		return stk_tlv_builder_append_ucs2(iter, text);
	}

	return FALSE;
}

static inline gboolean stk_tlv_builder_append_bytes(struct stk_tlv_builder *iter,
						const unsigned char *data,
						unsigned int length)
{
	if (iter->len + length > iter->max_len)
		return FALSE;

	memcpy(iter->value + iter->len, data, length);
	iter->len += length;

	return TRUE;
}

/* Described in TS 102.223 Section 8.1 */
static gboolean build_dataobj_address(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_address *addr = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_ADDRESS;
	unsigned int len;
	unsigned char number[128];

	if (addr->number == NULL)
		return TRUE;

	len = (strlen(addr->number) + 1) / 2;
	sim_encode_bcd_number(addr->number, number);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, addr->ton_npi) &&
		stk_tlv_builder_append_bytes(tlv, number, len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.2 */
static gboolean build_dataobj_alpha_id(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	unsigned char tag = STK_DATA_OBJECT_TYPE_ALPHA_ID;
	int len;
	unsigned char *string;

	if (data == NULL)
		return TRUE;

	if (strlen(data) == 0)
		return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
			stk_tlv_builder_close_container(tlv);

	string = utf8_to_sim_string(data, -1, &len);
	if (string == NULL)
		return FALSE;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, string, len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.3 */
static gboolean build_dataobj_subaddress(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_subaddress *sa = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_SUBADDRESS;

	if (sa->has_subaddr == FALSE)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, sa->subaddr, sa->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.4 */
static gboolean build_dataobj_ccp(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_ccp *ccp = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CCP;

	if (ccp->len == 0)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, ccp->len) &&
		stk_tlv_builder_append_bytes(tlv, ccp->ccp, ccp->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.5 */
static gboolean build_dataobj_cbs_page(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct cbs *page = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CBS_PAGE;
	unsigned char pdu[88];

	if (cbs_encode(page, NULL, pdu) == FALSE)
		return FALSE;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, pdu, 88) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.6 */
static gboolean build_dataobj_item_id(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const unsigned char *item_id = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_ITEM_ID;

	if (*item_id == 0)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *item_id) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.8 */
static gboolean build_dataobj_duration(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_duration *duration = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_DURATION;

	if (duration->interval == 0x00)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, duration->unit) &&
		stk_tlv_builder_append_byte(tlv, duration->interval) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.12 */
static gboolean build_dataobj_result(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_result *result = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_RESULT;

	if (stk_tlv_builder_open_container(tlv, cr, tag, FALSE) == FALSE)
		return FALSE;

	if (stk_tlv_builder_append_byte(tlv, result->type) == FALSE)
		return FALSE;

	if (result->additional_len > 0)
		if (stk_tlv_builder_append_bytes(tlv, result->additional,
					result->additional_len) == FALSE)
			return FALSE;

	return stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.13 */
static gboolean build_dataobj_gsm_sms_tpdu(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct sms_deliver *msg = data;
	struct sms sms;
	unsigned char tag = STK_DATA_OBJECT_TYPE_GSM_SMS_TPDU;
	unsigned char tpdu[165];
	int tpdu_len;

	sms.type = SMS_TYPE_DELIVER;
	memset(&sms.sc_addr, 0, sizeof(sms.sc_addr));
	memcpy(&sms.deliver, msg, sizeof(sms.deliver));

	if (sms_encode(&sms, NULL, &tpdu_len, tpdu) == FALSE)
		return FALSE;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, tpdu + 1, tpdu_len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.14 */
static gboolean build_dataobj_ss_string(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_address *addr = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_SS_STRING;
	unsigned int len;
	unsigned char number[128];

	if (addr->number == NULL)
		return TRUE;

	len = (strlen(addr->number) + 1) / 2;
	sim_encode_bcd_number(addr->number, number);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, addr->ton_npi) &&
		stk_tlv_builder_append_bytes(tlv, number, len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Defined in TS 102.223 Section 8.15 */
static gboolean build_dataobj_text(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_answer_text *text = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TEXT;
	gboolean ret;

	if (text->text == NULL && !text->yesno)
		return TRUE;

	if (stk_tlv_builder_open_container(tlv, cr, tag, TRUE) != TRUE)
		return FALSE;

	if (text->yesno == TRUE) {
		/*
		 * Section 6.8.5:
		 * When the terminal issues [...] command qualifier set
		 * to "Yes/No", it shall supply the value "01" when the
		 * answer is "positive" and the value '00' when the
		 * answer is "negative" in the text string data object.
		 */
		if (stk_tlv_builder_append_byte(tlv, 0x04) != TRUE)
			return FALSE;

		ret = stk_tlv_builder_append_byte(tlv,
						text->text ? 0x01 : 0x00);
	} else if (text->packed)
		ret = stk_tlv_builder_append_gsm_packed(tlv, text->text);
	else
		ret = stk_tlv_builder_append_text(tlv, -1, text->text);

	if (ret != TRUE)
		return ret;

	return stk_tlv_builder_close_container(tlv);
}

/* Defined in TS 102.223 Section 8.15 - USSD specific case*/
static gboolean build_dataobj_ussd_text(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_ussd_text *text = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TEXT;

	if (text->has_text == FALSE)
		return TRUE;

	if (stk_tlv_builder_open_container(tlv, cr, tag, TRUE) != TRUE)
		return FALSE;

	if (text->len > 0) {
		if (stk_tlv_builder_append_byte(tlv, text->dcs) != TRUE)
			return FALSE;

		if (stk_tlv_builder_append_bytes(tlv, text->text,
							text->len) != TRUE)
			return FALSE;
	}

	return stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.17 */
static gboolean build_dataobj_ussd_string(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_ussd_string *ussd = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_USSD_STRING;

	if (ussd->string == NULL)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, ussd->dcs) &&
		stk_tlv_builder_append_bytes(tlv, ussd->string, ussd->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.18 */
static gboolean build_dataobj_file_list(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	GSList *l = (void *) data;
	const struct stk_file *file;
	unsigned char tag = STK_DATA_OBJECT_TYPE_FILE_LIST;

	if (stk_tlv_builder_open_container(tlv, cr, tag, TRUE) != TRUE)
		return FALSE;

	if (stk_tlv_builder_append_byte(tlv, g_slist_length(l)) != TRUE)
		return FALSE;

	for (; l; l = l->next) {
		file = l->data;

		if (stk_tlv_builder_append_bytes(tlv, file->file,
							file->len) != TRUE)
			return FALSE;
	}

	return stk_tlv_builder_close_container(tlv);
}

/* Shortcut for a single File element */
static gboolean build_dataobj_file(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	GSList l = {
		.data = (void *) data,
		.next = NULL,
	};

	return build_dataobj_file_list(tlv, &l, cr);
}

/* Described in TS 102.223 Section 8.19 */
static gboolean build_dataobj_location_info(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_location_info *li = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_LOCATION_INFO;
	guint8 mccmnc[3];

	if (li->mcc[0] == '\0')
		return TRUE;

	sim_encode_mcc_mnc(mccmnc, li->mcc, li->mnc);

	if (stk_tlv_builder_open_container(tlv, cr, tag, FALSE) == FALSE)
		return FALSE;

	if (stk_tlv_builder_append_bytes(tlv, mccmnc, 3) == FALSE)
		return FALSE;

	if (stk_tlv_builder_append_short(tlv, li->lac_tac) == FALSE)
		return FALSE;

	if (li->has_ci && stk_tlv_builder_append_short(tlv, li->ci) == FALSE)
		return FALSE;

	if (li->has_ext_ci &&
			stk_tlv_builder_append_short(tlv, li->ext_ci) == FALSE)
		return FALSE;

	if (li->has_eutran_ci) {
		if (stk_tlv_builder_append_short(tlv,
					li->eutran_ci >> 12) == FALSE)
			return FALSE;

		if (stk_tlv_builder_append_short(tlv,
					(li->eutran_ci << 4) | 0xf) == FALSE)
			return FALSE;
	}

	return stk_tlv_builder_close_container(tlv);
}

static gboolean build_empty_dataobj_location_info(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	unsigned char tag = STK_DATA_OBJECT_TYPE_LOCATION_INFO;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_close_container(tlv);
}

/*
 * Described in TS 102.223 Section 8.20
 *
 * See format note in parse_dataobj_imei.
 */
static gboolean build_dataobj_imei(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	char byte0[3];
	const char *imei = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_IMEI;
	unsigned char value[8];

	if (imei == NULL)
		return TRUE;

	if (strlen(imei) != 15)
		return FALSE;

	byte0[0] = '*';
	byte0[1] = imei[0];
	byte0[2] = '\0';
	sim_encode_bcd_number(byte0, value);
	sim_encode_bcd_number(imei + 1, value + 1);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, value, 8) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.21 */
static gboolean build_dataobj_help_request(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const ofono_bool_t *help = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_HELP_REQUEST;

	if (*help != TRUE)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.22 */
static gboolean build_dataobj_network_measurement_results(
						struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_common_byte_array *nmr = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_NETWORK_MEASUREMENT_RESULTS;

	if (stk_tlv_builder_open_container(tlv, cr, tag, FALSE) == FALSE)
		return FALSE;

	if (nmr->len > 0 && stk_tlv_builder_append_bytes(tlv,
					nmr->array, nmr->len) == FALSE)
		return FALSE;

	return stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.25 */
static gboolean build_dataobj_event_list(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_event_list *list = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_EVENT_LIST;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, list->list, list->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Shortcut for a single Event type */
static gboolean build_dataobj_event_type(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_event_list list = {
		.list = { *(enum stk_event_type *) data },
		.len = 1,
	};

	return build_dataobj_event_list(tlv, &list, cr);
}

/* Described in TS 102.223 Section 8.26 */
static gboolean build_dataobj_cause(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_cause *cause = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CAUSE;

	if (cause->has_cause == FALSE)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, cause->cause, cause->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.27 */
static gboolean build_dataobj_location_status(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const enum stk_service_state *state = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_LOCATION_STATUS;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *state) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.28 */
static gboolean build_dataobj_transaction_ids(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_transaction_id *id = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TRANSACTION_ID;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, id->list, id->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Shortcut for a single Transaction ID */
static gboolean build_dataobj_transaction_id(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_transaction_id ids = {
		.list = { *(uint8_t *) data },
		.len = 1,
	};

	return build_dataobj_transaction_ids(tlv, &ids, cr);
}

/* Described in 3GPP 31.111 Section 8.29 */
static gboolean build_dataobj_bcch_channel_list(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_bcch_channel_list *list = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BCCH_CHANNEL_LIST;
	unsigned int i, bytes, pos, shift;
	unsigned char value;

	if (list->has_list == FALSE)
		return TRUE;

	if (stk_tlv_builder_open_container(tlv, cr, tag, TRUE) != TRUE)
		return FALSE;

	bytes = (list->num * 10 + 7) / 8;
	for (i = 0; i < bytes; i++) {
		pos = (i * 8 + 7) / 10;
		shift = pos * 10 + 10 - i * 8 - 8;

		value = 0;
		if (pos < list->num)
			value |= list->channels[pos] >> shift;
		if (shift > 2)
			value |= list->channels[pos - 1] << (10 - shift);

		if (stk_tlv_builder_append_byte(tlv, value) != TRUE)
			return FALSE;
	}

	return stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.30 */
static gboolean build_dataobj_cc_requested_action(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_common_byte_array *action = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CALL_CONTROL_REQUESTED_ACTION;

	if (action->array == NULL)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, action->array, action->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.33 */
static gboolean build_dataobj_card_reader_status(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_reader_status *status = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CARD_READER_STATUS;
	unsigned char byte;

	byte = status->id |
		(status->removable << 3) |
		(status->present << 4) |
		(status->id1_size << 5) |
		(status->card_present << 6) |
		(status->card_powered << 7);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, byte) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.37 */
static gboolean build_dataobj_timer_id(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const unsigned char *id = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TIMER_ID;

	if (id[0] == 0)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, id[0]) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.38 */
static gboolean build_dataobj_timer_value(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_timer_value *value = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TIMER_VALUE;

	if (value->has_value == FALSE)
		return TRUE;

#define TO_BCD(bin) ((((bin) / 10) & 0xf) | (((bin) % 10) << 4))
	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, TO_BCD(value->hour)) &&
		stk_tlv_builder_append_byte(tlv, TO_BCD(value->minute)) &&
		stk_tlv_builder_append_byte(tlv, TO_BCD(value->second)) &&
		stk_tlv_builder_close_container(tlv);
#undef TO_BCD
}

/* Described in TS 102.223 Section 8.39 */
static gboolean build_dataobj_datetime_timezone(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct sms_scts *scts = data;
	unsigned char value[7];
	int offset = 0;
	unsigned char tag = STK_DATA_OBJECT_TYPE_DATETIME_TIMEZONE;

	if (scts->month == 0 && scts->day == 0)
		return TRUE;

	if (sms_encode_scts(scts, value, &offset) != TRUE)
		return FALSE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, value, 7) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.41 */
static gboolean build_dataobj_at_response(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	unsigned char tag = STK_DATA_OBJECT_TYPE_AT_RESPONSE;
	int len;

	if (data == NULL)
		return TRUE;

	/*
	 * "If the AT Response string is longer than the maximum length
	 * capable of being transmitted to the UICC then the AT Response
	 * string shall be truncated to this length by the terminal."
	 */
	len = strlen(data);
	if (len > 240) /* Safe pick */
		len = 240;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, data, len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.42 */
static gboolean build_dataobj_bc_repeat(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	unsigned char tag = STK_DATA_OBJECT_TYPE_BC_REPEAT_INDICATOR;
	const struct stk_bc_repeat *bcr = data;

	if (bcr->has_bc_repeat == FALSE)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_byte(tlv, bcr->value) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.45 */
static gboolean build_dataobj_language(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	unsigned char tag = STK_DATA_OBJECT_TYPE_LANGUAGE;

	if (data == NULL)
		return TRUE;

	/*
	 * Coded as two GSM 7-bit characters with eighth bit clear.  Since
	 * ISO 639-2 codes use only english alphabet letters, no conversion
	 * from UTF-8 to GSM is needed.
	 */
	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, data, 2) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in 3GPP TS 31.111 Section 8.46 */
static gboolean build_dataobj_timing_advance(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_timing_advance *tadv = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TIMING_ADVANCE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, tadv->status) &&
		stk_tlv_builder_append_byte(tlv, tadv->advance) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.51 */
static gboolean build_dataobj_browser_termination_cause(
						struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const enum stk_browser_termination_cause *cause = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BROWSER_TERMINATION_CAUSE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *cause) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.52 */
static gboolean build_dataobj_bearer_description(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_bearer_description *bd = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BEARER_DESCRIPTION;

	if (bd->type != STK_BEARER_TYPE_GPRS_UTRAN)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, bd->type) &&
		stk_tlv_builder_append_byte(tlv,
			bd->gprs.precedence) &&
		stk_tlv_builder_append_byte(tlv,
			bd->gprs.delay) &&
		stk_tlv_builder_append_byte(tlv,
			bd->gprs.reliability) &&
		stk_tlv_builder_append_byte(tlv,
			bd->gprs.peak) &&
		stk_tlv_builder_append_byte(tlv,
			bd->gprs.mean) &&
		stk_tlv_builder_append_byte(tlv,
			bd->gprs.pdp_type) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.53 */
static gboolean build_dataobj_channel_data(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_common_byte_array *cd = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CHANNEL_DATA;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, cd->array, cd->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.54 */
static gboolean build_dataobj_channel_data_length(
						struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const unsigned short *length = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CHANNEL_DATA_LENGTH;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, MIN(*length, 255)) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.55 */
static gboolean build_dataobj_buffer_size(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const unsigned short *buf_size = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BUFFER_SIZE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_short(tlv, *buf_size) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.56 */
static gboolean build_dataobj_channel_status(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_channel *channel = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_CHANNEL_STATUS;
	unsigned char byte[2];

	switch (channel->status) {
	case STK_CHANNEL_PACKET_DATA_SERVICE_NOT_ACTIVATED:
	case STK_CHANNEL_TCP_IN_CLOSED_STATE:
		byte[0] = channel->id;
		byte[1] = 0x00;
		break;
	case STK_CHANNEL_PACKET_DATA_SERVICE_ACTIVATED:
	case STK_CHANNEL_TCP_IN_ESTABLISHED_STATE:
		byte[0] = channel->id | 0x80;
		byte[1] = 0x00;
		break;
	case STK_CHANNEL_TCP_IN_LISTEN_STATE:
		byte[0] = channel->id | 0x40;
		byte[1] = 0x00;
		break;
	case STK_CHANNEL_LINK_DROPPED:
		byte[0] = channel->id;
		byte[1] = 0x05;
		break;
	}

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
			stk_tlv_builder_append_bytes(tlv, byte, 2) &&
			stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.58 */
static gboolean build_dataobj_other_address(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_other_address *addr = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_OTHER_ADDRESS;
	gboolean ok = FALSE;

	if (!addr->type)
		return TRUE;

	if (stk_tlv_builder_open_container(tlv, cr, tag, FALSE) == FALSE)
		return FALSE;

	switch (addr->type) {
	case STK_ADDRESS_AUTO:
		ok = TRUE;
		break;
	case STK_ADDRESS_IPV4:
		ok = stk_tlv_builder_append_byte(tlv, addr->type) &&
			stk_tlv_builder_append_bytes(tlv,
					(const guint8 *) &addr->addr.ipv4, 4);
		break;
	case STK_ADDRESS_IPV6:
		ok = stk_tlv_builder_append_byte(tlv, addr->type) &&
			stk_tlv_builder_append_bytes(tlv, addr->addr.ipv6, 16);
		break;
	}

	if (!ok)
		return FALSE;

	return stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.59 */
static gboolean build_dataobj_uicc_te_interface(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_uicc_te_interface *iface = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_UICC_TE_INTERFACE;

	if (iface->protocol == 0 && iface->port == 0)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, iface->protocol) &&
		stk_tlv_builder_append_short(tlv, iface->port) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.61 */
static gboolean build_dataobj_access_technologies(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_access_technologies *techs = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_ACCESS_TECHNOLOGY;
	int i;

	if (stk_tlv_builder_open_container(tlv, cr, tag, FALSE) != TRUE)
		return FALSE;

	for (i = 0; i < techs->length; i++)
		if (stk_tlv_builder_append_byte(tlv, techs->techs[i]) != TRUE)
			return FALSE;

	return stk_tlv_builder_close_container(tlv);
}

/* Shortcut for a single Access Technology */
static gboolean build_dataobj_access_technology(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_access_technologies techs = {
		.techs = data,
		.length = 1,
	};

	return build_dataobj_access_technologies(tlv, &techs, cr);
}

/* Described in TS 102.223 Section 8.62 */
static gboolean build_dataobj_display_parameters(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_display_parameters *params = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_DISPLAY_PARAMETERS;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, params->height) &&
		stk_tlv_builder_append_byte(tlv, params->width) &&
		stk_tlv_builder_append_byte(tlv, params->effects) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.63 */
static gboolean build_dataobj_service_record(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_service_record *rec = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_SERVICE_RECORD;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_byte(tlv, rec->tech_id) &&
		stk_tlv_builder_append_byte(tlv, rec->serv_id) &&
		stk_tlv_builder_append_bytes(tlv, rec->serv_rec, rec->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.68 */
static gboolean build_dataobj_remote_entity_address(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_remote_entity_address *addr = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_REMOTE_ENTITY_ADDRESS;
	gboolean ok = FALSE;

	if (addr->has_address != TRUE)
		return TRUE;

	if (stk_tlv_builder_open_container(tlv, cr, tag, TRUE) != TRUE)
		return FALSE;

	if (stk_tlv_builder_append_byte(tlv, addr->coding_type) != TRUE)
		return FALSE;

	switch (addr->coding_type) {
	case 0x00:
		ok = stk_tlv_builder_append_bytes(tlv, addr->addr.ieee802, 6);
		break;
	case 0x01:
		ok = stk_tlv_builder_append_bytes(tlv, addr->addr.irda, 4);
		break;
	}

	if (!ok)
		return FALSE;

	return stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.69 */
static gboolean build_dataobj_esn(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const guint32 *esn = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_ESN;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_short(tlv, *esn >> 16) &&
		stk_tlv_builder_append_short(tlv, *esn >> 0) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.72, 3GPP 24.008 Section 9.5.7 */
static gboolean build_dataobj_pdp_context_params(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_common_byte_array *params = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_PDP_ACTIVATION_PARAMETER;

	if (params->len < 1)
		return TRUE;

	if (params->len > 0x7f)
		return FALSE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, params->array, params->len) &&
		stk_tlv_builder_close_container(tlv);
}

/*
 * Described in TS 102.223 Section 8.74
 *
 * See format note in parse_dataobj_imeisv.
 */
static gboolean build_dataobj_imeisv(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	char byte0[3];
	const char *imeisv = data;
	unsigned char value[9];
	unsigned char tag = STK_DATA_OBJECT_TYPE_IMEISV;

	if (imeisv == NULL)
		return TRUE;

	if (strlen(imeisv) != 16)
		return FALSE;

	byte0[0] = '3';
	byte0[1] = imeisv[0];
	byte0[2] = '\0';
	sim_encode_bcd_number(byte0, value);
	sim_encode_bcd_number(imeisv + 1, value + 1);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, value, 9) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.75 */
static gboolean build_dataobj_network_search_mode(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const enum stk_network_search_mode *mode = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_NETWORK_SEARCH_MODE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *mode) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.76 */
static gboolean build_dataobj_battery_state(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const enum stk_battery_state *state = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BATTERY_STATE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *state) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.77 */
static gboolean build_dataobj_browsing_status(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_common_byte_array *bs = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BROWSING_STATUS;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, bs->array, bs->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.79 */
static gboolean build_dataobj_frames_information(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_frames_info *info = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_FRAMES_INFO;
	unsigned int i;

	if (stk_tlv_builder_open_container(tlv, cr, tag, FALSE) != TRUE)
		return FALSE;

	if (stk_tlv_builder_append_byte(tlv, info->id) != TRUE)
		return FALSE;

	for (i = 0; i < info->len; i++) {
		if (stk_tlv_builder_append_byte(tlv,
						info->list[i].height) != TRUE)
			return FALSE;
		if (stk_tlv_builder_append_byte(tlv,
						info->list[i].width) != TRUE)
			return FALSE;
	}

	return stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.81 */
static gboolean build_dataobj_meid(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const char *meid = data;
	unsigned char value[8];
	unsigned char tag = STK_DATA_OBJECT_TYPE_MEID;

	if (meid == NULL)
		return TRUE;

	if (strlen(meid) != 16)
		return FALSE;

	sim_encode_bcd_number(meid, value);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, value, 8) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.83 */
static gboolean build_dataobj_mms_id(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_mms_id *id = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_MMS_ID;

	/* Assume the length is never 0 for a valid ID, however the whole
	 * data object's presence is conditional.  */
	if (id->len == 0)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, id->id, id->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.84 */
static gboolean build_dataobj_mms_transfer_status(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_mms_transfer_status *mts = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_MMS_TRANSFER_STATUS;

	/*
	 * Assume the length is never 0 for a valid Result message, however
	 * the whole data object's presence is conditional.
	 */
	if (mts->len == 0)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, mts->status, mts->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.84 */
static gboolean build_dataobj_i_wlan_access_status(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const enum stk_i_wlan_access_status *status = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_I_WLAN_ACCESS_STATUS;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *status) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.86 */
static gboolean build_dataobj_mms_notification(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_common_byte_array *msg = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_MMS_NOTIFICATION;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, msg->array, msg->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.87 */
static gboolean build_dataobj_last_envelope(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const ofono_bool_t *last = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_LAST_ENVELOPE;

	if (!*last)
		return TRUE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.88 */
static gboolean build_dataobj_registry_application_data(
						struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_registry_application_data *rad = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_REGISTRY_APPLICATION_DATA;
	guint8 dcs, *name;
	gsize len;
	long gsmlen;

	name = convert_utf8_to_gsm(rad->name, -1, NULL, &gsmlen, 0);
	len = gsmlen;
	dcs = 0x04;
	if (name == NULL) {
		name = (guint8 *) g_convert((const gchar *) rad->name, -1,
						"UCS-2BE", "UTF-8//TRANSLIT",
						NULL, &len, NULL);
		dcs = 0x08;

		if (name == NULL)
			return FALSE;
	}

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_short(tlv, rad->port) &&
		stk_tlv_builder_append_byte(tlv, dcs) &&
		stk_tlv_builder_append_byte(tlv, rad->type) &&
		stk_tlv_builder_append_bytes(tlv, name, len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 102.223 Section 8.90 */
static gboolean build_dataobj_broadcast_network_information(
						struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_broadcast_network_information *bni = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BROADCAST_NETWORK_INFO;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, bni->tech) &&
		stk_tlv_builder_append_bytes(tlv, bni->loc_info, bni->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.91 / 3GPP 24.008 Section 10.5.5.15 */
static gboolean build_dataobj_routing_area_id(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_routing_area_info *rai = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_ROUTING_AREA_INFO;
	guint8 mccmnc[3];

	if (rai->mcc[0] == 0)
		return TRUE;

	sim_encode_mcc_mnc(mccmnc, rai->mcc, rai->mnc);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, mccmnc, 3) &&
		stk_tlv_builder_append_short(tlv, rai->lac) &&
		stk_tlv_builder_append_byte(tlv, rai->rac) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.92 */
static gboolean build_dataobj_update_attach_type(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const enum stk_update_attach_type *type = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_UPDATE_ATTACH_TYPE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *type) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.93 */
static gboolean build_dataobj_rejection_cause_code(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const enum stk_rejection_cause_code *cause = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_REJECTION_CAUSE_CODE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, *cause) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.98, 3GPP 24.301 Section 6.5.1 */
static gboolean build_dataobj_eps_pdn_conn_params(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_common_byte_array *params = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_EPS_PDN_CONN_ACTIVATION_REQ;

	if (params->len < 1)
		return TRUE;

	if (params->len > 0x7f)
		return FALSE;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, params->array, params->len) &&
		stk_tlv_builder_close_container(tlv);
}

/* Described in TS 131.111 Section 8.99 / 3GPP 24.301 Section 9.9.3.32 */
static gboolean build_dataobj_tracking_area_id(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_tracking_area_id *tai = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TRACKING_AREA_ID;
	guint8 mccmnc[3];

	if (tai->mcc[0] == 0)
		return TRUE;

	sim_encode_mcc_mnc(mccmnc, tai->mcc, tai->mnc);

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_bytes(tlv, mccmnc, 3) &&
		stk_tlv_builder_append_short(tlv, tai->tac) &&
		stk_tlv_builder_close_container(tlv);
}

static gboolean build_dataobj(struct stk_tlv_builder *tlv,
				dataobj_writer builder_func, ...)
{
	va_list args;

	va_start(args, builder_func);

	while (builder_func) {
		unsigned int flags = va_arg(args, enum stk_data_object_flag);
		const void *data = va_arg(args, const void *);
		gboolean cr = (flags & DATAOBJ_FLAG_CR) ? TRUE : FALSE;

		if (builder_func(tlv, data, cr) != TRUE)
			return FALSE;

		builder_func = va_arg(args, dataobj_writer);
	}

	va_end(args);

	return TRUE;
}

static gboolean build_setup_call(struct stk_tlv_builder *builder,
					const struct stk_response *response)
{
	if (response->set_up_call.modified_result.cc_modified)
		return build_dataobj(builder,
				build_dataobj_cc_requested_action,
				DATAOBJ_FLAG_CR,
				&response->set_up_call.cc_requested_action,
				build_dataobj_result,
				DATAOBJ_FLAG_CR,
				&response->set_up_call.modified_result.result,
				NULL);
	else
		return build_dataobj(builder,
				build_dataobj_cc_requested_action,
				DATAOBJ_FLAG_CR,
				&response->set_up_call.cc_requested_action,
				NULL);
}

static gboolean build_local_info(struct stk_tlv_builder *builder,
					const struct stk_response *response)
{
	const struct stk_response_local_info *info =
		&response->provide_local_info;
	int i;

	switch (response->qualifier) {
	case 0x00: /* Location Information according to current NAA */
		return build_dataobj(builder,
					build_dataobj_location_info,
					DATAOBJ_FLAG_CR, &info->location,
					NULL);

	case 0x01: /* IMEI of the terminal */
		return build_dataobj(builder,
					build_dataobj_imei,
					DATAOBJ_FLAG_CR, info->imei,
					NULL);

	case 0x02: /* Network Measurement results according to current NAA */
		return build_dataobj(builder,
				build_dataobj_network_measurement_results,
				DATAOBJ_FLAG_CR, &info->nmr.nmr,
				build_dataobj_bcch_channel_list,
				DATAOBJ_FLAG_CR, &info->nmr.bcch_ch_list,
				NULL);

	case 0x03: /* Date, time and time zone */
		return build_dataobj(builder,
					build_dataobj_datetime_timezone,
					DATAOBJ_FLAG_CR, &info->datetime,
					NULL);

	case 0x04: /* Language setting */
		return build_dataobj(builder,
					build_dataobj_language,
					DATAOBJ_FLAG_CR, info->language,
					NULL);

	case 0x05: /* Timing Advance */
		return build_dataobj(builder,
					build_dataobj_timing_advance,
					DATAOBJ_FLAG_CR, &info->tadv,
					NULL);

	case 0x06: /* Access Technology (single access technology) */
		return build_dataobj(builder,
					build_dataobj_access_technology,
					0, &info->access_technology,
					NULL);

	case 0x07: /* ESN of the terminal */
		return build_dataobj(builder,
					build_dataobj_esn,
					DATAOBJ_FLAG_CR, &info->esn,
					NULL);

	case 0x08: /* IMEISV of the terminal */
		return build_dataobj(builder,
					build_dataobj_imeisv,
					DATAOBJ_FLAG_CR, info->imeisv,
					NULL);

	case 0x09: /* Search Mode */
		return build_dataobj(builder,
					build_dataobj_network_search_mode,
					0, &info->search_mode,
					NULL);

	case 0x0a: /* Charge State of the Battery */
		return build_dataobj(builder,
					build_dataobj_battery_state,
					DATAOBJ_FLAG_CR, &info->battery_charge,
					NULL);

	case 0x0b: /* MEID of the terminal */
		return build_dataobj(builder,
					build_dataobj_meid,
					0, info->meid,
					NULL);

	case 0x0d: /* Broadcast Network Information according to current tech */
		return build_dataobj(builder,
				build_dataobj_broadcast_network_information,
				0, &info->broadcast_network_info,
				NULL);

	case 0x0e: /* Multiple Access Technologies */
		return build_dataobj(builder,
					build_dataobj_access_technologies,
					0, &info->access_technologies,
					NULL);

	case 0x0f: /* Location Information for multiple NAAs */
		if (build_dataobj(builder,
					build_dataobj_access_technologies,
					0, &info->location_infos.access_techs,
					NULL) != TRUE)
			return FALSE;

		for (i = 0; i < info->location_infos.access_techs.length; i++) {
			dataobj_writer location = build_dataobj_location_info;
			/*
			 * "If no location information is available for an
			 * access technology, the respective data object
			 * shall have length zero."
			 */
			if (info->location_infos.locations[i].mcc[0] == '\0')
				location = build_empty_dataobj_location_info;

			if (build_dataobj(builder,
					location,
					0, &info->location_infos.locations[i],
					NULL) != TRUE)
				return FALSE;
		}

		return TRUE;

	case 0x10: /* Network Measurement results for multiple NAAs */
		if (build_dataobj(builder,
					build_dataobj_access_technologies,
					0, &info->nmrs.access_techs,
					NULL) != TRUE)
			return FALSE;

		for (i = 0; i < info->nmrs.access_techs.length; i++)
			if (build_dataobj(builder,
				build_dataobj_network_measurement_results,
				0, &info->nmrs.nmrs[i].nmr,
				build_dataobj_bcch_channel_list,
				0, &info->nmrs.nmrs[i].bcch_ch_list,
				NULL) != TRUE)
				return FALSE;

		return TRUE;
	}

	return FALSE;
}

static gboolean build_open_channel(struct stk_tlv_builder *builder,
					const struct stk_response *response)
{
	const struct stk_response_open_channel *open_channel =
		&response->open_channel;

	/* insert channel identifier only in case of success */
	if (response->result.type == STK_RESULT_TYPE_SUCCESS) {
		if (build_dataobj(builder, build_dataobj_channel_status,
						0, &open_channel->channel,
						NULL) != TRUE)
			return FALSE;
	}

	return build_dataobj(builder,
				build_dataobj_bearer_description,
				0, &open_channel->bearer_desc,
				build_dataobj_buffer_size,
				0, &open_channel->buf_size,
				NULL);
}

static gboolean build_receive_data(struct stk_tlv_builder *builder,
					const struct stk_response *response)
{
	const struct stk_response_receive_data *receive_data =
		&response->receive_data;

	if (response->result.type != STK_RESULT_TYPE_SUCCESS &&
			response->result.type != STK_RESULT_TYPE_MISSING_INFO)
		return TRUE;

	if (receive_data->rx_data.len) {
		if (build_dataobj(builder, build_dataobj_channel_data,
					DATAOBJ_FLAG_CR,
					&response->receive_data.rx_data,
					NULL) != TRUE)
		return FALSE;
	}

	return build_dataobj(builder, build_dataobj_channel_data_length,
				DATAOBJ_FLAG_CR,
				&response->receive_data.rx_remaining,
				NULL);
}

static gboolean build_send_data(struct stk_tlv_builder *builder,
					const struct stk_response *response)
{
	if (response->result.type != STK_RESULT_TYPE_SUCCESS)
		return TRUE;

	return build_dataobj(builder, build_dataobj_channel_data_length,
				DATAOBJ_FLAG_CR,
				&response->send_data.tx_avail,
				NULL);
}

const unsigned char *stk_pdu_from_response(const struct stk_response *response,
						unsigned int *out_length)
{
	struct stk_tlv_builder builder;
	gboolean ok = TRUE;
	unsigned char tag;
	static unsigned char pdu[512];

	stk_tlv_builder_init(&builder, pdu, sizeof(pdu));

	/*
	 * Encode command details, they come in order with
	 * Command Details TLV first, followed by Device Identities TLV
	 * and the Result TLV.  Comprehension required everywhere.
	 */
	tag = STK_DATA_OBJECT_TYPE_COMMAND_DETAILS;
	if (stk_tlv_builder_open_container(&builder, TRUE, tag, FALSE) == FALSE)
		return NULL;

	if (stk_tlv_builder_append_byte(&builder, response->number) == FALSE)
		return NULL;

	if (stk_tlv_builder_append_byte(&builder, response->type) == FALSE)
		return NULL;

	if (stk_tlv_builder_append_byte(&builder, response->qualifier) == FALSE)
		return NULL;

	if (stk_tlv_builder_close_container(&builder) == FALSE)
		return NULL;

	/*
	 * TS 102 223 section 6.8 states:
	 * "For all COMPREHENSION-TLV objects with Min = N, the terminal
	 * should set the CR flag to comprehension not required."
	 * All the data objects except "Command Details" and "Result" have
	 * Min = N.
	 *
	 * However comprehension required is set for many of the TLVs in
	 * TS 102 384 conformance tests so we set it per command and per
	 * data object type.
	 */
	tag = STK_DATA_OBJECT_TYPE_DEVICE_IDENTITIES;
	if (stk_tlv_builder_open_container(&builder, TRUE, tag, FALSE) == FALSE)
		return NULL;

	if (stk_tlv_builder_append_byte(&builder, response->src) == FALSE)
		return NULL;

	if (stk_tlv_builder_append_byte(&builder, response->dst) == FALSE)
		return NULL;

	if (stk_tlv_builder_close_container(&builder) == FALSE)
		return NULL;

	if (build_dataobj_result(&builder, &response->result, TRUE) != TRUE)
		return NULL;

	switch (response->type) {
	case STK_COMMAND_TYPE_DISPLAY_TEXT:
		break;
	case STK_COMMAND_TYPE_GET_INKEY:
		ok = build_dataobj(&builder,
					build_dataobj_text, DATAOBJ_FLAG_CR,
					&response->get_inkey.text,
					build_dataobj_duration, 0,
					&response->get_inkey.duration,
					NULL);
		break;
	case STK_COMMAND_TYPE_GET_INPUT:
		ok = build_dataobj(&builder,
					build_dataobj_text, DATAOBJ_FLAG_CR,
					&response->get_input.text,
					NULL);
		break;
	case STK_COMMAND_TYPE_MORE_TIME:
	case STK_COMMAND_TYPE_SEND_SMS:
	case STK_COMMAND_TYPE_PLAY_TONE:
		break;
	case STK_COMMAND_TYPE_POLL_INTERVAL:
		ok = build_dataobj(&builder,
					build_dataobj_duration, DATAOBJ_FLAG_CR,
					&response->poll_interval.max_interval,
					NULL);
		break;
	case STK_COMMAND_TYPE_REFRESH:
	case STK_COMMAND_TYPE_SETUP_MENU:
		break;
	case STK_COMMAND_TYPE_SELECT_ITEM:
		ok = build_dataobj(&builder,
					build_dataobj_item_id, DATAOBJ_FLAG_CR,
					&response->select_item.item_id,
					NULL);
		break;
	case STK_COMMAND_TYPE_SEND_SS:
		break;
	case STK_COMMAND_TYPE_SETUP_CALL:
		ok = build_setup_call(&builder, response);
		break;
	case STK_COMMAND_TYPE_POLLING_OFF:
		break;
	case STK_COMMAND_TYPE_PROVIDE_LOCAL_INFO:
		ok = build_local_info(&builder, response);
		break;
	case STK_COMMAND_TYPE_SETUP_EVENT_LIST:
		break;
	case STK_COMMAND_TYPE_TIMER_MANAGEMENT:
		ok = build_dataobj(&builder,
					build_dataobj_timer_id,
					DATAOBJ_FLAG_CR,
					&response->timer_mgmt.id,
					build_dataobj_timer_value,
					DATAOBJ_FLAG_CR,
					&response->timer_mgmt.value,
					NULL);
		break;
	case STK_COMMAND_TYPE_SETUP_IDLE_MODE_TEXT:
		break;
	case STK_COMMAND_TYPE_RUN_AT_COMMAND:
		ok = build_dataobj(&builder,
					build_dataobj_at_response,
					DATAOBJ_FLAG_CR,
					response->run_at_command.at_response,
					NULL);
		break;
	case STK_COMMAND_TYPE_SEND_DTMF:
	case STK_COMMAND_TYPE_LANGUAGE_NOTIFICATION:
	case STK_COMMAND_TYPE_LAUNCH_BROWSER:
	case STK_COMMAND_TYPE_CLOSE_CHANNEL:
		break;
	case STK_COMMAND_TYPE_SEND_USSD:
		ok = build_dataobj(&builder,
					build_dataobj_ussd_text,
					DATAOBJ_FLAG_CR,
					&response->send_ussd.text,
					NULL);
		break;
	case STK_COMMAND_TYPE_OPEN_CHANNEL:
		ok = build_open_channel(&builder, response);
		break;
	case STK_COMMAND_TYPE_RECEIVE_DATA:
		ok = build_receive_data(&builder, response);
		break;
	case STK_COMMAND_TYPE_SEND_DATA:
		ok = build_send_data(&builder, response);
		break;
	case STK_COMMAND_TYPE_GET_CHANNEL_STATUS:
		ok = build_dataobj(&builder,
					build_dataobj_channel_status,
					DATAOBJ_FLAG_CR,
					&response->channel_status.channel,
					NULL);
		break;
	default:
		return NULL;
	};

	if (ok != TRUE)
		return NULL;

	if (out_length)
		*out_length = stk_tlv_builder_get_length(&builder);

	return pdu;
}

/* Described in TS 102.223 Section 8.7 */
static gboolean build_envelope_dataobj_device_ids(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_envelope *envelope = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_DEVICE_IDENTITIES;

	return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
		stk_tlv_builder_append_byte(tlv, envelope->src) &&
		stk_tlv_builder_append_byte(tlv, envelope->dst) &&
		stk_tlv_builder_close_container(tlv);
}

static gboolean build_envelope_call_control(
					struct stk_tlv_builder *builder,
					const struct stk_envelope *envelope)
{
	const struct stk_envelope_call_control *cc = &envelope->call_control;
	gboolean ok = FALSE;

	if (build_dataobj(builder, build_envelope_dataobj_device_ids,
				DATAOBJ_FLAG_CR, envelope, NULL) != TRUE)
		return FALSE;

	switch (cc->type) {
	case STK_CC_TYPE_CALL_SETUP:
		ok = build_dataobj(builder, build_dataobj_address,
					DATAOBJ_FLAG_CR, &cc->address, NULL);
		break;
	case STK_CC_TYPE_SUPPLEMENTARY_SERVICE:
		ok = build_dataobj(builder, build_dataobj_ss_string,
					DATAOBJ_FLAG_CR, &cc->ss_string, NULL);
		break;
	case STK_CC_TYPE_USSD_OP:
		ok = build_dataobj(builder, build_dataobj_ussd_string,
					DATAOBJ_FLAG_CR, &cc->ussd_string,
					NULL);
		break;
	case STK_CC_TYPE_PDP_CTX_ACTIVATION:
		ok = build_dataobj(builder, build_dataobj_pdp_context_params,
					DATAOBJ_FLAG_CR, &cc->pdp_ctx_params,
					NULL);
		break;
	case STK_CC_TYPE_EPS_PDN_CONNECTION_ACTIVATION:
		ok = build_dataobj(builder, build_dataobj_eps_pdn_conn_params,
					DATAOBJ_FLAG_CR, &cc->eps_pdn_params,
					NULL);
		break;
	}

	if (ok != TRUE)
		return FALSE;

	return build_dataobj(builder,
				build_dataobj_ccp, 0, &cc->ccp1,
				build_dataobj_subaddress, 0, &cc->subaddress,
				build_dataobj_location_info, 0, &cc->location,
				build_dataobj_ccp, 0, &cc->ccp2,
				build_dataobj_alpha_id, 0, cc->alpha_id,
				build_dataobj_bc_repeat, 0, &cc->bc_repeat,
				NULL);
}

static gboolean build_envelope_event_download(struct stk_tlv_builder *builder,
					const struct stk_envelope *envelope)
{
	const struct stk_envelope_event_download *evt =
		&envelope->event_download;

	if (build_dataobj(builder,
				build_dataobj_event_type, DATAOBJ_FLAG_CR,
				&evt->type,
				build_envelope_dataobj_device_ids,
				DATAOBJ_FLAG_CR,
				envelope,
				NULL) == FALSE)
		return FALSE;

	switch (evt->type) {
	case STK_EVENT_TYPE_MT_CALL:
		return build_dataobj(builder,
					build_dataobj_transaction_id,
					DATAOBJ_FLAG_CR,
					&evt->mt_call.transaction_id,
					build_dataobj_address, 0,
					&evt->mt_call.caller_address,
					build_dataobj_subaddress, 0,
					&evt->mt_call.caller_subaddress,
					NULL);
	case STK_EVENT_TYPE_CALL_CONNECTED:
		return build_dataobj(builder,
					build_dataobj_transaction_id,
					DATAOBJ_FLAG_CR,
					&evt->call_connected.transaction_id,
					NULL);
	case STK_EVENT_TYPE_CALL_DISCONNECTED:
		return build_dataobj(builder,
					build_dataobj_transaction_ids,
					DATAOBJ_FLAG_CR,
					&evt->call_disconnected.transaction_ids,
					build_dataobj_cause, 0,
					&evt->call_disconnected.cause,
					NULL);
	case STK_EVENT_TYPE_LOCATION_STATUS:
		return build_dataobj(builder,
					build_dataobj_location_status,
					DATAOBJ_FLAG_CR,
					&evt->location_status.state,
					build_dataobj_location_info, 0,
					&evt->location_status.info,
					NULL);
	case STK_EVENT_TYPE_USER_ACTIVITY:
	case STK_EVENT_TYPE_IDLE_SCREEN_AVAILABLE:
		return TRUE;
	case STK_EVENT_TYPE_CARD_READER_STATUS:
		return build_dataobj(builder,
					build_dataobj_card_reader_status,
					DATAOBJ_FLAG_CR,
					&evt->card_reader_status,
					NULL);
	case STK_EVENT_TYPE_LANGUAGE_SELECTION:
		return build_dataobj(builder,
					build_dataobj_language, DATAOBJ_FLAG_CR,
					evt->language_selection,
					NULL);
	case STK_EVENT_TYPE_BROWSER_TERMINATION:
		return build_dataobj(builder,
					build_dataobj_browser_termination_cause,
					DATAOBJ_FLAG_CR,
					&evt->browser_termination.cause,
					NULL);
	case STK_EVENT_TYPE_DATA_AVAILABLE:
		return build_dataobj(builder,
					build_dataobj_channel_status,
					DATAOBJ_FLAG_CR,
					&evt->data_available.channel,
					build_dataobj_channel_data_length,
					DATAOBJ_FLAG_CR,
					&evt->data_available.channel_data_len,
					NULL);
	case STK_EVENT_TYPE_CHANNEL_STATUS:
		return build_dataobj(builder,
					build_dataobj_channel_status,
					DATAOBJ_FLAG_CR,
					&evt->channel_status.channel,
					build_dataobj_bearer_description,
					DATAOBJ_FLAG_CR,
					&evt->channel_status.bearer_desc,
					build_dataobj_other_address,
					DATAOBJ_FLAG_CR,
					&evt->channel_status.address,
					NULL);
	case STK_EVENT_TYPE_SINGLE_ACCESS_TECHNOLOGY_CHANGE:
		return build_dataobj(builder,
					build_dataobj_access_technology,
					DATAOBJ_FLAG_CR,
					&evt->access_technology_change,
					NULL);
	case STK_EVENT_TYPE_DISPLAY_PARAMETERS_CHANGED:
		return build_dataobj(builder,
					build_dataobj_display_parameters,
					DATAOBJ_FLAG_CR,
					&evt->display_params_changed,
					NULL);
	case STK_EVENT_TYPE_LOCAL_CONNECTION:
		return build_dataobj(builder,
					build_dataobj_service_record,
					DATAOBJ_FLAG_CR,
					&evt->local_connection.service_record,
					build_dataobj_remote_entity_address, 0,
					&evt->local_connection.remote_addr,
					build_dataobj_uicc_te_interface, 0,
					&evt->local_connection.transport_level,
					build_dataobj_other_address,
					0,
					&evt->local_connection.transport_addr,
					NULL);
	case STK_EVENT_TYPE_NETWORK_SEARCH_MODE_CHANGE:
		return build_dataobj(builder,
					build_dataobj_network_search_mode,
					DATAOBJ_FLAG_CR,
					&evt->network_search_mode_change,
					NULL);
	case STK_EVENT_TYPE_BROWSING_STATUS:
		return build_dataobj(builder,
					build_dataobj_browsing_status,
					DATAOBJ_FLAG_CR,
					&evt->browsing_status,
					NULL);
	case STK_EVENT_TYPE_FRAMES_INFORMATION_CHANGE:
		return build_dataobj(builder,
					build_dataobj_frames_information,
					DATAOBJ_FLAG_CR,
					&evt->frames_information_change,
					NULL);
	case STK_EVENT_TYPE_I_WLAN_ACCESS_STATUS:
		return build_dataobj(builder,
					build_dataobj_i_wlan_access_status,
					DATAOBJ_FLAG_CR,
					&evt->i_wlan_access_status,
					NULL);
	case STK_EVENT_TYPE_NETWORK_REJECTION:
		return build_dataobj(builder,
					build_dataobj_location_info, 0,
					&evt->network_rejection.location,
					build_dataobj_routing_area_id, 0,
					&evt->network_rejection.rai,
					build_dataobj_tracking_area_id, 0,
					&evt->network_rejection.tai,
					build_dataobj_access_technology,
					DATAOBJ_FLAG_CR,
					&evt->network_rejection.access_tech,
					build_dataobj_update_attach_type,
					DATAOBJ_FLAG_CR,
					&evt->network_rejection.update_attach,
					build_dataobj_rejection_cause_code,
					DATAOBJ_FLAG_CR,
					&evt->network_rejection.cause,
					NULL);
	case STK_EVENT_TYPE_HCI_CONNECTIVITY_EVENT:
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean build_envelope_terminal_apps(struct stk_tlv_builder *builder,
					const struct stk_envelope *envelope)
{
	const struct stk_envelope_terminal_apps *ta = &envelope->terminal_apps;
	int i;

	if (build_dataobj(builder,
				build_envelope_dataobj_device_ids,
				DATAOBJ_FLAG_CR, envelope, NULL) == FALSE)
		return FALSE;

	for (i = 0; i < ta->count; i++)
		if (build_dataobj(builder,
					build_dataobj_registry_application_data,
					0, &ta->list[i], NULL) == FALSE)
			return FALSE;

	return build_dataobj(builder,
				build_dataobj_last_envelope,
				0, &ta->last, NULL);
}

const unsigned char *stk_pdu_from_envelope(const struct stk_envelope *envelope,
						unsigned int *out_length)
{
	struct ber_tlv_builder btlv;
	struct stk_tlv_builder builder;
	gboolean ok = TRUE;
	static unsigned char buffer[512];
	unsigned char *pdu;

	if (ber_tlv_builder_init(&btlv, buffer, sizeof(buffer)) != TRUE)
		return NULL;

	if (stk_tlv_builder_recurse(&builder, &btlv, envelope->type) != TRUE)
		return NULL;

	switch (envelope->type) {
	case STK_ENVELOPE_TYPE_SMS_PP_DOWNLOAD:
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids,
					DATAOBJ_FLAG_CR,
					envelope,
					build_dataobj_address, 0,
					&envelope->sms_pp_download.address,
					build_dataobj_gsm_sms_tpdu,
					DATAOBJ_FLAG_CR,
					&envelope->sms_pp_download.message,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_CBS_PP_DOWNLOAD:
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids,
					DATAOBJ_FLAG_CR,
					envelope,
					build_dataobj_cbs_page,
					DATAOBJ_FLAG_CR,
					&envelope->cbs_pp_download.page,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_MENU_SELECTION:
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids,
					DATAOBJ_FLAG_CR,
					envelope,
					build_dataobj_item_id, DATAOBJ_FLAG_CR,
					&envelope->menu_selection.item_id,
					build_dataobj_help_request, 0,
					&envelope->menu_selection.help_request,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_CALL_CONTROL:
		ok = build_envelope_call_control(&builder, envelope);
		break;
	case STK_ENVELOPE_TYPE_MO_SMS_CONTROL:
		/*
		 * Comprehension Required according to the specs but not
		 * enabled in conformance tests in 3GPP 31.124.
		 */
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids, 0,
					envelope,
					build_dataobj_address, 0,
					&envelope->sms_mo_control.sc_address,
					build_dataobj_address, 0,
					&envelope->sms_mo_control.dest_address,
					build_dataobj_location_info, 0,
					&envelope->sms_mo_control.location,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_EVENT_DOWNLOAD:
		ok = build_envelope_event_download(&builder, envelope);
		break;
	case STK_ENVELOPE_TYPE_TIMER_EXPIRATION:
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids,
					DATAOBJ_FLAG_CR,
					envelope,
					build_dataobj_timer_id,
					DATAOBJ_FLAG_CR,
					&envelope->timer_expiration.id,
					build_dataobj_timer_value,
					DATAOBJ_FLAG_CR,
					&envelope->timer_expiration.value,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_USSD_DOWNLOAD:
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids,
					DATAOBJ_FLAG_CR,
					envelope,
					build_dataobj_ussd_string,
					DATAOBJ_FLAG_CR,
					&envelope->ussd_data_download.string,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_MMS_TRANSFER_STATUS:
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids,
					DATAOBJ_FLAG_CR,
					envelope,
					build_dataobj_file, DATAOBJ_FLAG_CR,
					&envelope->mms_status.transfer_file,
					build_dataobj_mms_id, 0,
					&envelope->mms_status.id,
					build_dataobj_mms_transfer_status, 0,
					&envelope->mms_status.transfer_status,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_MMS_NOTIFICATION:
		ok = build_dataobj(&builder,
					build_envelope_dataobj_device_ids,
					DATAOBJ_FLAG_CR,
					envelope,
					build_dataobj_mms_notification,
					DATAOBJ_FLAG_CR,
					&envelope->mms_notification.msg,
					build_dataobj_last_envelope, 0,
					&envelope->mms_notification.last,
					NULL);
		break;
	case STK_ENVELOPE_TYPE_TERMINAL_APP:
		ok = build_envelope_terminal_apps(&builder, envelope);
		break;
	default:
		return NULL;
	};

	if (ok != TRUE)
		return NULL;

	ber_tlv_builder_optimize(&btlv, &pdu, out_length);

	return pdu;
}

static const char *html_colors[] = {
	"#000000", /* Black */
	"#808080", /* Dark Grey */
	"#C11B17", /* Dark Red */
	"#FBB117", /* Dark Yellow */
	"#347235", /* Dark Green */
	"#307D7E", /* Dark Cyan */
	"#0000A0", /* Dark Blue */
	"#C031C7", /* Dark Magenta */
	"#C0C0C0", /* Grey */
	"#FFFFFF", /* White */
	"#FF0000", /* Bright Red */
	"#FFFF00", /* Bright Yellow */
	"#00FF00", /* Bright Green */
	"#00FFFF", /* Bright Cyan */
	"#0000FF", /* Bright Blue */
	"#FF00FF", /* Bright Magenta */
};

#define STK_TEXT_FORMAT_ALIGN_MASK 0x03
#define STK_TEXT_FORMAT_FONT_MASK 0x0C
#define STK_TEXT_FORMAT_STYLE_MASK 0xF0
#define STK_DEFAULT_TEXT_ALIGNMENT 0x00
#define STK_TEXT_FORMAT_INIT 0x9003

/* Defined in ETSI 123 40 9.2.3.24.10.1.1 */
enum stk_text_format_code {
	STK_TEXT_FORMAT_LEFT_ALIGN = 0x00,
	STK_TEXT_FORMAT_CENTER_ALIGN = 0x01,
	STK_TEXT_FORMAT_RIGHT_ALIGN = 0x02,
	STK_TEXT_FORMAT_NO_ALIGN = 0x03,
	STK_TEXT_FORMAT_FONT_SIZE_LARGE = 0x04,
	STK_TEXT_FORMAT_FONT_SIZE_SMALL = 0x08,
	STK_TEXT_FORMAT_FONT_SIZE_RESERVED = 0x0c,
	STK_TEXT_FORMAT_STYLE_BOLD = 0x10,
	STK_TEXT_FORMAT_STYLE_ITALIC = 0x20,
	STK_TEXT_FORMAT_STYLE_UNDERLINED = 0x40,
	STK_TEXT_FORMAT_STYLE_STRIKETHROUGH = 0x80,
};

static void end_format(GString *string, guint16 attr)
{
	guint code = attr & 0xFF;
	guint color = (attr >> 8) & 0xFF;

	if ((code & ~STK_TEXT_FORMAT_ALIGN_MASK) || color)
		g_string_append(string, "</span>");

	if ((code & STK_TEXT_FORMAT_ALIGN_MASK) != STK_TEXT_FORMAT_NO_ALIGN)
		g_string_append(string, "</div>");
}

static void start_format(GString *string, guint16 attr)
{
	guint8 code = attr & 0xFF;
	guint8 color = (attr >> 8) & 0xFF;
	guint8 align = code & STK_TEXT_FORMAT_ALIGN_MASK;
	guint8 font = code & STK_TEXT_FORMAT_FONT_MASK;
	guint8 style = code & STK_TEXT_FORMAT_STYLE_MASK;
	int fg = color & 0x0f;
	int bg = (color >> 4) & 0x0f;

	/* align formatting applies to a block of text */
	if (align != STK_TEXT_FORMAT_NO_ALIGN)
		g_string_append(string, "<div style=\"");

	switch (align) {
	case STK_TEXT_FORMAT_RIGHT_ALIGN:
		g_string_append(string, "text-align: right;\">");
		break;
	case STK_TEXT_FORMAT_CENTER_ALIGN:
		g_string_append(string, "text-align: center;\">");
		break;
	case STK_TEXT_FORMAT_LEFT_ALIGN:
		g_string_append(string, "text-align: left;\">");
		break;
	}

	if ((font == 0) && (style == 0) && (color == 0))
		return;

	/* font, style, and color are inline */
	g_string_append(string, "<span style=\"");

	switch (font) {
	case STK_TEXT_FORMAT_FONT_SIZE_LARGE:
		g_string_append(string, "font-size: big;");
		break;
	case STK_TEXT_FORMAT_FONT_SIZE_SMALL:
		g_string_append(string, "font-size: small;");
		break;
	}

	if (style & STK_TEXT_FORMAT_STYLE_BOLD)
		g_string_append(string, "font-weight: bold;");
	if (style & STK_TEXT_FORMAT_STYLE_ITALIC)
		g_string_append(string, "font-style: italic;");
	if (style & STK_TEXT_FORMAT_STYLE_UNDERLINED)
		g_string_append(string, "text-decoration: underline;");
	if (style & STK_TEXT_FORMAT_STYLE_STRIKETHROUGH)
		g_string_append(string, "text-decoration: line-through;");

	/* add any color */
	g_string_append_printf(string, "color: %s;", html_colors[fg]);
	g_string_append_printf(string, "background-color: %s;",
						html_colors[bg]);
	g_string_append(string, "\">");
}

char *stk_text_to_html(const char *utf8,
				const unsigned short *attrs, int num_attrs)
{
	long text_len = g_utf8_strlen(utf8, -1);
	GString *string = g_string_sized_new(strlen(utf8) + 1);
	short *formats;
	int pos, i, j;
	guint16 start, end, len, attr, prev_attr;
	guint8 code, color, align;
	const char *text = utf8;
	int attrs_len = num_attrs * 4;

	formats = g_try_new0(gint16, (text_len + 1));
	if (formats == NULL) {
		g_string_free(string, TRUE);
		return NULL;
	}

	/* we will need formatting at the position beyond the last char */
	for (i = 0; i <= text_len; i++)
		formats[i] = STK_TEXT_FORMAT_INIT;

	for (i = 0; i < attrs_len; i += 4) {
		start = attrs[i];
		len = attrs[i + 1];
		code = attrs[i + 2] & 0xFF;
		color = attrs[i + 3] & 0xFF;

		if (len == 0)
			end = text_len;
		else
			end = start + len;

		/* sanity check values */
		if (start > end || end > text_len)
			continue;

		/*
		 * if the alignment is the same as either the default
		 * or the last alignment used, don't set any alignment
		 * value.
		 */
		if (start == 0)
			align = STK_TEXT_FORMAT_NO_ALIGN;
		else {
			align = formats[start - 1] &
					STK_TEXT_FORMAT_ALIGN_MASK;
		}

		if ((code & STK_TEXT_FORMAT_ALIGN_MASK) == align)
			code |= STK_TEXT_FORMAT_NO_ALIGN;

		attr = code | (color << 8);

		for (j = start; j < end; j++)
			formats[j] = attr;
	}

	prev_attr = STK_TEXT_FORMAT_INIT;

	for (pos = 0; pos <= text_len; pos++) {
		attr = formats[pos];
		if (attr != prev_attr) {
			if (prev_attr != STK_TEXT_FORMAT_INIT)
				end_format(string, prev_attr);

			if (attr != STK_TEXT_FORMAT_INIT)
				start_format(string, attr);

			prev_attr = attr;
		}

		if (pos == text_len)
			break;

		switch (g_utf8_get_char(text)) {
		case '\n':
			g_string_append(string, "<br/>");
			break;
		case '\r':
		{
			char *next = g_utf8_next_char(text);
			gunichar c = g_utf8_get_char(next);

			g_string_append(string, "<br/>");

			if ((pos + 1 < text_len) && (c == '\n')) {
				text = g_utf8_next_char(text);
				pos++;
			}
			break;
		}
		case '<':
			g_string_append(string, "&lt;");
			break;
		case '>':
			g_string_append(string, "&gt;");
			break;
		case '&':
			g_string_append(string, "&amp;");
			break;
		default:
			g_string_append_unichar(string, g_utf8_get_char(text));
		}

		text = g_utf8_next_char(text);
	}

	g_free(formats);

	/* return characters from string. Caller must free char data */
	return g_string_free(string, FALSE);
}

static const char chars_table[] = {
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C',
	'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
	'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c',
	'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
	'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '+', '.' };

char *stk_image_to_xpm(const unsigned char *img, unsigned int len,
			enum stk_img_scheme scheme, const unsigned char *clut,
			unsigned short clut_len)
{
	guint8 width, height;
	unsigned int ncolors, nbits, entry, cpp;
	unsigned int i, j;
	int bit, k;
	GString *xpm;
	unsigned int pos = 0;
	const char xpm_header[] = "/* XPM */\n";
	const char declaration[] = "static char *xpm[] = {\n";
	char c[3];

	if (img == NULL)
		return NULL;

	/* sanity check length */
	if (len < 3)
		return NULL;

	width = img[pos++];
	height = img[pos++];

	if (scheme == STK_IMG_SCHEME_BASIC) {
		nbits = 1;
		ncolors = 2;
	} else {
		/* sanity check length */
		if ((pos + 4 > len) || (clut == NULL))
			return NULL;

		nbits = img[pos++];
		ncolors = img[pos++];

		/* the value of zero should be interpreted as 256 */
		if (ncolors == 0)
			ncolors = 256;

		/* skip clut offset bytes */
		pos += 2;

		if ((ncolors * 3) > clut_len)
			return NULL;
	}

	if (pos + ((width * height + 7) / 8) > len)
		return NULL;

	/* determine the number of chars need to represent the pixel */
	cpp = ncolors > 64 ? 2 : 1;

	/*
	 * space needed:
	 *	header line
	 *	declaration and beginning of assignment line
	 *	values - max length of 19
	 *	colors - ncolors * (cpp + whitespace + deliminators + color)
	 *	pixels - width * height * cpp + height deliminators "",\n
	 *	end of assignment - 2 chars "};"
	 */
	xpm = g_string_sized_new(strlen(xpm_header) + strlen(declaration) +
				19 + ((cpp + 14) * ncolors) +
				(width * height * cpp) + (4 * height) + 2);
	if (xpm == NULL)
		return NULL;

	/* add header, declaration, values */
	g_string_append(xpm, xpm_header);
	g_string_append(xpm, declaration);
	g_string_append_printf(xpm, "\"%d %d %d %d\",\n", width, height,
				ncolors, cpp);

	/* create colors */
	if (scheme == STK_IMG_SCHEME_BASIC) {
		g_string_append(xpm, "\"0\tc #000000\",\n");
		g_string_append(xpm, "\"1\tc #FFFFFF\",\n");
	} else {
		for (i = 0; i < ncolors; i++) {
			/* lookup char representation of this number */
			if (ncolors > 64) {
				c[0] = chars_table[i / 64];
				c[1] = chars_table[i % 64];
				c[2] = '\0';
			} else {
				c[0] = chars_table[i % 64];
				c[1] = '\0';
			}

			if ((i == (ncolors - 1)) &&
					scheme == STK_IMG_SCHEME_TRANSPARENCY)
				g_string_append_printf(xpm,
					"\"%s\tc None\",\n", c);
			else
				g_string_append_printf(xpm,
					"\"%s\tc #%02hhX%02hhX%02hhX\",\n",
					c, clut[0], clut[1], clut[2]);
			clut += 3;
		}
	}

	/* height rows of width pixels */
	k = 7;
	for (i = 0; i < height; i++) {
		g_string_append(xpm, "\"");
		for (j = 0; j < width; j++) {
			entry = 0;
			for (bit = nbits - 1; bit >= 0; bit--) {
				entry |= (img[pos] >> k & 0x1) << bit;
				k--;

				/* see if we crossed a byte boundary */
				if (k < 0) {
					k = 7;
					pos++;
				}
			}

			/* lookup char representation of this number */
			if (ncolors > 64) {
				c[0] = chars_table[entry / 64];
				c[1] = chars_table[entry % 64];
				c[2] = '\0';
			} else {
				c[0] = chars_table[entry % 64];
				c[1] = '\0';
			}

			g_string_append_printf(xpm, "%s", c);
		}

		g_string_append(xpm, "\",\n");
	}

	g_string_append(xpm, "};");

	/* Caller must free char data */
	return g_string_free(xpm, FALSE);
}
