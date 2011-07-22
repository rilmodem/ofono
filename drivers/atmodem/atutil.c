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

#include <glib.h>
#include <gatchat.h>
#include <string.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/log.h>
#include <ofono/types.h>

#include "atutil.h"
#include "vendor.h"

static const char *cpin_prefix[] = { "+CPIN:", NULL };

struct at_util_sim_state_query {
	GAtChat *chat;
	guint cpin_poll_source;
	guint cpin_poll_count;
	guint interval;
	guint num_times;
	at_util_sim_inserted_cb_t cb;
	void *userdata;
};

static gboolean cpin_check(gpointer userdata);

void decode_at_error(struct ofono_error *error, const char *final)
{
	if (!strcmp(final, "OK")) {
		error->type = OFONO_ERROR_TYPE_NO_ERROR;
		error->error = 0;
	} else if (g_str_has_prefix(final, "+CMS ERROR:")) {
		error->type = OFONO_ERROR_TYPE_CMS;
		error->error = strtol(&final[11], NULL, 0);
	} else if (g_str_has_prefix(final, "+CME ERROR:")) {
		error->type = OFONO_ERROR_TYPE_CME;
		error->error = strtol(&final[11], NULL, 0);
	} else {
		error->type = OFONO_ERROR_TYPE_FAILURE;
		error->error = 0;
	}
}

gint at_util_call_compare_by_status(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	int status = GPOINTER_TO_INT(b);

	if (status != call->status)
		return 1;

	return 0;
}

gint at_util_call_compare_by_phone_number(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	const struct ofono_phone_number *pb = b;

	return memcmp(&call->phone_number, pb,
				sizeof(struct ofono_phone_number));
}

gint at_util_call_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	if (id < call->id)
		return -1;

	if (id > call->id)
		return 1;

	return 0;
}

gint at_util_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

GSList *at_util_parse_clcc(GAtResult *result)
{
	GAtResultIter iter;
	GSList *l = NULL;
	int id, dir, status, type;
	ofono_bool_t mpty;
	struct ofono_call *call;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CLCC:")) {
		const char *str = "";
		int number_type = 129;

		if (!g_at_result_iter_next_number(&iter, &id))
			continue;

		if (!g_at_result_iter_next_number(&iter, &dir))
			continue;

		if (!g_at_result_iter_next_number(&iter, &status))
			continue;

		if (!g_at_result_iter_next_number(&iter, &type))
			continue;

		if (!g_at_result_iter_next_number(&iter, &mpty))
			continue;

		if (g_at_result_iter_next_string(&iter, &str))
			g_at_result_iter_next_number(&iter, &number_type);

		call = g_try_new(struct ofono_call, 1);
		if (call == NULL)
			break;

		ofono_call_init(call);

		call->id = id;
		call->direction = dir;
		call->status = status;
		call->type = type;
		strncpy(call->phone_number.number, str,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = number_type;

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		l = g_slist_insert_sorted(l, call, at_util_call_compare);
	}

	return l;
}

gboolean at_util_parse_reg_unsolicited(GAtResult *result, const char *prefix,
					int *status,
					int *lac, int *ci, int *tech,
					unsigned int vendor)
{
	GAtResultIter iter;
	int s;
	int l = -1, c = -1, t = -1;
	const char *str;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, prefix) == FALSE)
		return FALSE;

	if (g_at_result_iter_next_number(&iter, &s) == FALSE)
		return FALSE;

	/* Some firmware will report bogus lac/ci when unregistered */
	if (s != 1 && s != 5)
		goto out;

	switch (vendor) {
	case OFONO_VENDOR_GOBI:
	case OFONO_VENDOR_HUAWEI:
	case OFONO_VENDOR_NOVATEL:
		if (g_at_result_iter_next_unquoted_string(&iter, &str) == TRUE)
			l = strtol(str, NULL, 16);
		else
			goto out;

		if (g_at_result_iter_next_unquoted_string(&iter, &str) == TRUE)
			c = strtol(str, NULL, 16);
		else
			goto out;

		break;
	default:
		if (g_at_result_iter_next_string(&iter, &str) == TRUE)
			l = strtol(str, NULL, 16);
		else
			goto out;

		if (g_at_result_iter_next_string(&iter, &str) == TRUE)
			c = strtol(str, NULL, 16);
		else
			goto out;
	}

	g_at_result_iter_next_number(&iter, &t);

out:
	if (status)
		*status = s;

	if (lac)
		*lac = l;

	if (ci)
		*ci = c;

	if (tech)
		*tech = t;

	return TRUE;
}

gboolean at_util_parse_reg(GAtResult *result, const char *prefix,
				int *mode, int *status,
				int *lac, int *ci, int *tech,
				unsigned int vendor)
{
	GAtResultIter iter;
	int m, s;
	int l = -1, c = -1, t = -1;
	const char *str;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, prefix)) {
		gboolean r;

		g_at_result_iter_next_number(&iter, &m);

		/* Sometimes we get an unsolicited CREG/CGREG here, skip it */
		switch (vendor) {
		case OFONO_VENDOR_HUAWEI:
		case OFONO_VENDOR_NOVATEL:
			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == FALSE || strlen(str) != 1)
				continue;

			s = strtol(str, NULL, 10);

			break;
		default:
			if (g_at_result_iter_next_number(&iter, &s) == FALSE)
				continue;

			break;
		}

		/* Some firmware will report bogus lac/ci when unregistered */
		if (s != 1 && s != 5)
			goto out;

		switch (vendor) {
		case OFONO_VENDOR_GOBI:
		case OFONO_VENDOR_HUAWEI:
		case OFONO_VENDOR_NOVATEL:
			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == TRUE)
				l = strtol(str, NULL, 16);
			else
				goto out;

			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == TRUE)
				c = strtol(str, NULL, 16);
			else
				goto out;

			break;
		default:
			if (g_at_result_iter_next_string(&iter, &str) == TRUE)
				l = strtol(str, NULL, 16);
			else
				goto out;

			if (g_at_result_iter_next_string(&iter, &str) == TRUE)
				c = strtol(str, NULL, 16);
			else
				goto out;
		}

		g_at_result_iter_next_number(&iter, &t);

out:
		if (mode)
			*mode = m;

		if (status)
			*status = s;

		if (lac)
			*lac = l;

		if (ci)
			*ci = c;

		if (tech)
			*tech = t;

		return TRUE;
	}

	return FALSE;
}

gboolean at_util_parse_sms_index_delivery(GAtResult *result, const char *prefix,
						enum at_util_sms_store *out_st,
						int *out_index)
{
	GAtResultIter iter;
	const char *strstore;
	enum at_util_sms_store st;
	int index;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, prefix))
		return FALSE;

	if (!g_at_result_iter_next_string(&iter, &strstore))
		return FALSE;

	if (g_str_equal(strstore, "ME"))
		st = AT_UTIL_SMS_STORE_ME;
	else if (g_str_equal(strstore, "SM"))
		st = AT_UTIL_SMS_STORE_SM;
	else if (g_str_equal(strstore, "SR"))
		st = AT_UTIL_SMS_STORE_SR;
	else if (g_str_equal(strstore, "BM"))
		st = AT_UTIL_SMS_STORE_BM;
	else
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, &index))
		return FALSE;

	if (out_index)
		*out_index = index;

	if (out_st)
		*out_st = st;

	return TRUE;
}

static gboolean at_util_charset_string_to_charset(const char *str,
					enum at_util_charset *charset)
{
	if (!g_strcmp0(str, "GSM"))
		*charset = AT_UTIL_CHARSET_GSM;
	else if (!g_strcmp0(str, "HEX"))
		*charset = AT_UTIL_CHARSET_HEX;
	else if (!g_strcmp0(str, "IRA"))
		*charset = AT_UTIL_CHARSET_IRA;
	else if (!g_strcmp0(str, "PCCP437"))
		*charset = AT_UTIL_CHARSET_PCCP437;
	else if (!g_strcmp0(str, "PCDN"))
		*charset = AT_UTIL_CHARSET_PCDN;
	else if (!g_strcmp0(str, "UCS2"))
		*charset = AT_UTIL_CHARSET_UCS2;
	else if (!g_strcmp0(str, "UTF-8"))
		*charset = AT_UTIL_CHARSET_UTF8;
	else if (!g_strcmp0(str, "8859-1"))
		*charset = AT_UTIL_CHARSET_8859_1;
	else if (!g_strcmp0(str, "8859-2"))
		*charset = AT_UTIL_CHARSET_8859_2;
	else if (!g_strcmp0(str, "8859-3"))
		*charset = AT_UTIL_CHARSET_8859_3;
	else if (!g_strcmp0(str, "8859-4"))
		*charset = AT_UTIL_CHARSET_8859_4;
	else if (!g_strcmp0(str, "8859-5"))
		*charset = AT_UTIL_CHARSET_8859_5;
	else if (!g_strcmp0(str, "8859-6"))
		*charset = AT_UTIL_CHARSET_8859_6;
	else if (!g_strcmp0(str, "8859-C"))
		*charset = AT_UTIL_CHARSET_8859_C;
	else if (!g_strcmp0(str, "8859-A"))
		*charset = AT_UTIL_CHARSET_8859_A;
	else if (!g_strcmp0(str, "8859-G"))
		*charset = AT_UTIL_CHARSET_8859_G;
	else if (!g_strcmp0(str, "8859-H"))
		*charset = AT_UTIL_CHARSET_8859_H;
	else
		return FALSE;

	return TRUE;
}

gboolean at_util_parse_cscs_supported(GAtResult *result, int *supported)
{
	GAtResultIter iter;
	const char *str;
	enum at_util_charset charset;
	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		return FALSE;

	/* Some modems don't report CSCS in a proper list */
	g_at_result_iter_open_list(&iter);

	while (g_at_result_iter_next_string(&iter, &str)) {
		if (at_util_charset_string_to_charset(str, &charset))
			*supported |= charset;
	}

	g_at_result_iter_close_list(&iter);

	return TRUE;
}

gboolean at_util_parse_cscs_query(GAtResult *result,
				enum at_util_charset *charset)
{
	GAtResultIter iter;
	const char *str;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		return FALSE;

	if (g_at_result_iter_next_string(&iter, &str))
		return at_util_charset_string_to_charset(str, charset);

	return FALSE;
}

static const char *at_util_fixup_return(const char *line, const char *prefix)
{
	if (g_str_has_prefix(line, prefix) == FALSE)
		return line;

	line += strlen(prefix);

	while (line[0] == ' ')
		line++;

	return line;
}

gboolean at_util_parse_attr(GAtResult *result, const char *prefix,
				const char **out_attr)
{
	int numlines = g_at_result_num_response_lines(result);
	GAtResultIter iter;
	const char *line;
	int i;

	if (numlines == 0)
		return FALSE;

	g_at_result_iter_init(&iter, result);

	/*
	 * We have to be careful here, sometimes a stray unsolicited
	 * notification will appear as part of the response and we
	 * cannot rely on having a prefix to recognize the actual
	 * response line.  So use the last line only as the response
	 */
	for (i = 0; i < numlines; i++)
		g_at_result_iter_next(&iter, NULL);

	line = g_at_result_iter_raw_line(&iter);

	if (out_attr)
		*out_attr = at_util_fixup_return(line, prefix);

	return TRUE;
}

static void cpin_check_cb(gboolean ok, GAtResult *result, gpointer userdata)
{
	struct at_util_sim_state_query *req = userdata;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (error.type == OFONO_ERROR_TYPE_NO_ERROR)
		goto done;

	/*
	 * If we got a generic error the AT port might not be ready,
	 * try again
	 */
	if (error.type == OFONO_ERROR_TYPE_FAILURE)
		goto tryagain;

	/* If we got any other error besides CME, fail */
	if (error.type != OFONO_ERROR_TYPE_CME)
		goto done;

	switch (error.error) {
	case 10:
	case 13:
		goto done;

	case 14:
		goto tryagain;

	default:
		/* Assume SIM is present */
		ok = TRUE;
		goto done;
	}

tryagain:
	if (req->cpin_poll_count++ < req->num_times) {
		req->cpin_poll_source = g_timeout_add_seconds(req->interval,
								cpin_check,
								req);
		return;
	}

done:
	if (req->cb)
		req->cb(ok, req->userdata);
}

static gboolean cpin_check(gpointer userdata)
{
	struct at_util_sim_state_query *req = userdata;

	req->cpin_poll_source = 0;

	g_at_chat_send(req->chat, "AT+CPIN?", cpin_prefix,
			cpin_check_cb, req, NULL);

	return FALSE;
}

struct at_util_sim_state_query *at_util_sim_state_query_new(GAtChat *chat,
						guint interval, guint num_times,
						at_util_sim_inserted_cb_t cb,
						void *userdata)
{
	struct at_util_sim_state_query *req;

	req = g_new0(struct at_util_sim_state_query, 1);

	req->chat = chat;
	req->interval = interval;
	req->num_times = num_times;
	req->cb = cb;
	req->userdata = userdata;

	cpin_check(req);

	return req;
}

void at_util_sim_state_query_free(struct at_util_sim_state_query *req)
{
	if (req->cpin_poll_source > 0)
		g_source_remove(req->cpin_poll_source);

	g_free(req);
}
