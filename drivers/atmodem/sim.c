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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "gatchat.h"
#include "gatresult.h"
#include "simutil.h"
#include "vendor.h"

#include "atmodem.h"

struct sim_data {
	GAtChat *chat;
	unsigned int vendor;
	guint ready_id;
	guint ready_source;
};

static const char *crsm_prefix[] = { "+CRSM:", NULL };
static const char *cpin_prefix[] = { "+CPIN:", NULL };
static const char *clck_prefix[] = { "+CLCK:", NULL };
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

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, NULL, cbd->data);
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

		cb(&error, -1, -1, -1, NULL, cbd->data);
		return;
	}

	DBG("crsm_info_cb: %02x, %02x, %i", sw1, sw2, len);

	if (response[0] == 0x62)
		ok = sim_parse_3g_get_response(response, len, &flen, &rlen,
						&str, access, NULL);
	else
		ok = sim_parse_2g_get_response(response, len, &flen, &rlen,
						&str, access);

	if (!ok)
		goto error;

	cb(&error, flen, str, rlen, access, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, cbd->data);
}

static void at_sim_read_info(struct ofono_sim *sim, int fileid,
					ofono_sim_file_info_cb_t cb,
					void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	char buf[64];

	if (sd->vendor == OFONO_VENDOR_OPTION_HSO) {
		unsigned char access[3] = { 0x00, 0x00, 0x00 };

		if (fileid == SIM_EFAD_FILEID) {
			CALLBACK_WITH_SUCCESS(cb, 4, 0, 0, access, data);
			return;
		}
	}

	cbd = cb_data_new(cb, data);
	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=192,%i", fileid);

	if (sd->vendor == OFONO_VENDOR_QUALCOMM_MSM)
		strcat(buf, ",0,0,255"); /* Maximum possible length */

	if (g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_info_cb, cbd, g_free) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, data);
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
					ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=176,%i,%i,%i,%i", fileid,
			start >> 8, start & 0xff, length);

	if (g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void at_sim_read_record(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=178,%i,%i,4,%i", fileid,
			record, length);

	if (g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

error:
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

static void at_sim_update_binary(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=214,%i,%i,%i,%i,", fileid,
			start >> 8, start & 0xff, length);

	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *value++);

	ret = g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_sim_update_record(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=220,%i,%i,4,%i,", fileid,
			record, length);

	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *value++);

	ret = g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_sim_update_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=220,%i,0,3,%i,", fileid, length);

	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *value++);

	ret = g_at_chat_send(sd->chat, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
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

	if (!cbd)
		goto error;

	if (g_at_chat_send(sd->chat, "AT+CIMI", NULL,
				at_cimi_cb, cbd, g_free) > 0)
		return;

error:
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

	if (sd->vendor == OFONO_VENDOR_WAVECOM && ok && strlen(final) > 7)
		decode_at_error(&error, "OK");
	else
		decode_at_error(&error, final);

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	if (sd->vendor == OFONO_VENDOR_WAVECOM) {
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

	if (!cbd)
		goto error;

	cbd->user = sim;

	if (g_at_chat_send(sd->chat, "AT+CPIN?", cpin_prefix,
				at_cpin_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static gboolean ready_notify_unregister(gpointer user_data)
{
	struct sim_data *sd = user_data;

	sd->ready_source = 0;

	g_at_chat_unregister(sd->chat, sd->ready_id);
	sd->ready_id = 0;

	return FALSE;
}

static void at_xsim_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = cbd->user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_NO_ERROR };
	GAtResultIter iter;
	int state;

	if (sd->ready_source > 0)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XSIM:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	switch (state) {
	case 3:	/* PIN verified – Ready */
	case 7:	/* ready for attach (+COPS) */
		break;
	default:
		return;
	}

	cb(&error, cbd->data);

	sd->ready_source = g_timeout_add(0, ready_notify_unregister, sd);
}

static void at_epev_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct sim_data *sd = cbd->user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_NO_ERROR };

	if (sd->ready_source > 0)
		return;

	cb(&error, cbd->data);

	sd->ready_source = g_timeout_add(0, ready_notify_unregister, sd);
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
	}

done:
	cb(&error, cbd->data);

	g_free(cbd);
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

static void at_pin_send(struct ofono_sim *sim, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;

	if (!cbd)
		goto error;

	cbd->user = sd;

	snprintf(buf, sizeof(buf), "AT+CPIN=\"%s\"", passwd);

	ret = g_at_chat_send(sd->chat, buf, none_prefix,
				at_pin_send_cb, cbd, NULL);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
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

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CPIN=\"%s\",\"%s\"", puk, passwd);

	ret = g_at_chat_send(sd->chat, buf, none_prefix,
				at_lock_unlock_cb, cbd, g_free);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
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

	if (!cbd)
		goto error;

	if (passwd_type >= len || !at_clck_cpwd_fac[passwd_type])
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
			const char *old, const char *new,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;
	unsigned int len = sizeof(at_clck_cpwd_fac) / sizeof(*at_clck_cpwd_fac);

	if (!cbd)
		goto error;

	if (passwd_type >= len ||
			!at_clck_cpwd_fac[passwd_type])
		goto error;

	snprintf(buf, sizeof(buf), "AT+CPWD=\"%s\",\"%s\",\"%s\"",
			at_clck_cpwd_fac[passwd_type], old, new);

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

	if (!cbd)
		goto error;

	if (passwd_type >= len || !at_clck_cpwd_fac[passwd_type])
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

	switch (sd->vendor) {
	case OFONO_VENDOR_WAVECOM:
		g_at_chat_add_terminator(sd->chat, "+CPIN:", 6, TRUE);
		break;
	case OFONO_VENDOR_MBM:
		g_at_chat_send(sd->chat, "AT*EPEE=1", NULL, NULL, NULL, NULL);
		break;
	default:
		break;
	}

	ofono_sim_set_data(sim, sd);
	g_idle_add(at_sim_register, sim);

	return 0;
}

static void at_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	if (sd->ready_source > 0)
		g_source_remove(sd->ready_source);

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
	.send_passwd		= at_pin_send,
	.reset_passwd		= at_pin_send_puk,
	.lock			= at_pin_enable,
	.change_passwd		= at_change_passwd,
	.query_locked		= at_pin_query_enabled,
};

void at_sim_init()
{
	ofono_sim_driver_register(&driver);
}

void at_sim_exit()
{
	ofono_sim_driver_unregister(&driver);
}
