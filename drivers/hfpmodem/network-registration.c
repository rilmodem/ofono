/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009 ProFUSION embedded systems. All rights reserved.
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
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "common.h"

#include "hfpmodem.h"
#include "slc.h"

#define HFP_MAX_OPERATOR_NAME_LENGTH 16

static const char *cops_prefix[] = { "+COPS:", NULL };
static const char *cind_prefix[] = { "+CIND:", NULL };

struct netreg_data {
	GAtChat *chat;
	unsigned char cind_pos[HFP_INDICATOR_LAST];
	int cind_val[HFP_INDICATOR_LAST];
};

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_cb_t cb = cbd->cb;
	struct ofono_network_operator op;
	GAtResultIter iter;
	int format;
	const char *name;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
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

	strncpy(op.name, name, HFP_MAX_OPERATOR_NAME_LENGTH);
	op.name[HFP_MAX_OPERATOR_NAME_LENGTH] = '\0';

	op.mcc[0] = '\0';
	op.mnc[0] = '\0';
	op.status = 2;
	op.tech = -1;

	cb(&error, &op, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ciev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	int index, value, status;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIEV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	if (index == nd->cind_pos[HFP_INDICATOR_SERVICE]) {
		nd->cind_val[HFP_INDICATOR_SERVICE] = value;
		if (value)
			status = NETWORK_REGISTRATION_STATUS_REGISTERED;
		else
			status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;

		ofono_netreg_status_notify(netreg, status, -1, -1, -1);
	} else if (index == nd->cind_pos[HFP_INDICATOR_ROAM]) {
		nd->cind_val[HFP_INDICATOR_ROAM] = value;

		if (value)
			status = NETWORK_REGISTRATION_STATUS_ROAMING;
		else if (nd->cind_val[HFP_INDICATOR_SERVICE])
			status = NETWORK_REGISTRATION_STATUS_REGISTERED;
		else
			status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;

		ofono_netreg_status_notify(netreg, status, -1, -1, -1);
	} else if (index == nd->cind_pos[HFP_INDICATOR_SIGNAL]) {
		nd->cind_val[HFP_INDICATOR_SIGNAL] = value;
		ofono_netreg_strength_notify(netreg, value * 20);
	}

	return;
}

static void signal_strength_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	struct netreg_data *nd = ofono_netreg_get_data(cbd->user);
	GAtResultIter iter;
	int index, strength;
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

	index = 1;

	while (g_at_result_iter_next_number(&iter, &strength)) {
		if (index == nd->cind_pos[HFP_INDICATOR_SIGNAL]) {
			nd->cind_val[HFP_INDICATOR_SIGNAL] = strength;
			break;
		}

		index++;
	}

	DBG("signal_strength_cb: %d", strength);

	cb(&error, strength * 20, cbd->data);
}

static void registration_status_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	struct netreg_data *nd = ofono_netreg_get_data(cbd->user);
	GAtResultIter iter;
	int index, value, status;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIND:")) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
		return;
	}

	index = 1;

	while (g_at_result_iter_next_number(&iter, &value)) {
		if (index == nd->cind_pos[HFP_INDICATOR_SERVICE])
			nd->cind_val[HFP_INDICATOR_SERVICE] = value;

		if (index == nd->cind_pos[HFP_INDICATOR_ROAM])
			nd->cind_val[HFP_INDICATOR_ROAM] = value;

		index++;
	}

	if (nd->cind_val[HFP_INDICATOR_SERVICE])
		status = NETWORK_REGISTRATION_STATUS_REGISTERED;
	else
		status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;

	if (nd->cind_val[HFP_INDICATOR_ROAM])
		status = NETWORK_REGISTRATION_STATUS_ROAMING;

	cb(&error, status, -1, -1, -1, cbd->data);
}

static void hfp_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);
	gboolean ok;

	cbd->user = netreg;

	ok = g_at_chat_send(nd->chat, "AT+CIND?", cind_prefix,
				registration_status_cb, cbd, g_free);
	if (ok)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
}

static void hfp_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);
	gboolean ok;

	cbd->user = netreg;

	ok = g_at_chat_send(nd->chat, "AT+COPS=3,0", NULL,
			NULL, cbd, NULL);

	if (ok)
		ok = g_at_chat_send(nd->chat, "AT+COPS?", cops_prefix,
				cops_cb, cbd, g_free);

	if (ok)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void hfp_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = netreg;

	if (g_at_chat_send(nd->chat, "AT+CIND?", cind_prefix,
				signal_strength_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static gboolean hfp_netreg_register(gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;

	ofono_netreg_register(netreg);

	return FALSE;
}

static int hfp_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *user_data)
{
	struct hfp_slc_info *info = user_data;
	struct netreg_data *nd;

	nd = g_new0(struct netreg_data, 1);

	nd->chat = g_at_chat_clone(info->chat);
	memcpy(nd->cind_pos, info->cind_pos, HFP_INDICATOR_LAST);
	memcpy(nd->cind_val, info->cind_val, HFP_INDICATOR_LAST);

	ofono_netreg_set_data(netreg, nd);

	g_at_chat_register(nd->chat, "+CIEV:", ciev_notify, FALSE,
				netreg, NULL);

	g_idle_add(hfp_netreg_register, netreg);

	return 0;
}

static void hfp_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	ofono_netreg_set_data(netreg, NULL);

	g_free(nd);
}

static struct ofono_netreg_driver driver = {
	.name				= "hfpmodem",
	.probe				= hfp_netreg_probe,
	.remove				= hfp_netreg_remove,
	.registration_status		= hfp_registration_status,
	.current_operator		= hfp_current_operator,
	.strength			= hfp_signal_strength,
};

void hfp_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void hfp_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
