/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2012  Canonical Ltd.
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

#include "common.h"
#include "gril.h"
#include "rilmodem.h"

struct netreg_data {
	GRil *ril;
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

/* 27.007 Section 7.3 <stat> */
enum operator_status {
	OPERATOR_STATUS_UNKNOWN =	0,
	OPERATOR_STATUS_AVAILABLE =	1,
	OPERATOR_STATUS_CURRENT =	2,
	OPERATOR_STATUS_FORBIDDEN =	3,
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

/*
 * TODO: The functions in this file are stubbed out, and
 * will need to be re-worked to talk to the /gril layer
 * in order to get real values from RILD.
 */

static void ril_creg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_error error;
	int status, lac, ci, tech;

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		decode_ril_error(&error, "FAIL");
		ofono_error("Failed to pull registration state");
		cb(&error, -1, -1, -1, -1, cbd->data);
		return;
	}

	decode_ril_error(&error, "OK");

	if (ril_util_parse_reg(nd->ril, message, &status,
				&lac, &ci, &tech, NULL) == FALSE) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
		return;
	}

	nd->tech = tech;
	cb(&error, status, lac, ci, tech, cbd->data);
}

static void ril_creg_notify(struct ofono_error *error, int status, int lac,
					int ci, int tech, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;

        if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
                DBG("Error during status notification");
                return;
        }

	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void ril_network_state_change(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(ril_creg_notify, netreg, nd);
	int request = RIL_REQUEST_VOICE_REGISTRATION_STATE;
	int ret;

	if (message->req != RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED)
		goto error;

	g_ril_print_unsol_no_args(nd->ril, message);

	ret = g_ril_send(nd->ril, request, NULL,
				0, ril_creg_cb, cbd, g_free);

	/* For operator update ofono will use the current_operator cb
	 * so we don't need to probe ril here */

	g_ril_print_request_no_args(nd->ril, ret, request);

	if (ret > 0)
		return;

error:
	ofono_error("Unable to request network state changed");
	g_free(cbd);
}

static void ril_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);
	int request = RIL_REQUEST_VOICE_REGISTRATION_STATE;
	int ret;

	ret = g_ril_send(nd->ril, request, NULL,
				0, ril_creg_cb, cbd, g_free);

	g_ril_print_request_no_args(nd->ril, ret, request);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
	}
}

static void ril_cops_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_error error;
	struct parcel rilp;
	struct ofono_network_operator op;
	gchar *lalpha, *salpha, *numeric;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("Failed to retrive the current operator");
		goto error;
	}

	ril_util_init_parcel(message, &rilp);

	/* Size of char ** */
	if (parcel_r_int32(&rilp) == 0)
		goto error;

	lalpha = parcel_r_string(&rilp);
	salpha = parcel_r_string(&rilp);
	numeric = parcel_r_string(&rilp);

	/* Try to use long by default */
	if (lalpha)
		strncpy(op.name, lalpha, OFONO_MAX_OPERATOR_NAME_LENGTH);
	else
		strncpy(op.name, salpha, OFONO_MAX_OPERATOR_NAME_LENGTH);

	extract_mcc_mnc(numeric, op.mcc, op.mnc);

	/* Set to current */
	op.status = OPERATOR_STATUS_CURRENT;
	op.tech = nd->tech;

	g_ril_append_print_buf(nd->ril,
				"(lalpha=%s, salpha=%s, numeric=%s, %s, mcc=%s, mnc=%s, %s)",
				lalpha, salpha, numeric,
				op.name, op.mcc, op.mnc,
				registration_tech_to_string(op.tech));
	g_ril_print_response(nd->ril, message);

	g_free(lalpha);
	g_free(salpha);
	g_free(numeric);

	cb(&error, &op, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);
	int request = RIL_REQUEST_OPERATOR;
	int ret;

	ret = g_ril_send(nd->ril, request, NULL,
				0, ril_cops_cb, cbd, g_free);

	g_ril_print_request_no_args(nd->ril, ret, request);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static void ril_cops_list_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_network_operator *list;
	struct ofono_error error;
	struct parcel rilp;
	int noperators, i;
	gchar *lalpha, *salpha, *numeric, *status;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("Failed to retrive the list of operators");
		goto error;
	}

	ril_util_init_parcel(message, &rilp);

	g_ril_append_print_buf(nd->ril, "{");

	/* Number of operators at the list (4 strings for every operator) */
	noperators = parcel_r_int32(&rilp) / 4;
	DBG("noperators = %d", noperators);

	list = g_try_new0(struct ofono_network_operator, noperators);
	if (list == NULL)
		goto error;

	for (i = 0; i < noperators; i++) {
		lalpha = parcel_r_string(&rilp);
		salpha = parcel_r_string(&rilp);
		numeric = parcel_r_string(&rilp);
		status = parcel_r_string(&rilp);

		/* Try to use long by default */
		if (lalpha) {
			strncpy(list[i].name, lalpha,
					OFONO_MAX_OPERATOR_NAME_LENGTH);
		} else {
			strncpy(list[i].name, salpha,
					OFONO_MAX_OPERATOR_NAME_LENGTH);
		}

		extract_mcc_mnc(numeric, list[i].mcc, list[i].mnc);

		/* FIXME: need to fix this for CDMA */
		/* Use GSM as default, as RIL doesn't pass that info to us */
		list[i].tech = ACCESS_TECHNOLOGY_GSM;

		/* Set the proper status  */
		if (!strcmp(status, "unknown"))
			list[i].status = OPERATOR_STATUS_UNKNOWN;
		else if (!strcmp(status, "available"))
			list[i].status = OPERATOR_STATUS_AVAILABLE;
		else if (!strcmp(status, "current"))
			list[i].status = OPERATOR_STATUS_CURRENT;
		else if (!strcmp(status, "forbidden"))
			list[i].status = OPERATOR_STATUS_FORBIDDEN;

		g_ril_append_print_buf(nd->ril,
					"%s [operator=%s, %s, %s, status: %s]",
					print_buf,
					list[i].name, list[i].mcc,
					list[i].mnc, status);

		g_free(lalpha);
		g_free(salpha);
		g_free(numeric);
		g_free(status);
	}

	g_ril_append_print_buf(nd->ril, "%s}", print_buf);
	g_ril_print_response(nd->ril, message);

	cb(&error, noperators, list, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
}

static void ril_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);
	int request = RIL_REQUEST_QUERY_AVAILABLE_NETWORKS;
	int ret;

	ret = g_ril_send(nd->ril, request, NULL,
				0, ril_cops_list_cb, cbd, g_free);

	g_ril_print_request_no_args(nd->ril, ret, request);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
	}
}

static void ril_register_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_register_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_error error;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");

		g_ril_print_response_no_args(nd->ril, message);

	} else {
		decode_ril_error(&error, "FAIL");
	}

	cb(&error, cbd->data);
}

static void ril_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);
	int request = RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC;
	int ret;

	ret = g_ril_send(nd->ril, request,
				NULL, 0, ril_register_cb, cbd, g_free);

	g_ril_print_request_no_args(nd->ril, ret, request);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);
	char buf[OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 1];
	struct parcel rilp;
	int request = RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL;
	int ret;

	parcel_init(&rilp);

	/* RIL expects a char * specifying MCCMNC of network to select */
	snprintf(buf, sizeof(buf), "%s%s", mcc, mnc);
	parcel_w_string(&rilp, buf);

	ret = g_ril_send(nd->ril, request,
				rilp.data, rilp.size, ril_register_cb,
				cbd, g_free);
	parcel_free(&rilp);

	g_ril_append_print_buf(nd->ril,	"(%s)", buf);
	g_ril_print_request(nd->ril, ret, request);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_strength_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int strength;

	g_assert(message->req == RIL_UNSOL_SIGNAL_STRENGTH);

	strength = ril_util_get_signal(nd->ril, message);
	ofono_netreg_strength_notify(netreg, strength);

	return;
}

static void ril_strength_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_error error;
	int strength;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("Failed to retrive the signal strength");
		goto error;
	}

	strength = ril_util_get_signal(nd->ril, message);
	cb(&error, strength, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);
	int request = RIL_REQUEST_SIGNAL_STRENGTH;
	int ret;

	ret = g_ril_send(nd->ril, request,
				NULL, 0, ril_strength_cb, cbd, g_free);

	g_ril_print_request_no_args(nd->ril, ret, request);

	if (ret <= 0) {
		ofono_error("Send RIL_REQUEST_SIGNAL_STRENGTH failed.");

		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_nitz_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct parcel rilp;
	int year, mon, mday, hour, min, sec, dst, tzi;
	char tzs, tz[4];
	gchar *nitz;

	if (message->req != RIL_UNSOL_NITZ_TIME_RECEIVED)
		goto error;


	ril_util_init_parcel(message, &rilp);

	nitz = parcel_r_string(&rilp);

	g_ril_append_print_buf(nd->ril, "(%s)", nitz);
	g_ril_print_unsol(nd->ril, message);

	sscanf(nitz, "%u/%u/%u,%u:%u:%u%c%u,%u", &year, &mon, &mday,
			&hour, &min, &sec, &tzs, &tzi, &dst);
	sprintf(tz, "%c%d", tzs, tzi);

	nd->time.utcoff = atoi(tz) * 15 * 60;
	nd->time.dst = dst;
	nd->time.sec = sec;
	nd->time.min = min;
	nd->time.hour = hour;
	nd->time.mday = mday;
	nd->time.mon = mon;
	nd->time.year = 2000 + year;

	ofono_netreg_time_notify(netreg, &nd->time);

	g_free(nitz);

	return;

error:
	ofono_error("Unable to notify ofono about nitz");
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	ofono_netreg_register(netreg);

	/* Register for network state changes */
	g_ril_register(nd->ril, RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
			ril_network_state_change, netreg);

	/* Register for network time update reports */
	g_ril_register(nd->ril, RIL_UNSOL_NITZ_TIME_RECEIVED,
			ril_nitz_notify, netreg);

	/* Register for signal strength changes */
	g_ril_register(nd->ril, RIL_UNSOL_SIGNAL_STRENGTH,
			ril_strength_notify, netreg);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct netreg_data *nd;

	nd = g_new0(struct netreg_data, 1);

	nd->ril = g_ril_clone(ril);
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

        /*
	 * TODO: analyze if capability check is needed
	 * and/or timer should be adjusted.
	 *
	 * ofono_netreg_register() needs to be called after
	 * the driver has been set in ofono_netreg_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use a timer instead.
	 */
        g_timeout_add_seconds(1, ril_delayed_register, netreg);

	return 0;
}

static void ril_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (nd->nitz_timeout)
		g_source_remove(nd->nitz_timeout);

	ofono_netreg_set_data(netreg, NULL);

	g_ril_unref(nd->ril);
	g_free(nd);
}

static struct ofono_netreg_driver driver = {
	.name				= RILMODEM,
	.probe				= ril_netreg_probe,
	.remove				= ril_netreg_remove,
	.registration_status		= ril_registration_status,
	.current_operator		= ril_current_operator,
	.list_operators			= ril_list_operators,
	.register_auto			= ril_register_auto,
	.register_manual		= ril_register_manual,
	.strength			= ril_signal_strength,
};

void ril_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void ril_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
