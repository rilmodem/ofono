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

static char *decode_text(unsigned char dcs, int len, const unsigned char *data)
{
	char *utf8;

	switch (dcs) {
	case 0x00:
	{
		long written;
		unsigned long max_to_unpack = len * 8 / 7;
		unsigned char *unpacked = unpack_7bit(data, len, 0, FALSE,
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
		utf8 = convert_gsm_to_utf8(data, len, NULL, NULL, 0);
		break;
	case 0x08:
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
	if (len == 0)
		return TRUE;

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

/* Defined in TS 102.223 Section 8.15 */
static gboolean parse_dataobj_text(struct comprehension_tlv_iter *iter,
					void *user)
{
	char **text = user;
	unsigned int len = comprehension_tlv_iter_get_length(iter);
	const unsigned char *data = comprehension_tlv_iter_get_data(iter);
	char *utf8;

	/* DCS followed by some text, cannot be 1 */
	if (len == 1)
		return FALSE;

	if (len == 0) {
		*text = NULL;
		return TRUE;
	}

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
	g_slist_foreach(*fl, (GFunc)g_free, NULL);
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
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
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

/* Defined in 102.223 Section 8.47 */
static gboolean parse_dataobj_browser_id(struct comprehension_tlv_iter *iter,
						void *user)
{
	unsigned char *byte = user;
	return parse_dataobj_common_byte(iter, byte);
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
	bd->len = len - 1;
	memcpy(bd->pars, data + 1, bd->len);

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

	if (len == 0)
		return TRUE;

	if ((len != 5) && (len != 17))
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	oa->type = data[0];
	memcpy(&oa->addr, data + 1, len - 1);

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
		struct comprehension_tlv_iter *iter, void *user)
{
	struct stk_network_access_name *nan = user;
	const unsigned char *data;
	unsigned int len = comprehension_tlv_iter_get_length(iter);

	if (len == 0)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);
	nan->len = len;
	memcpy(nan->name, data, len);

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

	if (len < 1)
		return FALSE;

	data = comprehension_tlv_iter_get_data(iter);

	if (data[0] > 0x0f)
		return FALSE;

	if ((len == 1 && data[0] != 0) || (len > 1 && data[0] == 0))
		return FALSE;

	if (len == 1)
		return TRUE;

	fi->id = data[0];
	fi->len = len - 1;
	memcpy(fi->list, data + 1, fi->len);

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
		return parse_dataobj_text;
	case STK_DATA_OBJECT_TYPE_TONE:
		return parse_dataobj_tone;
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
	case STK_DATA_OBJECT_TYPE_ITEM_TEXT_ATTRIBUTE_LIST:
		return parse_dataobj_item_text_attribute_list;
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

static gboolean parse_item_list(struct comprehension_tlv_iter *iter,
				void *data)
{
	GSList **out = data;
	unsigned short tag = STK_DATA_OBJECT_TYPE_ITEM;
	struct comprehension_tlv_iter iter_old;
	struct stk_item item;
	GSList *list = NULL;

	do {
		comprehension_tlv_iter_copy(iter, &iter_old);
		memset(&item, 0, sizeof(item));

		if (parse_dataobj_item(iter, &item) == TRUE)
			list = g_slist_prepend(list,
						g_memdup(&item, sizeof(item)));
	} while (comprehension_tlv_iter_next(iter) == TRUE &&
			comprehension_tlv_iter_get_tag(iter) == tag);

	comprehension_tlv_iter_copy(&iter_old, iter);
	*out = g_slist_reverse(list);

	return TRUE;
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

	if (comprehension_tlv_iter_next(iter) != TRUE)
		goto out;

	entries = g_slist_reverse(entries);

	for (l = entries; l; l = l->next) {
		dataobj_handler handler;
		struct dataobj_handler_entry *entry = l->data;

		if (entry->flags & DATAOBJ_FLAG_LIST)
			handler = list_handler_for_type(entry->type);
		else
			handler = handler_for_type(entry->type);

		if (handler == NULL)
			continue;

		if (comprehension_tlv_iter_get_tag(iter) == entry->type) {
			if (handler(iter, entry->data))
				entry->parsed = TRUE;

			if (comprehension_tlv_iter_next(iter) == FALSE)
				break;
		}
	}

out:
	for (l = entries; l; l = l->next) {
		struct dataobj_handler_entry *entry = l->data;

		if ((entry->flags & DATAOBJ_FLAG_MINIMUM) &&
				entry->parsed == FALSE)
			minimum_set = FALSE;
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
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	command->destructor = destroy_display_text;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_get_inkey(struct stk_command *command)
{
	g_free(command->get_inkey.text);
}

static gboolean parse_get_inkey(struct stk_command *command,
				struct comprehension_tlv_iter *iter)
{
	struct stk_command_get_inkey *obj = &command->get_inkey;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
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

	command->destructor = destroy_get_inkey;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_get_input(struct stk_command *command)
{
	g_free(command->get_input.text);
	g_free(command->get_input.default_text);
}

static gboolean parse_get_input(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_get_input *obj = &command->get_input;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
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

	command->destructor = destroy_get_input;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static gboolean parse_more_time(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	return TRUE;
}

static void destroy_play_tone(struct stk_command *command)
{
	g_free(command->play_tone.alpha_id);
}

static gboolean parse_play_tone(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_play_tone *obj = &command->play_tone;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_EARPIECE)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
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

	command->destructor = destroy_play_tone;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static gboolean parse_poll_interval(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_poll_interval *obj = &command->poll_interval;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_DURATION,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->duration,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_stk_item(struct stk_item *item)
{
	g_free(item->text);
	g_free(item);
}

static void destroy_setup_menu(struct stk_command *command)
{
	g_free(command->setup_menu.alpha_id);
	g_slist_foreach(command->setup_menu.items,
				(GFunc)destroy_stk_item, NULL);
	g_slist_free(command->setup_menu.items);
}

static gboolean parse_setup_menu(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_menu *obj = &command->setup_menu;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	command->destructor = destroy_setup_menu;

	ret = parse_dataobj(iter,
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

	if (ret == FALSE)
		return FALSE;

	if (obj->items == NULL)
		return FALSE;

	return TRUE;
}

static void destroy_select_item(struct stk_command *command)
{
	g_free(command->select_item.alpha_id);
	g_slist_foreach(command->select_item.items,
				(GFunc)destroy_stk_item, NULL);
	g_slist_free(command->select_item.items);
}

static gboolean parse_select_item(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_select_item *obj = &command->select_item;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	command->destructor = destroy_select_item;

	ret = parse_dataobj(iter,
			STK_DATA_OBJECT_TYPE_ALPHA_ID,
			DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
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

	if (ret == FALSE)
		return FALSE;

	if (obj->items == NULL)
		return FALSE;

	return TRUE;
}

static void destroy_send_sms(struct stk_command *command)
{
	g_free(command->send_sms.alpha_id);
	g_free(command->send_sms.address.number);
	g_free(command->send_sms.cdma_sms.array);
}

static gboolean parse_send_sms(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_sms *obj = &command->send_sms;
	struct gsm_sms_tpdu gsm_tpdu;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return FALSE;

	memset(&gsm_tpdu, 0, sizeof(gsm_tpdu));
	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
				&obj->alpha_id,
				STK_DATA_OBJECT_TYPE_ADDRESS, 0,
				&obj->address,
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

	if (ret == FALSE)
		return FALSE;

	if (gsm_tpdu.len == 0 && obj->cdma_sms.len == 0)
		return FALSE;

	if (gsm_tpdu.len > 0 && obj->cdma_sms.len > 0)
		return FALSE;

	/* We don't process CDMA pdus for now */
	if (obj->cdma_sms.len > 0)
		return TRUE;

	/* packing is needed */
	if (command->qualifier & 0x01)
		return sms_decode_unpacked_stk_pdu(gsm_tpdu.tpdu, gsm_tpdu.len,
							&obj->gsm_sms);

	if (sms_decode(gsm_tpdu.tpdu, gsm_tpdu.len, TRUE,
				gsm_tpdu.len, &obj->gsm_sms) == FALSE)
		return FALSE;

	if (obj->gsm_sms.type != SMS_TYPE_SUBMIT &&
			obj->gsm_sms.type != SMS_TYPE_COMMAND)
		return FALSE;

	return TRUE;
}

static void destroy_setup_call(struct stk_command *command)
{
	g_free(command->setup_call.alpha_id_usr_cfm);
	g_free(command->setup_call.addr.number);
	g_free(command->setup_call.alpha_id_call_setup);
}

static gboolean parse_setup_call(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_call *obj = &command->setup_call;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
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

	command->destructor = destroy_setup_call;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_refresh(struct stk_command *command)
{
	g_slist_foreach(command->refresh.file_list, (GFunc)g_free, NULL);
	g_slist_free(command->refresh.file_list);
	g_free(command->refresh.alpha_id);
}

static gboolean parse_refresh(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_refresh *obj = &command->refresh;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_FILE_LIST, 0,
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

	command->destructor = destroy_refresh;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static gboolean parse_polling_off(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	return TRUE;
}

static gboolean parse_provide_local_info(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	return TRUE;
}

static gboolean parse_setup_event_list(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_event_list *obj = &command->setup_event_list;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_EVENT_LIST,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->event_list,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static gboolean parse_perform_card_apdu(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_perform_card_apdu *obj = &command->perform_card_apdu;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_C_APDU,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->c_apdu,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static gboolean parse_power_off_card(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
		return FALSE;

	return TRUE;
}

static gboolean parse_power_on_card(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
			(command->dst > STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
		return FALSE;

	return TRUE;
}

static gboolean parse_get_reader_status(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	switch (command->qualifier) {
	case STK_QUALIFIER_TYPE_CARD_READER_STATUS:
		if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
			return FALSE;
		break;
	case STK_QUALIFIER_TYPE_CARD_READER_ID:
		if ((command->dst < STK_DEVICE_IDENTITY_TYPE_CARD_READER_0) ||
				(command->dst >
					STK_DEVICE_IDENTITY_TYPE_CARD_READER_7))
			return FALSE;
		break;
	default:
		return FALSE;
	}

	return TRUE;
}

static gboolean parse_timer_mgmt(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_timer_mgmt *obj = &command->timer_mgmt;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TIMER_ID,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->timer_id,
				STK_DATA_OBJECT_TYPE_TIMER_VALUE, 0,
				&obj->timer_value,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_setup_idle_mode_text(struct stk_command *command)
{
	g_free(command->setup_idle_mode_text.text);
}

static gboolean parse_setup_idle_mode_text(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_setup_idle_mode_text *obj =
					&command->setup_idle_mode_text;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_TEXT,
				DATAOBJ_FLAG_MANDATORY | DATAOBJ_FLAG_MINIMUM,
				&obj->text,
				STK_DATA_OBJECT_TYPE_ICON_ID, 0,
				&obj->icon_id,
				STK_DATA_OBJECT_TYPE_TEXT_ATTRIBUTE, 0,
				&obj->text_attr,
				STK_DATA_OBJECT_TYPE_FRAME_ID, 0,
				&obj->frame_id,
				STK_DATA_OBJECT_TYPE_INVALID);

	command->destructor = destroy_setup_idle_mode_text;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_run_at_command(struct stk_command *command)
{
	g_free(command->run_at_command.alpha_id);
	g_free(command->run_at_command.at_command);
}

static gboolean parse_run_at_command(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_run_at_command *obj = &command->run_at_command;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
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

	command->destructor = destroy_run_at_command;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_send_dtmf(struct stk_command *command)
{
	g_free(command->send_dtmf.alpha_id);
	g_free(command->send_dtmf.dtmf);
}

static gboolean parse_send_dtmf(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_send_dtmf *obj = &command->send_dtmf;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_NETWORK)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_ALPHA_ID, 0,
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

	command->destructor = destroy_send_dtmf;

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static gboolean parse_language_notification(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_language_notification *obj =
					&command->language_notification;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter, STK_DATA_OBJECT_TYPE_LANGUAGE, 0,
				&obj->language,
				STK_DATA_OBJECT_TYPE_INVALID);

	if (ret == FALSE)
		return FALSE;

	return TRUE;
}

static void destroy_launch_browser(struct stk_command *command)
{
	g_free(command->launch_browser.url);
	g_free(command->launch_browser.bearer.array);
	g_slist_foreach(command->launch_browser.prov_file_refs,
				(GFunc)g_free, NULL);
	g_slist_free(command->launch_browser.prov_file_refs);
	g_free(command->launch_browser.text_gateway_proxy_id);
	g_free(command->launch_browser.alpha_id);
	g_free(command->launch_browser.network_name.array);
	g_free(command->launch_browser.text_usr);
	g_free(command->launch_browser.text_passwd);
}

static gboolean parse_launch_browser(struct stk_command *command,
					struct comprehension_tlv_iter *iter)
{
	struct stk_command_launch_browser *obj = &command->launch_browser;
	gboolean ret;

	if (command->src != STK_DEVICE_IDENTITY_TYPE_UICC)
		return FALSE;

	if (command->dst != STK_DEVICE_IDENTITY_TYPE_TERMINAL)
		return FALSE;

	ret = parse_dataobj(iter,
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

	command->destructor = destroy_launch_browser;

	if (ret == FALSE)
		return FALSE;

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
	case STK_COMMAND_TYPE_MORE_TIME:
		ok = parse_more_time(command, &iter);
		break;
	case STK_COMMAND_TYPE_PLAY_TONE:
		ok = parse_play_tone(command, &iter);
		break;
	case STK_COMMAND_TYPE_POLL_INTERVAL:
		ok = parse_poll_interval(command, &iter);
		break;
	case STK_COMMAND_TYPE_SETUP_MENU:
		ok = parse_setup_menu(command, &iter);
		break;
	case STK_COMMAND_TYPE_SELECT_ITEM:
		ok = parse_select_item(command, &iter);
		break;
	case STK_COMMAND_TYPE_SEND_SMS:
		ok = parse_send_sms(command, &iter);
		break;
	case STK_COMMAND_TYPE_SETUP_CALL:
		ok = parse_setup_call(command, &iter);
		break;
	case STK_COMMAND_TYPE_REFRESH:
		ok = parse_refresh(command, &iter);
		break;
	case STK_COMMAND_TYPE_POLLING_OFF:
		ok = parse_polling_off(command, &iter);
		break;
	case STK_COMMAND_TYPE_PROVIDE_LOCAL_INFO:
		ok = parse_provide_local_info(command, &iter);
		break;
	case STK_COMMAND_TYPE_SETUP_EVENT_LIST:
		ok = parse_setup_event_list(command, &iter);
		break;
	case STK_COMMAND_TYPE_PERFORM_CARD_APDU:
		ok = parse_perform_card_apdu(command, &iter);
		break;
	case STK_COMMAND_TYPE_POWER_OFF_CARD:
		ok = parse_power_off_card(command, &iter);
		break;
	case STK_COMMAND_TYPE_POWER_ON_CARD:
		ok = parse_power_on_card(command, &iter);
		break;
	case STK_COMMAND_TYPE_GET_READER_STATUS:
		ok = parse_get_reader_status(command, &iter);
		break;
	case STK_COMMAND_TYPE_TIMER_MANAGEMENT:
		ok = parse_timer_mgmt(command, &iter);
		break;
	case STK_COMMAND_TYPE_SETUP_IDLE_MODE_TEXT:
		ok = parse_setup_idle_mode_text(command, &iter);
		break;
	case STK_COMMAND_TYPE_RUN_AT_COMMAND:
		ok = parse_run_at_command(command, &iter);
		break;
	case STK_COMMAND_TYPE_SEND_DTMF:
		ok = parse_send_dtmf(command, &iter);
		break;
	case STK_COMMAND_TYPE_LANGUAGE_NOTIFICATION:
		ok = parse_language_notification(command, &iter);
		break;
	case STK_COMMAND_TYPE_LAUNCH_BROWSER:
		ok = parse_launch_browser(command, &iter);
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

static gboolean stk_tlv_builder_init(struct stk_tlv_builder *iter,
						unsigned char *pdu,
						unsigned int size)
{
	iter->value = NULL;
	iter->len = 0;

	return comprehension_tlv_builder_init(&iter->ctlv, pdu, size);
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

/* Defined in TS 102.223 Section 8.15 */
static gboolean build_dataobj_text(struct stk_tlv_builder *tlv,
					const void *data, gboolean cr)
{
	const struct stk_answer_text *text = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_TEXT;
	gboolean ret;

	if (!text->text && !text->yesno)
		return TRUE;

	if (stk_tlv_builder_open_container(tlv, cr, tag, TRUE) != TRUE)
		return FALSE;

	if (text->yesno == TRUE) {
		/* Section 6.8.5:
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

/* Described in TS 102.223 Section 8.19 */
static gboolean build_dataobj_location_info(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_location_info *li = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_LOCATION_INFO;
	guint8 mccmnc[3];

	if (li->mcc[0] == 0)
		/*
		 * "If no location information is available for an access
		 * technology, the respective data object shall have
		 * length zero."
		 */
		return stk_tlv_builder_open_container(tlv, cr, tag, FALSE) &&
			stk_tlv_builder_close_container(tlv);

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

/* Described in TS 102.223 Section 8.20
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

/* Described in 3GPP 31.111 Section 8.29 */
static gboolean build_dataobj_bcch_channel_list(struct stk_tlv_builder *tlv,
						const void *data, gboolean cr)
{
	const struct stk_bcch_ch_list *list = data;
	unsigned char tag = STK_DATA_OBJECT_TYPE_BCCH_CHANNEL_LIST;
	int i, bytes, pos, shift;
	unsigned char value;

	/* To distinguish between no BCCH Channel List data object and
	 * an empty object in a sequence of empty and non-empty objects,
	 * .channels must be non-NULL in objects in sequences.  */
	if (list->channels == NULL)
		return TRUE;

	if (stk_tlv_builder_open_container(tlv, cr, tag, TRUE) != TRUE)
		return FALSE;

	bytes = (list->length * 10 + 7) / 8;
	for (i = 0; i < bytes; i++) {
		pos = (i * 8 + 7) / 10;
		shift = pos * 10 + 10 - i * 8 - 8;

		value = 0;
		if (pos < list->length)
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
	struct sms_scts timestamp;
	unsigned char value[7];
	int offset = 0;
	unsigned char tag = STK_DATA_OBJECT_TYPE_DATETIME_TIMEZONE;

	if (scts->month == 0 && scts->day == 0)
		return TRUE;

	/* Time zone information is optional */
	if (scts->timezone == (gint8) 0xff) {
		memcpy(&timestamp, scts, sizeof(timestamp));
		timestamp.timezone = 0;
		if (sms_encode_scts(&timestamp, value, &offset) != TRUE)
			return FALSE;
		value[6] = 0xff;
	} else
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

	/* "If the AT Response string is longer than the maximum length
	 * capable of being transmitted to the UICC then the AT Response
	 * string shall be truncated to this length by the terminal." */
	len = strlen(data);
	if (len > 240) /* Safe pick */
		len = 240;

	return stk_tlv_builder_open_container(tlv, cr, tag, TRUE) &&
		stk_tlv_builder_append_bytes(tlv, data, len) &&
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

/* Described in TS 102.223 Section 8.74
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

		for (i = 0; i < info->location_infos.access_techs.length; i++)
			if (build_dataobj(builder,
					build_dataobj_location_info,
					0, &info->location_infos.locations[i],
					NULL) != TRUE)
				return FALSE;

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

unsigned int stk_pdu_from_response(const struct stk_response *response,
					unsigned char *pdu, unsigned int size)
{
	struct stk_tlv_builder builder;
	gboolean ok = TRUE;
	unsigned char tag;

	stk_tlv_builder_init(&builder, pdu, size);

	/*
	 * Encode command details, they come in order with
	 * Command Details TLV first, followed by Device Identities TLV
	 * and the Result TLV.  Comprehension required everywhere.
	 */
	tag = STK_DATA_OBJECT_TYPE_COMMAND_DETAILS;
	if (stk_tlv_builder_open_container(&builder, TRUE, tag, FALSE) == FALSE)
		return 0;

	if (stk_tlv_builder_append_byte(&builder, response->number) == FALSE)
		return 0;

	if (stk_tlv_builder_append_byte(&builder, response->type) == FALSE)
		return 0;

	if (stk_tlv_builder_append_byte(&builder, response->qualifier) == FALSE)
		return 0;

	if (stk_tlv_builder_close_container(&builder) == FALSE)
		return 0;

	/* TS 102 223 section 6.8 states:
	 * "For all COMPREHENSION-TLV objects with Min = N, the terminal
	 * should set the CR flag to comprehension not required."
	 * All the data objects except "Command Details" and "Result" have
	 * Min = N.
	 *
	 * However comprehension required is set for many of the TLVs in
	 * TS 102 384 conformace tests so we set it per command and per
	 * data object type.
	 */
	tag = STK_DATA_OBJECT_TYPE_DEVICE_IDENTITIES;
	if (stk_tlv_builder_open_container(&builder, TRUE, tag, FALSE) == FALSE)
		return 0;

	if (stk_tlv_builder_append_byte(&builder, response->src) == FALSE)
		return 0;

	if (stk_tlv_builder_append_byte(&builder, response->dst) == FALSE)
		return 0;

	if (stk_tlv_builder_close_container(&builder) == FALSE)
		return 0;

	if (build_dataobj_result(&builder, &response->result, TRUE) != TRUE)
		return 0;

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
	default:
		return 0;
	};

	if (ok != TRUE)
		return 0;

	return stk_tlv_builder_get_length(&builder);
}
