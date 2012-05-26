/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
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
#include <ofono/netreg.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"
#include "atmodem.h"
#include "vendor.h"

static const char *none_prefix[] = { NULL };
static const char *creg_prefix[] = { "+CREG:", NULL };
static const char *cops_prefix[] = { "+COPS:", NULL };
static const char *csq_prefix[] = { "+CSQ:", NULL };
static const char *cind_prefix[] = { "+CIND:", NULL };
static const char *zpas_prefix[] = { "+ZPAS:", NULL };
static const char *option_tech_prefix[] = { "_OCTI:", "_OUWCTI:", NULL };

struct netreg_data {
	GAtChat *chat;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int signal_index; /* If strength is reported via CIND */
	int signal_min; /* min strength reported via CIND */
	int signal_max; /* max strength reported via CIND */
	int signal_invalid; /* invalid strength reported via CIND */
	int tech;
	struct ofono_network_time time;
	guint nitz_timeout;
	unsigned int vendor;
};

struct tech_query {
	int status;
	int lac;
	int ci;
	struct ofono_netreg *netreg;
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

static int zte_parse_tech(GAtResult *result)
{
	GAtResultIter iter;
	const char *network, *domain;
	int tech;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+ZPAS:"))
		return -1;

	if (!g_at_result_iter_next_string(&iter, &network))
		return -1;

	if (!g_at_result_iter_next_string(&iter, &domain))
		return -1;

	if (g_str_equal(network, "GSM") == TRUE ||
			g_str_equal(network, "GPRS") == TRUE)
		tech = ACCESS_TECHNOLOGY_GSM;
	else if (g_str_equal(network, "EDGE") == TRUE)
		tech = ACCESS_TECHNOLOGY_GSM_EGPRS;
	else if (g_str_equal(network, "UMTS") == TRUE)
		tech = ACCESS_TECHNOLOGY_UTRAN;
	else if (g_str_equal(network, "HSDPA") == TRUE)
		tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA;
	else
		tech = -1;

	DBG("network %s domain %s tech %d", network, domain, tech);

	return tech;
}

static int option_parse_tech(GAtResult *result)
{
	GAtResultIter iter;
	int s, octi, ouwcti;
	int tech;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "_OCTI:"))
		return -1;

	if (!g_at_result_iter_next_number(&iter, &s))
		return -1;

	if (!g_at_result_iter_next_number(&iter, &octi))
		return -1;

	if (!g_at_result_iter_next(&iter, "_OUWCTI:"))
		return -1;

	if (!g_at_result_iter_next_number(&iter, &s))
		return -1;

	if (!g_at_result_iter_next_number(&iter, &ouwcti))
		return -1;

	switch (octi) {
	case 1: /* GSM */
		tech = ACCESS_TECHNOLOGY_GSM;
		break;
	case 2: /* GPRS */
		tech = ACCESS_TECHNOLOGY_GSM;
		break;
	case 3: /* EDGE */
		tech = ACCESS_TECHNOLOGY_GSM_EGPRS;
		break;
	default:
		tech = -1;
		break;
	}

	switch (ouwcti) {
	case 1: /* UMTS */
		tech = ACCESS_TECHNOLOGY_UTRAN;
		break;
	case 2: /* HSDPA */
		tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA;
		break;
	case 3: /* HSUPA */
		tech = ACCESS_TECHNOLOGY_UTRAN_HSUPA;
		break;
	case 4: /* HSPA */
		tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
		break;
	}

	DBG("octi %d ouwcti %d tech %d", octi, ouwcti, tech);

	return tech;
}

static void at_creg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	int status, lac, ci, tech;
	struct ofono_error error;
	struct netreg_data *nd = cbd->user;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, -1, cbd->data);
		return;
	}

	if (at_util_parse_reg(result, "+CREG:", NULL, &status,
				&lac, &ci, &tech, nd->vendor) == FALSE) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
		return;
	}

	if ((status == 1 || status == 5) && (tech == -1))
		tech = nd->tech;

	cb(&error, status, lac, ci, tech, cbd->data);
}

static void zte_tech_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_netreg *netreg = cbd->data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (ok)
		nd->tech = zte_parse_tech(result);
	else
		nd->tech = -1;
}

static void option_tech_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_netreg *netreg = cbd->data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (ok)
		nd->tech = option_parse_tech(result);
	else
		nd->tech = -1;
}

static void at_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = nd;

	switch (nd->vendor) {
	case OFONO_VENDOR_MBM:
		/*
		 * Send *ERINFO to find out the current tech, it will be
		 * intercepted in mbm_erinfo_notify
		 */
		g_at_chat_send(nd->chat, "AT*ERINFO?", none_prefix,
				NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_GOBI:
		/*
		 * Send *CNTI=0 to find out the current tech, it will be
		 * intercepted in gobi_cnti_notify
		 */
		g_at_chat_send(nd->chat, "AT*CNTI=0", none_prefix,
				NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_NOVATEL:
		/*
		 * Send $CNTI=0 to find out the current tech, it will be
		 * intercepted in nw_cnti_notify
		 */
		g_at_chat_send(nd->chat, "AT$CNTI=0", none_prefix,
				NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_ZTE:
		/*
		 * Send +ZPAS? to find out the current tech, zte_tech_cb
		 * will call, fire CREG? to do the rest.
		 */
		if (g_at_chat_send(nd->chat, "AT+ZPAS?", zpas_prefix,
					zte_tech_cb, cbd, NULL) == 0)
			nd->tech = -1;
		break;
	case OFONO_VENDOR_OPTION_HSO:
		/*
		 * Send AT_OCTI?;_OUWCTI? to find out the current tech,
		 * option_tech_cb will call, fire CREG? to do the rest.
		 */
		if (g_at_chat_send(nd->chat, "AT_OCTI?;_OUWCTI?",
					option_tech_prefix,
					option_tech_cb, cbd, NULL) == 0)
			nd->tech = -1;
		break;
	}

	if (g_at_chat_send(nd->chat, "AT+CREG?", creg_prefix,
				at_creg_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
}

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(cbd->user);
	ofono_netreg_operator_cb_t cb = cbd->cb;
	struct ofono_network_operator op;
	GAtResultIter iter;
	int format, tech;
	const char *name;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto error;

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
		tech = ACCESS_TECHNOLOGY_GSM;

	strncpy(op.name, name, OFONO_MAX_OPERATOR_NAME_LENGTH);
	op.name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

	strncpy(op.mcc, nd->mcc, OFONO_MAX_MCC_LENGTH);
	op.mcc[OFONO_MAX_MCC_LENGTH] = '\0';

	strncpy(op.mnc, nd->mnc, OFONO_MAX_MNC_LENGTH);
	op.mnc[OFONO_MAX_MNC_LENGTH] = '\0';

	/* Set to current */
	op.status = 2;
	op.tech = tech;

	DBG("cops_cb: %s, %s %s %d", name, nd->mcc, nd->mnc, tech);

	cb(&error, &op, cbd->data);
	g_free(cbd);

	return;

error:
	cb(&error, NULL, cbd->data);

	g_free(cbd);
}

static void cops_numeric_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(cbd->user);
	ofono_netreg_operator_cb_t cb = cbd->cb;
	GAtResultIter iter;
	const char *str;
	int format;
	int len;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COPS:"))
		goto error;

	g_at_result_iter_skip_next(&iter);

	ok = g_at_result_iter_next_number(&iter, &format);

	if (ok == FALSE || format != 2)
		goto error;

	if (g_at_result_iter_next_string(&iter, &str) == FALSE)
		goto error;

	len = strspn(str, "0123456789");

	if (len != 5 && len != 6)
		goto error;

	extract_mcc_mnc(str, nd->mcc, nd->mnc);

	DBG("Cops numeric got mcc: %s, mnc: %s", nd->mcc, nd->mnc);

	ok = g_at_chat_send(nd->chat, "AT+COPS=3,0", none_prefix,
					NULL, NULL, NULL);

	if (ok)
		ok = g_at_chat_send(nd->chat, "AT+COPS?", cops_prefix,
					cops_cb, cbd, NULL);

	if (ok)
		return;

error:
	cb(&error, NULL, cbd->data);
	g_free(cbd);
}

static void at_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);
	gboolean ok;

	cbd->user = netreg;

	/* Nokia modems have a broken return value for the string
	 * returned for the numeric value. It misses a " at the end.
	 * Trying to read this will stall the parser. So skip it. */
	if (nd->vendor == OFONO_VENDOR_NOKIA) {
		ok = g_at_chat_send(nd->chat, "AT+COPS=3,0", none_prefix,
							NULL, NULL, NULL);

		if (ok)
			ok = g_at_chat_send(nd->chat, "AT+COPS?", cops_prefix,
							cops_cb, cbd, NULL);
	} else {
		ok = g_at_chat_send(nd->chat, "AT+COPS=3,2", none_prefix,
							NULL, NULL, NULL);

		if (ok)
			ok = g_at_chat_send(nd->chat, "AT+COPS?", cops_prefix,
						cops_numeric_cb, cbd, NULL);
	}

	if (ok)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void cops_list_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct ofono_network_operator *list;
	GAtResultIter iter;
	int num = 0;
	struct ofono_error error;

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

	DBG("Got %d elements", num);

	list = g_try_new0(struct ofono_network_operator, num);
	if (list == NULL) {
		CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
		return;
	}

	num = 0;
	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+COPS:")) {
		int status, tech, plmn;
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
				tech = ACCESS_TECHNOLOGY_GSM;

			list[num].tech = tech;

			if (!g_at_result_iter_next_number(&iter, &plmn))
				plmn = 0;

			if (!g_at_result_iter_close_list(&iter))
				break;

			num += 1;
		}
	}

	DBG("Got %d operators", num);

{
	int i = 0;

	for (; i < num; i++) {
		DBG("Operator: %s, %s, %s, status: %d, %d",
			list[i].name, list[i].mcc, list[i].mnc,
			list[i].status, list[i].tech);
	}
}

	cb(&error, num, list, cbd->data);

	g_free(list);
}

static void at_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(nd->chat, "AT+COPS=?", cops_prefix,
				cops_list_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
}

static void register_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_register_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(nd->chat, "AT+COPS=0", none_prefix,
				register_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[128];

	snprintf(buf, sizeof(buf), "AT+COPS=1,2,\"%s%s\"", mcc, mnc);

	if (g_at_chat_send(nd->chat, buf, none_prefix,
				register_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void csq_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int strength;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSQ:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	ofono_netreg_strength_notify(netreg,
				at_util_convert_signal_strength(strength));
}

static void calypso_csq_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int strength;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%CSQ:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	ofono_netreg_strength_notify(netreg,
				at_util_convert_signal_strength(strength));
}

static void option_osigq_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int strength;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "_OSIGQ:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	ofono_netreg_strength_notify(netreg,
				at_util_convert_signal_strength(strength));
}

static void ifx_xhomezr_notify(GAtResult *result, gpointer user_data)
{
	//struct ofono_netreg *netreg = user_data;
	const char *label;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XHOMEZR:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &label))
		return;

	ofono_info("Home zone: %s", label);
}

static void ifx_xciev_notify(GAtResult *result, gpointer user_data)
{
	//struct ofono_netreg *netreg = user_data;
	int ind;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XCIEV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &ind))
		return;

	DBG("ind %d", ind);

	/*
	 * Radio signal strength indicators are defined for 0-7,
	 * but this notification seems to return CSQ 0-31,99 values.
	 *
	 * Ignore this indication for now since it can not be trusted.
	 */
}

static void ifx_xcsq_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int rssi, ber, strength;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XCSQ:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &rssi))
		return;

	if (!g_at_result_iter_next_number(&iter, &ber))
		return;

	DBG("rssi %d ber %d", rssi, ber);

	if (rssi == 99)
		strength = -1;
	else
		strength = (rssi * 100) / 31;

	ofono_netreg_strength_notify(netreg, strength);
}

static void ciev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int strength, ind;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIEV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &ind))
		return;

	if (ind != nd->signal_index)
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	if (strength == nd->signal_invalid)
		strength = -1;
	else
		strength = (strength * 100) / (nd->signal_max - nd->signal_min);

	ofono_netreg_strength_notify(netreg, strength);
}

static void ctzv_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	const char *tz;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CTZV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &tz))
		return;

	DBG("tz %s", tz);

	nd->time.utcoff = atoi(tz) * 15 * 60;

	ofono_netreg_time_notify(netreg, &nd->time);
}

static void tlts_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int year, mon, mday, hour, min, sec;
	char tz[4];
	const char *time;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "*TLTS:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &time))
		return;

	DBG("time %s", time);

	if (sscanf(time, "%02u/%02u/%02u,%02u:%02u:%02u%s", &year, &mon, &mday,
						&hour, &min, &sec, tz) != 7)
                return;

        nd->time.sec = sec;
        nd->time.min = min;
        nd->time.hour = hour;
        nd->time.mday = mday;
        nd->time.mon = mon;
        nd->time.year = 2000 + year;

	nd->time.utcoff = atoi(tz) * 15 * 60;

	ofono_netreg_time_notify(netreg, &nd->time);
}

static gboolean notify_time(gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	nd->nitz_timeout = 0;

	ofono_netreg_time_notify(netreg, &nd->time);

	return FALSE;
}

static void ifx_ctzv_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int year, mon, mday, hour, min, sec;
	const char *tz, *time;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CTZV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &tz))
		return;

	if (!g_at_result_iter_next_string(&iter, &time))
		return;

	DBG("tz %s time %s", tz, time);

	if (sscanf(time, "%u/%u/%u,%u:%u:%u", &year, &mon, &mday,
						&hour, &min, &sec) != 6)
		return;

	nd->time.sec = sec;
	nd->time.min = min;
	nd->time.hour = hour;
	nd->time.mday = mday;
	nd->time.mon = mon;
	nd->time.year = 2000 + year;

	if (nd->nitz_timeout > 0)
		g_source_remove(nd->nitz_timeout);

	nd->nitz_timeout = g_timeout_add_seconds(1, notify_time, user_data);
}

static void ifx_ctzdst_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int dst;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CTZDST:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &dst))
		return;

	DBG("dst %d", dst);

	nd->time.dst = dst;

	if (nd->nitz_timeout > 0) {
		g_source_remove(nd->nitz_timeout);
		nd->nitz_timeout = 0;
	}

	ofono_netreg_time_notify(netreg, &nd->time);
}

static void cind_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	int index;
	int strength;
	GAtResultIter iter;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIND:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	for (index = 1; index < nd->signal_index; index++)
		g_at_result_iter_skip_next(&iter);

	g_at_result_iter_next_number(&iter, &strength);

	if (strength == nd->signal_invalid)
		strength = -1;
	else
		strength = (strength * 100) / (nd->signal_max - nd->signal_min);

	cb(&error, strength, cbd->data);
}

static void huawei_rssi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	GAtResultIter iter;
	int strength;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^RSSI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	ofono_netreg_strength_notify(netreg,
				at_util_convert_signal_strength(strength));
}

static void huawei_mode_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	int mode, submode;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^MODE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	if (!g_at_result_iter_next_number(&iter, &submode))
		return;

	switch (mode) {
	case 3:
		nd->tech = ACCESS_TECHNOLOGY_GSM;
		break;
	case 5:
		nd->tech = ACCESS_TECHNOLOGY_UTRAN;
		break;
	}
}

static void huawei_nwtime_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int year, mon, mday, hour, min, sec;
	char tz[4];
	const char *date, *time, *dst;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^NWTIME:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &date))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &time))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &dst))
		return;

	DBG("date %s time %s dst %s", date, time, dst);

	if (sscanf(date, "%u/%u/%u", &year, &mon, &mday) != 3)
		return;

	if (sscanf(time, "%u:%u:%u%s", &hour, &min, &sec, tz) != 4)
		return;

	nd->time.utcoff = atoi(tz) * 15 * 60;
	nd->time.dst = atoi(dst);

	nd->time.sec = sec;
	nd->time.min = min;
	nd->time.hour = hour;
	nd->time.mday = mday;
	nd->time.mon = mon;
	nd->time.year = 2000 + year;

	ofono_netreg_time_notify(netreg, &nd->time);
}

static void csq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	int strength;
	GAtResultIter iter;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSQ:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &strength);

	DBG("csq_cb: %d", strength);

	if (strength == 99)
		strength = -1;
	else
		strength = (strength * 100) / 31;

	cb(&error, strength, cbd->data);
}

static void at_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = nd;

	/*
	 * If we defaulted to using CIND, then keep using it,
	 * otherwise fall back to CSQ
	 */
	if (nd->signal_index > 0) {
		if (g_at_chat_send(nd->chat, "AT+CIND?", cind_prefix,
					cind_cb, cbd, g_free) > 0)
			return;
	} else {
		if (g_at_chat_send(nd->chat, "AT+CSQ", csq_prefix,
				csq_cb, cbd, g_free) > 0)
			return;
	}

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void mbm_etzv_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int year, mon, mday, hour, min, sec;
	const char *tz, *time, *timestamp;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*ETZV:") == FALSE)
		return;

	if (g_at_result_iter_next_string(&iter, &tz) == FALSE)
		return;

	if (g_at_result_iter_next_string(&iter, &time) == FALSE)
		time = NULL;

	if (g_at_result_iter_next_string(&iter, &timestamp) == FALSE)
		timestamp = NULL;

	DBG("tz %s time %s timestamp %s", tz, time, timestamp);

	if (time == NULL) {
		year = -1;
		mon = -1;
		mday = -1;
		hour = -1;
		min = -1;
		sec = -1;
	} else {
		if (sscanf(time, "%u/%u/%u,%u:%u:%u", &year, &mon, &mday,
						&hour, &min, &sec) != 6)
		return;
	}

	nd->time.utcoff = atoi(tz) * 15 * 60;

	nd->time.sec = sec;
	nd->time.min = min;
	nd->time.hour = hour;
	nd->time.mday = mday;
	nd->time.mon = mon;
	nd->time.year = year;

	ofono_netreg_time_notify(netreg, &nd->time);
}

static void mbm_erinfo_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	int mode, gsm, umts;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*ERINFO:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &mode) == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &gsm) == FALSE)
		return;

	/*
	 * According to MBM the ERINFO unsolicited response does not contain
	 * the mode parameter, however at least the MD300 does report it.  So
	 * we handle both 2 and 3 argument versions
	 */
	if (g_at_result_iter_next_number(&iter, &umts) == FALSE) {
		gsm = mode;
		umts = gsm;
	}

	ofono_info("network capability: GSM %d UMTS %d", gsm, umts);

	/* Convert to tech values from 27.007 */
	switch (gsm) {
	case 1: /* GSM */
		nd->tech = ACCESS_TECHNOLOGY_GSM;
		break;
	case 2: /* EDGE */
		nd->tech = ACCESS_TECHNOLOGY_GSM_EGPRS;
		break;
	default:
		nd->tech = -1;
	}

	switch (umts) {
	case 1: /* UMTS */
		nd->tech = ACCESS_TECHNOLOGY_UTRAN;
		break;
	case 2: /* UMTS + HSDPA */
		nd->tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA;
		break;
	}
}

static void icera_nwstate_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	const char *mccmnc, *tech, *state;
	int rssi;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "%NWSTATE:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &rssi) == FALSE)
		return;

	if (g_at_result_iter_next_unquoted_string(&iter, &mccmnc) == FALSE)
		return;

	if (g_at_result_iter_next_unquoted_string(&iter, &tech) == FALSE)
		return;

	if (g_at_result_iter_next_unquoted_string(&iter, &state) == FALSE)
		return;

	DBG("rssi %d tech %s state %s", rssi, tech, state);

	/* small 'g' means CS, big 'G' means PS */
	if (g_str_equal(tech, "2g") == TRUE ||
				g_str_equal(tech, "2G") == TRUE ||
				g_str_equal(tech, "2G-GPRS") == TRUE) {
		nd->tech = ACCESS_TECHNOLOGY_GSM;
	} else if (g_str_equal(tech, "2G-EDGE") == TRUE) {
		nd->tech = ACCESS_TECHNOLOGY_GSM_EGPRS;
	} else if (g_str_equal(tech, "3g") == TRUE ||
				g_str_equal(tech, "3G") == TRUE ||
				g_str_equal(tech, "R99") == TRUE) {
		if (g_str_equal(state, "HSDPA") == TRUE)
			nd->tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA;
		else if (g_str_equal(state, "HSUPA") == TRUE)
			nd->tech = ACCESS_TECHNOLOGY_UTRAN_HSUPA;
		else if (g_str_equal(state, "HSDPA-HSUPA") == TRUE)
			nd->tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
		else if (g_str_equal(state, "HSDPA-HSUPA-HSPA+") == TRUE)
			nd->tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
		else
			nd->tech = ACCESS_TECHNOLOGY_UTRAN;
	} else
		nd->tech = -1;
}

static int cnti_to_tech(const char *cnti)
{
	if (g_str_equal(cnti, "GSM") == TRUE ||
			g_str_equal(cnti, "GPRS") == TRUE)
		return ACCESS_TECHNOLOGY_GSM;
	else if (g_str_equal(cnti, "EDGE") == TRUE)
		return ACCESS_TECHNOLOGY_GSM_EGPRS;
	else if (g_str_equal(cnti, "UMTS") == TRUE)
		return ACCESS_TECHNOLOGY_UTRAN;
	else if (g_str_equal(cnti, "HSDPA") == TRUE)
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA;
	else if (g_str_equal(cnti, "HSUPA") == TRUE)
		return ACCESS_TECHNOLOGY_UTRAN_HSUPA;

	return -1;
}

static void gobi_cnti_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	const char *tech;
	int option;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "*CNTI:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &option) == FALSE)
		return;

	if (option != 0)
		return;

	if (g_at_result_iter_next_unquoted_string(&iter, &tech) == FALSE)
		return;

	nd->tech = cnti_to_tech(tech);
}

static void nw_cnti_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	const char *tech;
	int option;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "$CNTI:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &option) == FALSE)
		return;

	if (option != 0)
		return;

	if (g_at_result_iter_next_unquoted_string(&iter, &tech) == FALSE)
		return;

	nd->tech = cnti_to_tech(tech);
}

static void cnti_query_tech_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct tech_query *tq = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(tq->netreg);

	ofono_netreg_status_notify(tq->netreg,
			tq->status, tq->lac, tq->ci, nd->tech);
}

static void zte_query_tech_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct tech_query *tq = user_data;
	int tech;

	if (ok)
		tech = zte_parse_tech(result);
	else
		tech = -1;

	ofono_netreg_status_notify(tq->netreg,
			tq->status, tq->lac, tq->ci, tech);
}

static void option_query_tech_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct tech_query *tq = user_data;
	int tech;

	if (ok)
		tech = option_parse_tech(result);
	else
		tech = -1;

	ofono_netreg_status_notify(tq->netreg,
			tq->status, tq->lac, tq->ci, tech);
}

static void creg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int status, lac, ci, tech;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct tech_query *tq;

	if (at_util_parse_reg_unsolicited(result, "+CREG:", &status,
				&lac, &ci, &tech, nd->vendor) == FALSE)
		return;

	if (status != 1 && status != 5)
		goto notify;

	tq = g_try_new0(struct tech_query, 1);
	if (tq == NULL)
		goto notify;

	tq->status = status;
	tq->lac = lac;
	tq->ci = ci;
	tq->netreg = netreg;

	switch (nd->vendor) {
	case OFONO_VENDOR_GOBI:
		if (g_at_chat_send(nd->chat, "AT*CNTI=0", none_prefix,
					cnti_query_tech_cb, tq, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_NOVATEL:
		if (g_at_chat_send(nd->chat, "AT$CNTI=0", none_prefix,
					cnti_query_tech_cb, tq, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_ZTE:
		if (g_at_chat_send(nd->chat, "AT+ZPAS?", zpas_prefix,
					zte_query_tech_cb, tq, g_free) > 0)
			return;
		break;
	case OFONO_VENDOR_OPTION_HSO:
		if (g_at_chat_send(nd->chat, "AT_OCTI?;_OUWCTI?",
					option_tech_prefix,
					option_query_tech_cb, tq, g_free) > 0)
			return;
		break;
	}

	g_free(tq);

	if ((status == 1 || status == 5) && tech == -1)
		tech = nd->tech;

notify:
	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void cind_support_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	const char *str;
	char *signal_identifier = "signal";
	int index;
	int min = 0;
	int max = 0;
	int tmp_min, tmp_max, invalid;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	/*
	 * Telit encapsulates the CIND=? tokens with braces
	 * so we need to skip them
	 */
	if (nd->vendor == OFONO_VENDOR_TELIT) {
		g_at_result_iter_open_list(&iter);
		signal_identifier = "rssi";
	}

	while (g_at_result_iter_open_list(&iter)) {
		/* Reset invalid default value for every token */
		invalid = 99;

		if (!g_at_result_iter_next_string(&iter, &str))
			goto error;

		if (!g_at_result_iter_open_list(&iter))
			goto error;

		while (g_at_result_iter_next_range(&iter, &tmp_min, &tmp_max)) {
			if (tmp_min != tmp_max) {
				min = tmp_min;
				max = tmp_max;
			} else
				invalid = tmp_min;
		}

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (g_str_equal(signal_identifier, str) == TRUE) {
			nd->signal_index = index;
			nd->signal_min = min;
			nd->signal_max = max;
			nd->signal_invalid = invalid;
		}

		index += 1;
	}

	if (nd->vendor == OFONO_VENDOR_TELIT)
		g_at_result_iter_close_list(&iter);

	if (nd->signal_index == 0)
		goto error;

	g_at_chat_send(nd->chat, "AT+CMER=3,0,0,1", NULL,
			NULL, NULL, NULL);
	g_at_chat_register(nd->chat, "+CIEV:",
				ciev_notify, FALSE, netreg, NULL);
	g_at_chat_register(nd->chat, "+CREG:",
				creg_notify, FALSE, netreg, NULL);

	ofono_netreg_register(netreg);
	return;

error:
	ofono_error("This driver is not setup with Signal Strength reporting"
			" via CIND indications, please write proper netreg"
			" handling for this device");

	ofono_netreg_remove(netreg);
}

static void at_creg_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (!ok) {
		ofono_error("Unable to initialize Network Registration");
		ofono_netreg_remove(netreg);
		return;
	}

	switch (nd->vendor) {
	case OFONO_VENDOR_PHONESIM:
		g_at_chat_register(nd->chat, "+CSQ:",
					csq_notify, FALSE, netreg, NULL);
		break;
	case OFONO_VENDOR_CALYPSO:
		g_at_chat_send(nd->chat, "AT%CSQ=1", none_prefix,
				NULL, NULL, NULL);
		g_at_chat_register(nd->chat, "%CSQ:", calypso_csq_notify,
					FALSE, netreg, NULL);
		break;
	case OFONO_VENDOR_OPTION_HSO:
		g_at_chat_send(nd->chat, "AT_OSSYS=1", none_prefix,
				NULL, NULL, NULL);
		g_at_chat_send(nd->chat, "AT_OSQI=1", none_prefix,
				NULL, NULL, NULL);
		g_at_chat_register(nd->chat, "_OSIGQ:", option_osigq_notify,
					FALSE, netreg, NULL);

		g_at_chat_send(nd->chat, "AT_OSSYS?", none_prefix,
				NULL, NULL, NULL);
		g_at_chat_send(nd->chat, "AT_OSQI?", none_prefix,
				NULL, NULL, NULL);

		/* Register for network time update reports */
		g_at_chat_register(nd->chat, "+CTZV:", ctzv_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT+CTZR=1", none_prefix,
						NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_MBM:
		/* Enable network registration updates */
		g_at_chat_send(nd->chat, "AT*E2REG=1", none_prefix,
						NULL, NULL, NULL);
		g_at_chat_send(nd->chat, "AT*EREG=2", none_prefix,
						NULL, NULL, NULL);
		g_at_chat_send(nd->chat, "AT*EPSB=1", none_prefix,
						NULL, NULL, NULL);

		/* Register for network technology updates */
		g_at_chat_send(nd->chat, "AT*ERINFO=1", none_prefix,
						NULL, NULL, NULL);
		g_at_chat_register(nd->chat, "*ERINFO:", mbm_erinfo_notify,
						FALSE, netreg, NULL);

		/* Register for network time update reports */
		g_at_chat_register(nd->chat, "*ETZV:", mbm_etzv_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT*ETZR=2", none_prefix,
						NULL, NULL, NULL);

		g_at_chat_send(nd->chat, "AT+CIND=?", cind_prefix,
					cind_support_cb, netreg, NULL);
		return;
	case OFONO_VENDOR_GOBI:
		/*
		 * Gobi devices don't support unsolicited notifications
		 * of technology changes, but register a handle for
		 * CNTI so we get notified by any query.
		 */
		g_at_chat_register(nd->chat, "*CNTI:", gobi_cnti_notify,
					FALSE, netreg, NULL);
		break;
	case OFONO_VENDOR_NOVATEL:
		/*
		 * Novatel doesn't support unsolicited notifications
		 * of technology changes, but register a handle for
		 * CNTI so we get notified by any query.
		 */
		g_at_chat_register(nd->chat, "$CNTI:", nw_cnti_notify,
					FALSE, netreg, NULL);
		break;
	case OFONO_VENDOR_HUAWEI:
		/* Register for RSSI reports */
		g_at_chat_register(nd->chat, "^RSSI:", huawei_rssi_notify,
						FALSE, netreg, NULL);

		/* Register for system mode reports */
		g_at_chat_register(nd->chat, "^MODE:", huawei_mode_notify,
						FALSE, netreg, NULL);

		/* Register for network time reports */
		g_at_chat_register(nd->chat, "^NWTIME:", huawei_nwtime_notify,
						FALSE, netreg, NULL);
		break;
	case OFONO_VENDOR_IFX:
		/* Register for specific signal strength reports */
		g_at_chat_register(nd->chat, "+XCIEV:", ifx_xciev_notify,
						FALSE, netreg, NULL);
		g_at_chat_register(nd->chat, "+XCSQ:", ifx_xcsq_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT+XCSQ=1", none_prefix,
						NULL, NULL, NULL);
		g_at_chat_send(nd->chat, "AT+XMER=1", none_prefix,
						NULL, NULL, NULL);

		/* Register for home zone reports */
		g_at_chat_register(nd->chat, "+XHOMEZR:", ifx_xhomezr_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT+XHOMEZR=1", none_prefix,
						NULL, NULL, NULL);

		/* Register for network time update reports */
		g_at_chat_register(nd->chat, "+CTZV:", ifx_ctzv_notify,
						FALSE, netreg, NULL);
		g_at_chat_register(nd->chat, "+CTZDST:", ifx_ctzdst_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT+CTZR=1", none_prefix,
						NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_ZTE:
		/* Register for network time update reports */
		g_at_chat_register(nd->chat, "+CTZV:", ctzv_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT+CTZR=1", none_prefix,
						NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_ICERA:
		/* Register for network technology updates */
		g_at_chat_register(nd->chat, "%NWSTATE:", icera_nwstate_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT%NWSTATE=1", none_prefix,
						NULL, NULL, NULL);

		/* Register for radio access technology updates */
		g_at_chat_send(nd->chat, "AT*TRATD=1", none_prefix,
						NULL, NULL, NULL);

		/* Register for network time update reports */
		g_at_chat_register(nd->chat, "*TLTS:", tlts_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->chat, "AT*TLTS=1", none_prefix,
						NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_NOKIA:
	case OFONO_VENDOR_SAMSUNG:
	case OFONO_VENDOR_SIMCOM:
		/* Signal strength reporting via CIND is not supported */
		break;
	default:
		g_at_chat_send(nd->chat, "AT+CIND=?", cind_prefix,
				cind_support_cb, netreg, NULL);
		return;
	}

	g_at_chat_register(nd->chat, "+CREG:",
				creg_notify, FALSE, netreg, NULL);
	ofono_netreg_register(netreg);
}

static void at_creg_test_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	gint range[2];
	GAtResultIter iter;
	int creg1 = 0;
	int creg2 = 0;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

retry:
	if (!g_at_result_iter_next(&iter, "+CREG:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto retry;

	while (g_at_result_iter_next_range(&iter, &range[0], &range[1])) {
		if (1 >= range[0] && 1 <= range[1])
			creg1 = 1;
		if (2 >= range[0] && 2 <= range[1])
			creg2 = 1;
	}

	g_at_result_iter_close_list(&iter);

	if (creg2) {
		g_at_chat_send(nd->chat, "AT+CREG=2", none_prefix,
				at_creg_set_cb, netreg, NULL);
		return;
	}

	if (creg1) {
		g_at_chat_send(nd->chat, "AT+CREG=1", none_prefix,
				at_creg_set_cb, netreg, NULL);
		return;
	}

error:
	ofono_error("Unable to initialize Network Registration");
	ofono_netreg_remove(netreg);
}

static int at_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct netreg_data *nd;

	nd = g_new0(struct netreg_data, 1);

	nd->chat = g_at_chat_clone(chat);
	nd->vendor = vendor;
	nd->tech = -1;
	nd->time.sec = -1;
	nd->time.min = -1;
	nd->time.hour = -1;
	nd->time.mday = -1;
	nd->time.mon = -1;
	nd->time.year = -1;
	nd->time.dst = 0;
	nd->time.utcoff = 0;
	ofono_netreg_set_data(netreg, nd);

	g_at_chat_send(nd->chat, "AT+CREG=?", creg_prefix,
			at_creg_test_cb, netreg, NULL);

	return 0;
}

static void at_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (nd->nitz_timeout)
		g_source_remove(nd->nitz_timeout);

	ofono_netreg_set_data(netreg, NULL);

	g_at_chat_unref(nd->chat);
	g_free(nd);
}

static struct ofono_netreg_driver driver = {
	.name				= "atmodem",
	.probe				= at_netreg_probe,
	.remove				= at_netreg_remove,
	.registration_status		= at_registration_status,
	.current_operator		= at_current_operator,
	.list_operators			= at_list_operators,
	.register_auto			= at_register_auto,
	.register_manual		= at_register_manual,
	.strength			= at_signal_strength,
};

void at_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void at_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
