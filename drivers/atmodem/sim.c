/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "gatchat.h"
#include "gatresult.h"
#include "simutil.h"
#include "vendor.h"

#include "atmodem.h"

#define EF_STATUS_INVALIDATED 0
#define EF_STATUS_VALID 1

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct sim_data {
	GAtChat *chat;
	unsigned int vendor;
	guint ready_id;
	struct at_util_sim_state_query *sim_state_query;
};

static const char *crsm_prefix[] = { "+CRSM:", NULL };
static const char *cpin_prefix[] = { "+CPIN:", NULL };
static const char *clck_prefix[] = { "+CLCK:", NULL };
static const char *huawei_cpin_prefix[] = { "^CPIN:", NULL };
static const char *xpincnt_prefix[] = { "+XPINCNT:", NULL };
static const char *zpinpuk_prefix[] = { "+ZPINPUK:", NULL };
static const char *pinnum_prefix[] = { "%PINNUM:", NULL };
static const char *oercn_prefix[] = { "_OERCN:", NULL };
static const char *cpinr_prefixes[] = { "+CPINR:", "+CPINRE:", NULL };
static const char *epin_prefix[] = { "*EPIN:", NULL };
static const char *spic_prefix[] = { "+SPIC:", NULL };
static const char *pct_prefix[] = { "#PCT:", NULL };
static const char *pnnm_prefix[] = { "+PNNM:", NULL };
static const char *none_prefix[] = { NULL };

static void at_crsm_info_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct ofono_error error;
	const guint8 *response;
	gint sw1, sw2, len;
	int flen, rlen;
	int str;
	unsigned char access[3];
	unsigned char file_status;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:"))
		goto error;

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &sw2);

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len) ||
			(sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92) ||
			(sw1 == 0x90 && sw2 != 0x00)) {
		memset(&error, 0, sizeof(error));

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		cb(&error, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
		return;
	}

	DBG("crsm_info_cb: %02x, %02x, %i", sw1, sw2, len);

	if (response[0] == 0x62) {
		ok = sim_parse_3g_get_response(response, len, &flen, &rlen,
						&str, access, NULL);

		file_status = EF_STATUS_VALID;
	} else
		ok = sim_parse_2g_get_response(response, len, &flen, &rlen,
						&str, access, &file_status);

	if (!ok)
		goto error;

	cb(&error, flen, str, rlen, access, file_status, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
				EF_STATUS_INVALIDATED, cbd->data);
}

static void at_sim_read_info(struct ofono_sim *sim, int fileid,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	char buf[128];
	unsigned int len;

	if (sd->vendor == OFONO_VENDOR_OPTION_HSO) {
		unsigned char access[3] = { 0x00, 0x00, 0x00 };

		if (fileid == SIM_EFAD_FILEID) {
			CALLBACK_WITH_SUCCESS(cb, 4, 0, 0, access,
						EF_STATUS_VALID, data);
			return;
		}
	}

	cbd = cb_data_new(cb, data);

	len = snprintf(buf, sizeof(buf), "AT+CRSM=192,%i", fileid);

	switch (sd->vendor) {
	default:
		if (path_len == 0)
			break;

		/* Fall through */
	case OFONO_VENDOR_ZTE:
	case OFONO_VENDOR_HUAWEI:
	case OFONO_VENDOR_SIERRA:
	case OFONO_VENDOR_SPEEDUP:
	case OFONO_VENDOR_QUALCOMM_MSM:
		/* Maximum possible length */
		len += sprintf(buf + len, ",0,0,255");
		break;
	}

	if (path_len > 0) {
		len += sprintf(buf + len, ",,\"");

		for (; path_len; path_len--)
			len += sprintf(buf + len, "%02hhX", *path++);

		buf[len++] = '\"';
		buf[len] = '\0';
	}

	if (g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_info_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
				EF_STATUS_INVALIDATED, data);
}

static void at_crsm_read_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct ofono_error error;
	const guint8 *response;
	gint sw1, sw2, len;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, 0, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:")) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &sw2);

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
			(sw1 == 0x90 && sw2 != 0x00)) {
		memset(&error, 0, sizeof(error));

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		cb(&error, NULL, 0, cbd->data);
		return;
	}

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len)) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	DBG("crsm_read_cb: %02x, %02x, %d", sw1, sw2, len);

	cb(&error, response, len, cbd->data);
}

static void at_sim_read_binary(struct ofono_sim *sim, int fileid,
				int start, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	unsigned int len;

	len = snprintf(buf, sizeof(buf), "AT+CRSM=176,%i,%i,%i,%i", fileid,
			start >> 8, start & 0xff, length);

	if (path_len > 0) {
		buf[len++] = ',';
		buf[len++] = ',';
		buf[len++] = '\"';

		for (; path_len; path_len--)
			len += sprintf(buf + len, "%02hhX", *path++);

		buf[len++] = '\"';
		buf[len] = '\0';
	}

	if (g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void at_sim_read_record(struct ofono_sim *sim, int fileid,
				int record, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[128];

	snprintf(buf, sizeof(buf), "AT+CRSM=178,%i,%i,4,%i", fileid,
			record, length);

	if (g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void at_crsm_update_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_write_cb_t cb = cbd->cb;
	struct ofono_error error;
	gint sw1, sw2;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:")) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &sw2);

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
			(sw1 == 0x90 && sw2 != 0x00)) {
		memset(&error, 0, sizeof(error));

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;
	}

	DBG("crsm_update_cb: %02x, %02x", sw1, sw2);

	cb(&error, cbd->data);
}

static void at_sim_update_file(struct ofono_sim *sim, int cmd, int fileid,
				int p1, int p2, int p3,
				const unsigned char *value,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_write_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf;
	int len, ret;
	int size = 38 + p3 * 2;

	DBG("");

	buf = g_try_new(char, size);
	if (buf == NULL)
		goto error;

	len = sprintf(buf, "AT+CRSM=%i,%i,%i,%i,%i,\"", cmd, fileid,p1, p2, p3);

	for (; p3; p3--)
		len += sprintf(buf + len, "%02hhX", *value++);

	buf[len++] = '\"';
	buf[len] = '\0';

	ret = g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_sim_update_binary(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	at_sim_update_file(sim, 214, fileid, start >> 8, start & 0xff,
				length, value, path, path_len, cb, data);
}

static void at_sim_update_record(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	at_sim_update_file(sim, 220, fileid, record, 4, length,
				value, path, path_len, cb, data);
}

static void at_sim_update_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	at_sim_update_file(sim, 220, fileid, 0, 3, length, value,
				path, path_len, cb, data);
}

static void at_cimi_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	struct ofono_error error;
	const char *imsi;
	int i;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (i = 0; i < g_at_result_num_response_lines(result); i++)
		g_at_result_iter_next(&iter, NULL);

	imsi = g_at_result_iter_raw_line(&iter);

	DBG("cimi_cb: %s", imsi);

	cb(&error, imsi, cbd->data);
}

static void at_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
				void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(sd->chat, "AT+CIMI", NULL,
				at_cimi_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static struct {
	enum ofono_sim_password_type type;
	const char *name;
} const at_sim_name[] = {
	{ OFONO_SIM_PASSWORD_NONE,		"READY"		},
	{ OFONO_SIM_PASSWORD_SIM_PIN,		"SIM PIN"	},
	{ OFONO_SIM_PASSWORD_SIM_PUK,		"SIM PUK"	},
	{ OFONO_SIM_PASSWORD_PHSIM_PIN,		"PH-SIM PIN"	},
	{ OFONO_SIM_PASSWORD_PHFSIM_PIN,	"PH-FSIM PIN"	},
	{ OFONO_SIM_PASSWORD_PHFSIM_PUK,	"PH-FSIM PUK"	},
	{ OFONO_SIM_PASSWORD_SIM_PIN2,		"SIM PIN2"	},
	{ OFONO_SIM_PASSWORD_SIM_PUK2,		"SIM PUK2"	},
	{ OFONO_SIM_PASSWORD_PHNET_PIN,		"PH-NET PIN"	},
	{ OFONO_SIM_PASSWORD_PHNET_PUK,		"PH-NET PUK"	},
	{ OFONO_SIM_PASSWORD_PHNETSUB_PIN,	"PH-NETSUB PIN"	},
	{ OFONO_SIM_PASSWORD_PHNETSUB_PUK,	"PH-NETSUB PUK"	},
	{ OFONO_SIM_PASSWORD_PHSP_PIN,		"PH-SP PIN"	},
	{ OFONO_SIM_PASSWORD_PHSP_PUK,		"PH-SP PUK"	},
	{ OFONO_SIM_PASSWORD_PHCORP_PIN,	"PH-CORP PIN"	},
	{ OFONO_SIM_PASSWORD_PHCORP_PUK,	"PH-CORP PUK"	},
};

#define BUILD_PIN_RETRIES_ARRAY(passwd_types, passwd_types_cnt, retry)	\
	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)		\
		retry[i] = -1;						\
									\
	for (i = 0; i < passwd_types_cnt; i++) {			\
		int val;						\
									\
		if (!g_at_result_iter_next_number(&iter, &val))		\
			goto error;					\
									\
		retry[passwd_types[i]] = val;				\
									\
		DBG("retry counter id=%d, val=%d", passwd_types[i],	\
					retry[passwd_types[i]]);	\
	}								\

static void huawei_cpin_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PUK,
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PUK2,
		OFONO_SIM_PASSWORD_SIM_PIN2,
	};

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CPIN:"))
		goto error;

	/* Skip status since we are not interested in this */
	if (!g_at_result_iter_skip_next(&iter))
		goto error;

	/* Skip "overall counter" since we'll grab each one individually */
	if (!g_at_result_iter_skip_next(&iter))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void pinnum_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PUK,
		OFONO_SIM_PASSWORD_SIM_PIN2,
		OFONO_SIM_PASSWORD_SIM_PUK2,
	};


	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%PINNUM:"))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void zpinpuk_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PUK,
	};


	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+ZPINPUK:"))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void xpincnt_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PIN2,
		OFONO_SIM_PASSWORD_SIM_PUK,
		OFONO_SIM_PASSWORD_SIM_PUK2,
	};

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XPINCNT:"))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void oercn_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PUK,
	};

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "_OERCN:"))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void cpnnum_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	const char *line;
	int num;
	char **entries;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (num = 0; num < g_at_result_num_response_lines(result); num++)
		g_at_result_iter_next(&iter, NULL);

	line = g_at_result_iter_raw_line(&iter);

	DBG("%s", line);

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		retries[i] = -1;

	entries = g_strsplit(line, "; ", -1);

	for (num = 0; entries[num]; num++) {
		int retry;

		if (strlen(entries[num]) < 5)
			continue;

		retry = strtol(entries[num] + 5, NULL, 10);
		if (retry == 0 && errno == EINVAL)
			continue;

		if (g_str_has_prefix(entries[num], "PIN1=") == TRUE)
			retries[OFONO_SIM_PASSWORD_SIM_PIN] = retry;
		else if (g_str_has_prefix(entries[num], "PUK1=") == TRUE)
			retries[OFONO_SIM_PASSWORD_SIM_PUK] = retry;
		else if (g_str_has_prefix(entries[num], "PIN2=") == TRUE)
			retries[OFONO_SIM_PASSWORD_SIM_PIN2] = retry;
		else if (g_str_has_prefix(entries[num], "PUK2=") == TRUE)
			retries[OFONO_SIM_PASSWORD_SIM_PUK2] = retry;
	}

	g_strfreev(entries);

	cb(&error, retries, cbd->data);
}

static void at_epin_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PUK,
		OFONO_SIM_PASSWORD_SIM_PIN2,
		OFONO_SIM_PASSWORD_SIM_PUK2,
	};

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "*EPIN:"))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void at_cpinr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t len = sizeof(at_sim_name) / sizeof(*at_sim_name);
	size_t i;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		retries[i] = -1;

	g_at_result_iter_init(&iter, result);

	/* Ignore +CPINRE results... */
	while (g_at_result_iter_next(&iter, "+CPINR:")) {
		const char *name;
		int val;

		if (!g_at_result_iter_next_unquoted_string(&iter, &name))
			continue;

		if (!g_at_result_iter_next_number(&iter, &val))
			continue;

		for (i = 1; i < len; i++) {
			if (!strcmp(name, at_sim_name[i].name)) {
				retries[i] = val;
				break;
			}
		}
	}

	cb(&error, retries, cbd->data);
}

static void at_spic_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PUK,
		OFONO_SIM_PASSWORD_SIM_PIN2,
		OFONO_SIM_PASSWORD_SIM_PUK2,
	};

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+SPIC:"))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

#define AT_PCT_SET_RETRIES(retries, pin_type, value) \
	retries[pin_type] = value; \
	DBG("retry counter id=%d, val=%d", pin_type, value);

static void at_pct_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	struct ofono_sim *sim = cbd->user;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	enum ofono_sim_password_type pin_type;

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		retries[i] = -1;

	pin_type = ofono_sim_get_password_type(sim);
	if (pin_type == OFONO_SIM_PASSWORD_NONE) {
		DBG("Note: No password required, returning maximum retries:");

		AT_PCT_SET_RETRIES(retries, OFONO_SIM_PASSWORD_SIM_PIN, 3);
		AT_PCT_SET_RETRIES(retries, OFONO_SIM_PASSWORD_SIM_PIN2, 3);
		AT_PCT_SET_RETRIES(retries, OFONO_SIM_PASSWORD_SIM_PUK, 10);
		AT_PCT_SET_RETRIES(retries, OFONO_SIM_PASSWORD_SIM_PUK2, 10);

		goto callback;
	}

	if (g_at_result_iter_next(&iter, "#PCT:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &retries[pin_type]) == FALSE)
		goto error;

	DBG("retry counter id=%d, val=%d", pin_type, retries[pin_type]);

callback:
	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void at_pnnm_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	const char *final = g_at_result_final_response(result);
	GAtResultIter iter;
	struct ofono_error error;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	static enum ofono_sim_password_type password_types[] = {
		OFONO_SIM_PASSWORD_SIM_PIN,
		OFONO_SIM_PASSWORD_SIM_PUK,
	};

	decode_at_error(&error, final);

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+PNNM:"))
		goto error;

	BUILD_PIN_RETRIES_ARRAY(password_types, ARRAY_SIZE(password_types),
				retries);

	cb(&error, retries, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void at_pin_retries_query(struct ofono_sim *sim,
					ofono_sim_pin_retries_cb_t cb,
					void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	cbd->user = sim;

	DBG("");

	switch (sd->vendor) {
	case OFONO_VENDOR_IFX:
		if (g_at_chat_send(sd->chat, "AT+XPINCNT", xpincnt_prefix,
					xpincnt_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_SPEEDUP:
		if (g_at_chat_send(sd->chat, "AT+CPNNUM", NULL,
					cpnnum_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_OPTION_HSO:
		if (g_at_chat_send(sd->chat, "AT_OERCN?", oercn_prefix,
					oercn_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_HUAWEI:
		if (g_at_chat_send(sd->chat, "AT^CPIN?", huawei_cpin_prefix,
					huawei_cpin_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_ICERA:
		if (g_at_chat_send(sd->chat, "AT%PINNUM?", pinnum_prefix,
					pinnum_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_ZTE:
		if (g_at_chat_send(sd->chat, "AT+ZPINPUK=?", zpinpuk_prefix,
					zpinpuk_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_MBM:
		if (g_at_chat_send(sd->chat, "AT*EPIN?", epin_prefix,
					at_epin_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_SIMCOM:
		if (g_at_chat_send(sd->chat, "AT+SPIC", spic_prefix,
					at_spic_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_TELIT:
		if (g_at_chat_send(sd->chat, "AT#PCT", pct_prefix,
					at_pct_cb, cbd, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_ALCATEL:
		if (g_at_chat_send(sd->chat, "AT+PNNM?", pnnm_prefix,
					at_pnnm_cb, cbd, g_free) > 0)
			return;
		break;
	default:
		if (g_at_chat_send(sd->chat, "AT+CPINR", cpinr_prefixes,
					at_cpinr_cb, cbd, g_free) > 0)
			return;
		break;
	}

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static int needs_wavecom_sim_quirk(int vendor)
{
	return vendor == OFONO_VENDOR_WAVECOM ||
			vendor == OFONO_VENDOR_WAVECOM_Q2XXX;
}

static void at_cpin_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = ofono_sim_get_data(cbd->user);
	GAtResultIter iter;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	struct ofono_error error;
	const char *pin_required;
	int pin_type = OFONO_SIM_PASSWORD_INVALID;
	int i;
	int len = sizeof(at_sim_name) / sizeof(*at_sim_name);
	const char *final = g_at_result_final_response(result);

	if (needs_wavecom_sim_quirk(sd->vendor) && ok && strlen(final) > 7)
		decode_at_error(&error, "OK");
	else
		decode_at_error(&error, final);

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	if (needs_wavecom_sim_quirk(sd->vendor)) {
		/* +CPIN: <pin> */
		pin_required = final + 7;
	} else {
		g_at_result_iter_init(&iter, result);

		if (!g_at_result_iter_next(&iter, "+CPIN:")) {
			CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
			return;
		}

		g_at_result_iter_next_unquoted_string(&iter, &pin_required);
	}

	for (i = 0; i < len; i++) {
		if (strcmp(pin_required, at_sim_name[i].name))
			continue;

		pin_type = at_sim_name[i].type;
		break;
	}

	if (pin_type == OFONO_SIM_PASSWORD_INVALID) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	DBG("crsm_pin_cb: %s", pin_required);

	cb(&error, pin_type, cbd->data);
}

static void at_pin_query(struct ofono_sim *sim, ofono_sim_passwd_cb_t cb,
			void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = sim;

	if (g_at_chat_send(sd->chat, "AT+CPIN?", cpin_prefix,
				at_cpin_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void at_xsim_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = cbd->user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_NO_ERROR };
	GAtResultIter iter;
	int state;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XSIM:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	switch (state) {
	case 3:	/* PIN verified – Ready */
		break;
	default:
		return;
	}

	cb(&error, cbd->data);

	g_at_chat_unregister(sd->chat, sd->ready_id);
	sd->ready_id = 0;
}

static void at_epev_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = cbd->user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_NO_ERROR };

	cb(&error, cbd->data);

	g_at_chat_unregister(sd->chat, sd->ready_id);
	sd->ready_id = 0;
}

static void at_qss_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = cbd->user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_NO_ERROR };
	GAtResultIter iter;
	int state;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "#QSS:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	switch (state) {
	case 3:	/* SIM inserted and READY. */
		break;
	default:
		return;
	}

	cb(&error, cbd->data);

	g_at_chat_unregister(sd->chat, sd->ready_id);
	sd->ready_id = 0;
}

static void sim_state_cb(gboolean present, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = cbd->user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;

	at_util_sim_state_query_free(sd->sim_state_query);
	sd->sim_state_query = NULL;

	if (present == 1)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void at_pin_send_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = cbd->user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto done;

	switch (sd->vendor) {
	case OFONO_VENDOR_IFX:
		/*
		 * On the IFX modem, AT+CPIN? can return READY too
		 * early and so use +XSIM notification to detect
		 * the ready state of the SIM.
		 */
		sd->ready_id = g_at_chat_register(sd->chat, "+XSIM",
							at_xsim_notify,
							FALSE, cbd, g_free);
		return;
	case OFONO_VENDOR_MBM:
		/*
		 * On the MBM modem, AT+CPIN? keeps returning SIM PIN
		 * for a moment after successful AT+CPIN="..", but then
		 * sends *EPEV when that changes.
		 */
		sd->ready_id = g_at_chat_register(sd->chat, "*EPEV",
							at_epev_notify,
							FALSE, cbd, g_free);
		return;
	case OFONO_VENDOR_TELIT:
		/*
		 * On the Telit modem, AT+CPIN? can return READY too
		 * early and so use #QSS notification to detect
		 * the ready state of the SIM.
		 */
		sd->ready_id = g_at_chat_register(sd->chat, "#QSS",
							at_qss_notify,
							FALSE, cbd, g_free);
		return;
	case OFONO_VENDOR_ZTE:
	case OFONO_VENDOR_ALCATEL:
	case OFONO_VENDOR_HUAWEI:
		/*
		 * On ZTE modems, after pin is entered, SIM state is checked
		 * by polling CPIN as their modem doesn't provide unsolicited
		 * notification of SIM readiness.
		 */
		sd->sim_state_query = at_util_sim_state_query_new(sd->chat,
						2, 20, sim_state_cb, cbd,
						g_free);
		return;
	}

done:
	cb(&error, cbd->data);

	g_free(cbd);
}

static void at_pin_send(struct ofono_sim *sim, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;

	cbd->user = sd;

	snprintf(buf, sizeof(buf), "AT+CPIN=\"%s\"", passwd);

	ret = g_at_chat_send(sd->chat, buf, none_prefix,
				at_pin_send_cb, cbd, NULL);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_pin_send_puk(struct ofono_sim *sim, const char *puk,
				const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;

	cbd->user = sd;

	snprintf(buf, sizeof(buf), "AT+CPIN=\"%s\",\"%s\"", puk, passwd);

	ret = g_at_chat_send(sd->chat, buf, none_prefix,
				at_pin_send_cb, cbd, NULL);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_lock_unlock_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static const char *const at_clck_cpwd_fac[] = {
	[OFONO_SIM_PASSWORD_SIM_PIN] = "SC",
	[OFONO_SIM_PASSWORD_SIM_PIN2] = "P2",
	[OFONO_SIM_PASSWORD_PHSIM_PIN] = "PS",
	[OFONO_SIM_PASSWORD_PHFSIM_PIN] = "PF",
	[OFONO_SIM_PASSWORD_PHNET_PIN] = "PN",
	[OFONO_SIM_PASSWORD_PHNETSUB_PIN] = "PU",
	[OFONO_SIM_PASSWORD_PHSP_PIN] = "PP",
	[OFONO_SIM_PASSWORD_PHCORP_PIN] = "PC",
};

static void at_pin_enable(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;
	unsigned int len = sizeof(at_clck_cpwd_fac) / sizeof(*at_clck_cpwd_fac);

	if (passwd_type >= len || at_clck_cpwd_fac[passwd_type] == NULL)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CLCK=\"%s\",%i,\"%s\"",
			at_clck_cpwd_fac[passwd_type], enable ? 1 : 0, passwd);

	ret = g_at_chat_send(sd->chat, buf, none_prefix,
				at_lock_unlock_cb, cbd, g_free);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;
	unsigned int len = sizeof(at_clck_cpwd_fac) / sizeof(*at_clck_cpwd_fac);

	if (passwd_type >= len ||
			at_clck_cpwd_fac[passwd_type] == NULL)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CPWD=\"%s\",\"%s\",\"%s\"",
			at_clck_cpwd_fac[passwd_type], old_passwd, new_passwd);

	ret = g_at_chat_send(sd->chat, buf, none_prefix,
				at_lock_unlock_cb, cbd, g_free);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_lock_status_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_locked_cb_t cb = cbd->cb;
	struct ofono_error error;
	int locked;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLCK:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &locked);

	DBG("lock_status_cb: %i", locked);

	cb(&error, locked, cbd->data);
}

static void at_pin_query_enabled(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				ofono_sim_locked_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	unsigned int len = sizeof(at_clck_cpwd_fac) / sizeof(*at_clck_cpwd_fac);

	if (passwd_type >= len || at_clck_cpwd_fac[passwd_type] == NULL)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CLCK=\"%s\",2",
			at_clck_cpwd_fac[passwd_type]);

	if (g_at_chat_send(sd->chat, buf, clck_prefix,
				at_lock_status_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static gboolean at_sim_register(gpointer user)
{
	struct ofono_sim *sim = user;

	ofono_sim_register(sim);

	return FALSE;
}

static int at_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct sim_data *sd;

	sd = g_new0(struct sim_data, 1);
	sd->chat = g_at_chat_clone(chat);
	sd->vendor = vendor;

	if (sd->vendor == OFONO_VENDOR_MBM)
		g_at_chat_send(sd->chat, "AT*EPEE=1", NULL, NULL, NULL, NULL);

	ofono_sim_set_data(sim, sd);
	g_idle_add(at_sim_register, sim);

	return 0;
}

static void at_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	g_idle_remove_by_data(sim);
	/* Cleanup potential SIM state polling */
	at_util_sim_state_query_free(sd->sim_state_query);

	ofono_sim_set_data(sim, NULL);

	g_at_chat_unref(sd->chat);
	g_free(sd);
}

static struct ofono_sim_driver driver = {
	.name			= "atmodem",
	.probe			= at_sim_probe,
	.remove			= at_sim_remove,
	.read_file_info		= at_sim_read_info,
	.read_file_transparent	= at_sim_read_binary,
	.read_file_linear	= at_sim_read_record,
	.read_file_cyclic	= at_sim_read_record,
	.write_file_transparent	= at_sim_update_binary,
	.write_file_linear	= at_sim_update_record,
	.write_file_cyclic	= at_sim_update_cyclic,
	.read_imsi		= at_read_imsi,
	.query_passwd_state	= at_pin_query,
	.query_pin_retries	= at_pin_retries_query,
	.send_passwd		= at_pin_send,
	.reset_passwd		= at_pin_send_puk,
	.lock			= at_pin_enable,
	.change_passwd		= at_change_passwd,
	.query_locked		= at_pin_query_enabled,
};

static struct ofono_sim_driver driver_noef = {
	.name			= "atmodem-noef",
	.probe			= at_sim_probe,
	.remove			= at_sim_remove,
	.read_imsi		= at_read_imsi,
	.query_passwd_state	= at_pin_query,
	.query_pin_retries	= at_pin_retries_query,
	.send_passwd		= at_pin_send,
	.reset_passwd		= at_pin_send_puk,
	.lock			= at_pin_enable,
	.change_passwd		= at_change_passwd,
	.query_locked		= at_pin_query_enabled,
};

void at_sim_init(void)
{
	ofono_sim_driver_register(&driver);
	ofono_sim_driver_register(&driver_noef);
}

void at_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
	ofono_sim_driver_unregister(&driver_noef);
}
