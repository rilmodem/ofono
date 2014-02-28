/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2012-2013  Canonical Ltd.
 *  Copyright (C) 2013  Jolla Ltd.
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
#include <ofono/spn-table.h>

#include "common.h"
#include "gril.h"
#include "rilmodem.h"

#include "grilreply.h"
#include "grilrequest.h"
#include "grilunsol.h"

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

static void ril_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data);

static void extract_mcc_mnc(const char *str, char *mcc, char *mnc)
{
	/* Three digit country code */
	strncpy(mcc, str, OFONO_MAX_MCC_LENGTH);
	mcc[OFONO_MAX_MCC_LENGTH] = '\0';

	/* Usually a 2 but sometimes 3 digit network code */
	strncpy(mnc, str + OFONO_MAX_MCC_LENGTH, OFONO_MAX_MNC_LENGTH);
	mnc[OFONO_MAX_MNC_LENGTH] = '\0';
}

static void ril_creg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct reply_reg_state *reply;

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: failed to pull registration state",
				__func__);
		goto error;
	}

	if ((reply = g_ril_reply_parse_reg_state(nd->ril, message)) == NULL)
		goto error;

	nd->tech = reply->tech;

	CALLBACK_WITH_SUCCESS(cb,
				reply->status,
				reply->lac,
				reply->ci,
				reply->tech,
				cbd->data);

	g_free(reply);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
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

	g_ril_print_unsol_no_args(nd->ril, message);

	ril_registration_status(netreg, NULL, NULL);
}

static void ril_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd;

	/*
	 * If no cb specified, setup internal callback to
	 * handle unsolicited VOICE_NET_STATE_CHANGE events.
	 */
	if (cb == NULL)
		cbd = cb_data_new(ril_creg_notify, netreg, nd);
	else
		cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_VOICE_REGISTRATION_STATE, NULL,
			ril_creg_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
	}
}

static void set_oper_name(const struct reply_operator *reply,
				struct ofono_network_operator *op)
{
	const char *spn = NULL;

	/* Use SPN list if we do not have name */
	if (strcmp(reply->numeric, reply->lalpha) == 0) {
		spn = __ofono_spn_table_get_spn(reply->numeric);
		if (spn != NULL) {
			DBG("using spn override %s", spn);
			strncpy(op->name, spn, OFONO_MAX_OPERATOR_NAME_LENGTH);
		}
	}

	if (spn == NULL) {
		/* Try to use long by default */
		if (reply->lalpha)
			strncpy(op->name, reply->lalpha,
				OFONO_MAX_OPERATOR_NAME_LENGTH);
		else if (reply->salpha)
			strncpy(op->name, reply->salpha,
				OFONO_MAX_OPERATOR_NAME_LENGTH);
	}
}

static void ril_cops_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct reply_operator *reply;
	struct ofono_network_operator op;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: failed to retrive the current operator",
				__func__);
		goto error;
	}

	if ((reply = g_ril_reply_parse_operator(nd->ril, message)) == NULL)
		goto error;

	set_oper_name(reply, &op);

	extract_mcc_mnc(reply->numeric, op.mcc, op.mnc);

	/* Set to current */
	op.status = OPERATOR_STATUS_CURRENT;
	op.tech = nd->tech;

	CALLBACK_WITH_SUCCESS(cb, &op, cbd->data);

	g_ril_reply_free_operator(reply);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_OPERATOR, NULL,
			ril_cops_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static void ril_cops_list_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct reply_avail_ops *reply = NULL;
	struct ofono_network_operator *ops;
	struct reply_operator *operator;
	GSList *l;
	unsigned int i = 0;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: failed to retrive the list of operators",
				__func__);
		goto error;
	}

	if ((reply = g_ril_reply_parse_avail_ops(nd->ril, message)) == NULL)
		goto error;

	ops = g_try_new0(struct ofono_network_operator, reply->num_ops);
	if (ops == NULL) {
		ofono_error("%s: can't allocate ofono_network_operator",
				__func__);

		goto error;
	}

	for (l = reply->list; l; l = l->next) {
		operator = l->data;

		set_oper_name(operator, &ops[i]);

		extract_mcc_mnc(operator->numeric, ops[i].mcc, ops[i].mnc);

		ops[i].tech = operator->tech;

		/* Set the proper status  */
		if (!strcmp(operator->status, "unknown"))
			ops[i].status = OPERATOR_STATUS_UNKNOWN;
		else if (!strcmp(operator->status, "available"))
			ops[i].status = OPERATOR_STATUS_AVAILABLE;
		else if (!strcmp(operator->status, "current"))
			ops[i].status = OPERATOR_STATUS_CURRENT;
		else if (!strcmp(operator->status, "forbidden"))
			ops[i].status = OPERATOR_STATUS_FORBIDDEN;

		i++;
	}

	CALLBACK_WITH_SUCCESS(cb, reply->num_ops, ops, cbd->data);
	g_ril_reply_free_avail_ops(reply);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
	g_ril_reply_free_avail_ops(reply);
}

static void ril_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_QUERY_AVAILABLE_NETWORKS, NULL,
			ril_cops_list_cb, cbd, g_free) == 0) {
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

	if (g_ril_send(nd->ril, RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC, NULL,
			ril_register_cb, cbd, g_free) == 0) {
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

	/* RIL expects a char * specifying MCCMNC of network to select */
	snprintf(buf, sizeof(buf), "%s%s", mcc, mnc);

	g_ril_request_set_net_select_manual(nd->ril, buf, &rilp);

	/* In case of error free cbd and return the cb with failure */
	if (g_ril_send(nd->ril, RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL, &rilp,
			ril_register_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_strength_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int strength = g_ril_unsol_parse_signal_strength(nd->ril, message);

	ofono_netreg_strength_notify(netreg, strength);
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

	/* The g_ril_unsol* function handles both reply & unsolicited */
	strength = g_ril_unsol_parse_signal_strength(nd->ril, message);
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

	if (g_ril_send(nd->ril, RIL_REQUEST_SIGNAL_STRENGTH, NULL,
			ril_strength_cb, cbd, g_free) == 0) {
		ofono_error("Send RIL_REQUEST_SIGNAL_STRENGTH failed.");

		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_nitz_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int year, mon, mday, hour, min, sec, dst, tzi;
	char tzs, tz[4];
	gchar *nitz;

	if ((nitz = g_ril_unsol_parse_nitz(nd->ril, message)) == NULL)
		goto error;

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
	ofono_error("%s: unable to notify ofono about NITZ", __func__);
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
	 * ofono_netreg_register() needs to be called after
	 * the driver has been set in ofono_netreg_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use the idle loop here.
	 */
	g_idle_add(ril_delayed_register, netreg);

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
