/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include "atmodem.h"

static const char *crsm_prefix[] = { "+CRSM:", NULL };

static void at_crsm_info_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct ofono_error error;
	const guint8 *response;
	gint sw1, sw2, len;
	int flen, rlen;
	enum ofono_sim_file_structure str;
	unsigned char access[3];

	dump_response("at_crsm_info_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRSM:")) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &sw1);
	g_at_result_iter_next_number(&iter, &sw2);

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len) ||
			(sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92) ||
			(sw1 == 0x90 && sw2 != 0x00) ||
			len < 14 || response[6] != 0x04 ||
			(response[13] == 0x01 && len < 15)) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, cbd->data);
		return;
	}

	ofono_debug("crsm_info_cb: %02x, %02x, %i", sw1, sw2, len);

	flen = (response[2] << 8) | response[3];
	str = response[13];

	access[0] = response[8];
	access[1] = response[9];
	access[2] = response[10];

	if (str == 0x01 || str == 0x03)
		rlen = response[14];
	else
		rlen = 0;

	cb(&error, flen, str, rlen, access, cbd->data);
}

static void at_sim_read_info(struct ofono_sim *sim, int fileid,
					ofono_sim_file_info_cb_t cb,
					void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=192,%i,0,0,15", fileid);

	if (g_at_chat_send(chat, buf, crsm_prefix,
				at_crsm_info_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

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

	dump_response("at_crsm_read_cb", ok, result);
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

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len) ||
		(sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	ofono_debug("crsm_read_cb: %02x, %02x, %d", sw1, sw2, len);

	cb(&error, response, len, cbd->data);
}

static void at_sim_read_binary(struct ofono_sim *sim, int fileid,
					int start, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=176,%i,%i,%i,%i", fileid,
			start >> 8, start & 0xff, length);

	if (g_at_chat_send(chat, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void at_sim_read_record(struct ofono_sim *sim, int fileid,
					int record, int length,
					ofono_sim_read_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CRSM=178,%i,%i,4,%i", fileid,
			record, length);

	if (g_at_chat_send(chat, buf, crsm_prefix,
				at_crsm_read_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
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

	dump_response("at_crsm_update_cb", ok, result);
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
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	ofono_debug("crsm_update_cb: %02x, %02x", sw1, sw2);

	cb(&error, cbd->data);
}

static void at_sim_update_binary(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=214,%i,%i,%i,%i,", fileid,
			start >> 8, start & 0xff, length);

	for (; length; length--)
		len += sprintf(buf + len, "%02hhx", *value++);

	ret = g_at_chat_send(chat, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_sim_update_record(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=220,%i,%i,4,%i,", fileid,
			record, length);

	for (; length; length--)
		len += sprintf(buf + len, "%02hhx", *value++);

	ret = g_at_chat_send(chat, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_sim_update_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					ofono_sim_write_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 36 + length * 2);
	int len, ret;

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+CRSM=220,%i,0,3,%i,", fileid, length);

	for (; length; length--)
		len += sprintf(buf + len, "%02hhx", *value++);

	ret = g_at_chat_send(chat, buf, crsm_prefix,
				at_crsm_update_cb, cbd, g_free);

	g_free(buf);

	if (ret > 0)
		return;

error:
	if (cbd)
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

	dump_response("at_cimi_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	for (i = 0; i < g_at_result_num_response_lines(result); i++)
		g_at_result_iter_next(&iter, NULL);

	imsi = g_at_result_iter_raw_line(&iter);

	ofono_debug("cimi_cb: %s", imsi);

	cb(&error, imsi, cbd->data);
}

static void at_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
			void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, "AT+CIMI", NULL,
				at_cimi_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static struct {
	enum ofono_sim_password_type type;
	const char *name;
} const at_sim_name[] = {
	{ OFONO_SIM_PASSWORD_NONE, "READY" },
	{ OFONO_SIM_PASSWORD_SIM_PIN, "SIM PIN" },
	{ OFONO_SIM_PASSWORD_SIM_PUK, "SIM PUK" },
	{ OFONO_SIM_PASSWORD_PHSIM_PIN, "PH-SIM PIN" },
	{ OFONO_SIM_PASSWORD_PHFSIM_PIN, "PH-FSIM PIN" },
	{ OFONO_SIM_PASSWORD_PHFSIM_PUK, "PH-FSIM PUK" },
	{ OFONO_SIM_PASSWORD_SIM_PIN2, "SIM PIN2" },
	{ OFONO_SIM_PASSWORD_SIM_PUK2, "SIM PUK2" },
	{ OFONO_SIM_PASSWORD_PHNET_PIN, "PH-NET PIN" },
	{ OFONO_SIM_PASSWORD_PHNET_PUK, "PH-NET PUK" },
	{ OFONO_SIM_PASSWORD_PHNETSUB_PIN, "PH-NETSUB PIN" },
	{ OFONO_SIM_PASSWORD_PHNETSUB_PUK, "PH-NETSUB PUK" },
	{ OFONO_SIM_PASSWORD_PHSP_PIN, "PH-SP PIN" },
	{ OFONO_SIM_PASSWORD_PHSP_PUK, "PH-SP PUK" },
	{ OFONO_SIM_PASSWORD_PHCORP_PIN, "PH-CORP PIN" },
	{ OFONO_SIM_PASSWORD_PHCORP_PUK, "PH-CORP PUK" },
};

static void at_cpin_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	struct ofono_error error;
	const char *pin_required;
	int pin_type = OFONO_SIM_PASSWORD_INVALID;
	int i;
	int len = sizeof(at_sim_name) / sizeof(*at_sim_name);

	dump_response("at_cpin_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CPIN:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_unquoted_string(&iter, &pin_required);

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

	ofono_debug("crsm_pin_cb: %s", pin_required);

	cb(&error, pin_type, cbd->data);
}

static void at_pin_query(struct ofono_sim *sim, ofono_sim_passwd_cb_t cb,
			void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, "AT+CPIN?", NULL,
				at_cpin_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void at_lock_unlock_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("at_lock_unlock_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_pin_send(struct ofono_sim *sim, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CPIN=\"%s\"", passwd);

	ret = g_at_chat_send(chat, buf, NULL, at_lock_unlock_cb, cbd, g_free);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_pin_send_puk(struct ofono_sim *sim, const char *puk,
				const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int ret;

	if (!cbd)
		goto error;

	snprintf(buf, sizeof(buf), "AT+CPIN=\"%s\",\"%s\"", puk, passwd);

	ret = g_at_chat_send(chat, buf, NULL, at_lock_unlock_cb, cbd, g_free);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
	if (cbd)
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
	GAtChat *chat = ofono_sim_get_data(sim);
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

	ret = g_at_chat_send(chat, buf, NULL, at_lock_unlock_cb, cbd, g_free);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_change_passwd(struct ofono_sim *sim,
			enum ofono_sim_password_type passwd_type,
			const char *old, const char *new,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
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

	ret = g_at_chat_send(chat, buf, NULL, at_lock_unlock_cb, cbd, g_free);

	memset(buf, 0, sizeof(buf));

	if (ret > 0)
		return;

error:
	if (cbd)
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

	dump_response("at_lock_status_cb", ok, result);
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

	ofono_debug("lock_status_cb: %i", locked);

	cb(&error, locked, cbd->data);
}

static void at_pin_query_enabled(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				ofono_sim_locked_cb_t cb, void *data)
{
	GAtChat *chat = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	unsigned int len = sizeof(at_clck_cpwd_fac) / sizeof(*at_clck_cpwd_fac);

	if (!cbd)
		goto error;

	if (passwd_type >= len || !at_clck_cpwd_fac[passwd_type])
		goto error;

	snprintf(buf, sizeof(buf), "AT+CLCK=\"%s\",2",
			at_clck_cpwd_fac[passwd_type]);

	if (g_at_chat_send(chat, buf, NULL,
				at_lock_status_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
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

	ofono_sim_set_data(sim, chat);
	g_idle_add(at_sim_register, sim);

	return 0;
}

static void at_sim_remove(struct ofono_sim *sim)
{
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
