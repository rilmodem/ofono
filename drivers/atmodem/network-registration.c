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
#include "driver.h"

#include "gatchat.h"
#include "gatresult.h"

#include "at.h"

static const char *none_prefix[] = { NULL };
static const char *creg_prefix[] = { "+CREG:", NULL };
static const char *cops_prefix[] = { "+COPS:", NULL };
static const char *csq_prefix[] = { "+CSQ:", NULL };

struct netreg_data {
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
};

static void extract_mcc_mnc(const char *str, char *mcc, char *mnc)
{
	/* Three digit country code */
	strncpy(mcc, str, OFONO_MAX_MCC_LENGTH);
	mcc[OFONO_MAX_MCC_LENGTH] = '\0';

	/* Usually a 2 but sometimes 3 digit network code */
	strncpy(mnc, str + OFONO_MAX_MCC_LENGTH, OFONO_MAX_MNC_LENGTH);
	mnc[OFONO_MAX_MNC_LENGTH] = '\0';
}

static void at_creg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_registration_status_cb_t cb = cbd->cb;
	int status;
	const char *str;
	int lac = -1, ci = -1, tech = -1;
	struct ofono_error error;

	dump_response("at_creg_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CREG:")) {
		DECLARE_FAILURE(e);

		cb(&e, -1, -1, -1, -1, cbd->data);
		return;
	}

	/* Skip <n> the unsolicited result code */
	g_at_result_iter_skip_next(&iter);

	g_at_result_iter_next_number(&iter, &status);

	if (g_at_result_iter_next_string(&iter, &str) == TRUE)
		lac = strtol(str, NULL, 16);
	else
		goto out;

	if (g_at_result_iter_next_string(&iter, &str) == TRUE)
		ci = strtol(str, NULL, 16);
	else
		goto out;

	g_at_result_iter_next_number(&iter, &tech);

out:
	ofono_debug("creg_cb: %d, %d, %d, %d", status, lac, ci, tech);

	cb(&error, status, lac, ci, tech, cbd->data);
}

static void at_registration_status(struct ofono_modem *modem,
					ofono_registration_status_cb_t cb,
					void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CREG?", creg_prefix,
				at_creg_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, -1, -1, -1, -1, data);
	}
}

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct at_data *at = ofono_modem_get_userdata(cbd->modem);
	ofono_current_operator_cb_t cb = cbd->cb;
	struct ofono_network_operator op;
	GAtResultIter iter;
	int format, tech;
	const char *name;
	struct ofono_error error;

	dump_response("cops_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok || at->netreg->mcc[0] == '\0' || at->netreg->mnc[0] == '\0') {
		cb(&error, NULL, cbd->data);
		goto out;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COPS:"))
		goto error;

	g_at_result_iter_skip_next(&iter);

	ok = g_at_result_iter_next_number(&iter, &format);

	if (ok == FALSE || format != 0)
		goto error;

	if (g_at_result_iter_next_string(&iter, &name) == FALSE)
		goto error;

	/* Default to GSM */
	if (g_at_result_iter_next_number(&iter, &tech) == FALSE)
		tech = 0;

	strncpy(op.name, name, OFONO_MAX_OPERATOR_NAME_LENGTH);
	op.name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

	strncpy(op.mcc, at->netreg->mcc, OFONO_MAX_MCC_LENGTH);
	op.mcc[OFONO_MAX_MCC_LENGTH] = '\0';

	strncpy(op.mnc, at->netreg->mnc, OFONO_MAX_MNC_LENGTH);
	op.mnc[OFONO_MAX_MNC_LENGTH] = '\0';

	op.status = -1;
	op.tech = tech;

	ofono_debug("cops_cb: %s, %s %s %d", name, at->netreg->mcc,
			at->netreg->mnc, tech);

	cb(&error, &op, cbd->data);

out:
	g_free(cbd);

	return;

error:
	{
		DECLARE_FAILURE(e);

		cb(&e, NULL, cbd->data);
	}

	g_free(cbd);
}

static void cops_numeric_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct at_data *at = ofono_modem_get_userdata(cbd->modem);
	GAtResultIter iter;
	const char *str;
	int format;

	dump_response("cops_numeric_cb", ok, result);

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COPS:"))
		goto error;

	g_at_result_iter_skip_next(&iter);

	ok = g_at_result_iter_next_number(&iter, &format);

	if (ok == FALSE || format != 2)
		goto error;

	if (g_at_result_iter_next_string(&iter, &str) == FALSE ||
		strlen(str) == 0)
		goto error;

	extract_mcc_mnc(str, at->netreg->mcc, at->netreg->mnc);

	ofono_debug("Cops numeric got mcc: %s, mnc: %s",
			at->netreg->mcc, at->netreg->mnc);

	return;

error:
	at->netreg->mcc[0] = '\0';
	at->netreg->mnc[0] = '\0';
}

static void at_current_operator(struct ofono_modem *modem,
				ofono_current_operator_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	gboolean ok;

	if (!cbd)
		goto error;

	ok = g_at_chat_send(at->parser, "AT+COPS=3,2", none_prefix,
				NULL, NULL, NULL);

	if (ok)
		ok = g_at_chat_send(at->parser, "AT+COPS?", cops_prefix,
					cops_numeric_cb, cbd, NULL);

	if (ok)
		ok = g_at_chat_send(at->parser, "AT+COPS=3,0", none_prefix,
					NULL, NULL, NULL);

	if (ok)
		ok = g_at_chat_send(at->parser, "AT+COPS?", cops_prefix,
					cops_cb, cbd, NULL);

	if (ok)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static void cops_list_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_operator_list_cb_t cb = cbd->cb;
	struct ofono_network_operator *list;
	GAtResultIter iter;
	int num = 0;
	struct ofono_error error;

	dump_response("cops_list_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, 0, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+COPS:")) {
		while (g_at_result_iter_skip_next(&iter))
			num += 1;
	}

	ofono_debug("Got %d elements", num);

	list = g_try_new0(struct ofono_network_operator, num);

	if (!list) {
		DECLARE_FAILURE(e);
		cb(&e, 0, NULL, cbd->data);
		return;
	}

	num = 0;
	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+COPS:")) {
		int status, tech;
		const char *l, *s, *n;
		gboolean have_long = FALSE;

		while (1) {
			if (!g_at_result_iter_open_list(&iter))
				break;

			if (!g_at_result_iter_next_number(&iter, &status))
				break;

			list[num].status = status;

			if (!g_at_result_iter_next_string(&iter, &l))
				break;

			if (strlen(l) > 0) {
				have_long = TRUE;
				strncpy(list[num].name, l,
					OFONO_MAX_OPERATOR_NAME_LENGTH);
			}

			if (!g_at_result_iter_next_string(&iter, &s))
				break;

			if (strlen(s) > 0 && !have_long)
				strncpy(list[num].name, s,
					OFONO_MAX_OPERATOR_NAME_LENGTH);

			list[num].name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

			if (!g_at_result_iter_next_string(&iter, &n))
				break;

			extract_mcc_mnc(n, list[num].mcc, list[num].mnc);

			if (!g_at_result_iter_next_number(&iter, &tech))
				tech = 0;

			list[num].tech = tech;

			if (!g_at_result_iter_close_list(&iter))
				break;

			num += 1;
		}
	}

	ofono_debug("Got %d operators", num);

{
	int i = 0;

	for (; i < num; i++) {
		ofono_debug("Operator: %s, %s, %s, status: %d, %d",
				list[i].name, list[i].mcc, list[i].mnc,
				list[i].status, list[i].tech);
	}
}

	cb(&error, num, list, cbd->data);

	g_free(list);
}

static void at_list_operators(struct ofono_modem *modem, ofono_operator_list_cb_t cb,
				void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+COPS=?", cops_prefix,
				cops_list_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, 0, NULL, data);
	}
}

static void register_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("register_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_register_auto(struct ofono_modem *modem, ofono_generic_cb_t cb,
				void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+COPS=0", none_prefix,
				register_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_register_manual(struct ofono_modem *modem,
				const struct ofono_network_operator *oper,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[128];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+COPS=1,2,\"%s%s\"", oper->mcc, oper->mnc);

	if (g_at_chat_send(at->parser, buf, none_prefix,
				register_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_deregister(struct ofono_modem *modem, ofono_generic_cb_t cb,
				void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+COPS=2", none_prefix,
				register_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void csq_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int strength;
	GAtResultIter iter;

	dump_response("csq_notify", TRUE, result);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSQ:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	ofono_debug("csq_notify: %d", strength);

	if (strength == 99)
		strength = -1;
	else
		strength = strength * 100 / 31;

	ofono_signal_strength_notify(modem, strength);
}

static void csq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_signal_strength_cb_t cb = cbd->cb;
	int strength;
	GAtResultIter iter;
	struct ofono_error error;

	dump_response("csq_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSQ:")) {
		DECLARE_FAILURE(e);

		cb(&e, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &strength);

	ofono_debug("csq_cb: %d", strength);

	if (strength == 99)
		strength = -1;
	else
		strength = strength * 100 / 31;

	cb(&error, strength, cbd->data);
}

static void at_signal_strength(struct ofono_modem *modem,
				ofono_signal_strength_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_get_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CSQ", csq_prefix,
				csq_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, -1, data);
	}
}

static void creg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GAtResultIter iter;
	int status;
	int lac = -1, ci = -1, tech = -1;
	const char *str;

	dump_response("creg_notify", TRUE, result);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CREG:"))
		return;

	g_at_result_iter_next_number(&iter, &status);

	if (g_at_result_iter_next_string(&iter, &str) == TRUE)
		lac = strtol(str, NULL, 16);
	else
		goto out;

	if (g_at_result_iter_next_string(&iter, &str) == TRUE)
		ci = strtol(str, NULL, 16);
	else
		goto out;

	g_at_result_iter_next_number(&iter, &tech);

out:
	ofono_debug("creg_notify: %d, %d, %d, %d", status, lac, ci, tech);

	ofono_network_registration_notify(modem, status, lac, ci, tech);
}

static struct ofono_network_registration_ops ops = {
	.registration_status 		= at_registration_status,
	.current_operator 		= at_current_operator,
	.list_operators			= at_list_operators,
	.register_auto			= at_register_auto,
	.register_manual		= at_register_manual,
	.deregister			= at_deregister,
	.signal_strength		= at_signal_strength,
};

static void at_network_registration_initialized(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_get_userdata(modem);

	if (!ok) {
		ofono_error("Unable to initialize Network Registration");
		return;
	}

	g_at_chat_register(at->parser, "+CREG:",
				creg_notify, FALSE, modem, NULL);
	g_at_chat_register(at->parser, "+CSQ:",
				csq_notify, FALSE, modem, NULL);

	ofono_network_registration_register(modem, &ops);
}

void at_network_registration_init(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_get_userdata(modem);

	at->netreg = g_try_new0(struct netreg_data, 1);

	if (!at->netreg)
		return;

	g_at_chat_send(at->parser, "AT+CREG=2", NULL,
				at_network_registration_initialized,
				modem, NULL);
}

void at_network_registration_exit(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_get_userdata(modem);

	if (!at->netreg)
		return;

	g_free(at->netreg);
	at->netreg = NULL;

	ofono_network_registration_unregister(modem);
}
