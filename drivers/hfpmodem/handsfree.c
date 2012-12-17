/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  BMW Car IT GmbH. All rights reserved.
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
#include <unistd.h>

#include <glib.h>
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/handsfree.h>

#include "hfpmodem.h"
#include "slc.h"

static const char *binp_prefix[] = { "+BINP:", NULL };
static const char *bvra_prefix[] = { "+BVRA:", NULL };

struct hf_data {
	GAtChat *chat;
	unsigned int ag_features;
	int battchg_index;
	guint register_source;
};

static void hf_generic_set_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_handsfree_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void bsir_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_handsfree *hf = user_data;
	GAtResultIter iter;
	int value;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+BSIR:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	ofono_handsfree_set_inband_ringing(hf, (ofono_bool_t) value);
}

static void bvra_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_handsfree *hf = user_data;
	GAtResultIter iter;
	int value;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+BVRA:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	ofono_handsfree_voice_recognition_notify(hf, (ofono_bool_t) value);
}

static void ciev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_handsfree *hf = user_data;
	struct hf_data *hd = ofono_handsfree_get_data(hf);
	int index;
	int value;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIEV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		return;

	if (index != hd->battchg_index)
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	ofono_handsfree_battchg_notify(hf, value);
}

static gboolean hfp_handsfree_register(gpointer user_data)
{
	struct ofono_handsfree *hf = user_data;
	struct hf_data *hd = ofono_handsfree_get_data(hf);

	hd->register_source = 0;

	g_at_chat_register(hd->chat, "+BSIR:", bsir_notify, FALSE, hf, NULL);
	g_at_chat_register(hd->chat, "+BVRA:", bvra_notify, FALSE, hf, NULL);
	g_at_chat_register(hd->chat, "+CIEV:", ciev_notify, FALSE, hf, NULL);

	if (hd->ag_features & HFP_AG_FEATURE_IN_BAND_RING_TONE)
		ofono_handsfree_set_inband_ringing(hf, TRUE);

	ofono_handsfree_set_ag_features(hf, hd->ag_features);
	ofono_handsfree_register(hf);

	return FALSE;
}

static int hfp_handsfree_probe(struct ofono_handsfree *hf,
				unsigned int vendor, void *data)
{
	struct hfp_slc_info *info = data;
	struct hf_data *hd;

	DBG("");
	hd = g_new0(struct hf_data, 1);
	hd->chat = g_at_chat_clone(info->chat);
	hd->ag_features = info->ag_features;

	ofono_handsfree_set_data(hf, hd);

	hd->battchg_index = info->cind_pos[HFP_INDICATOR_BATTCHG];
	ofono_handsfree_battchg_notify(hf,
					info->cind_val[HFP_INDICATOR_BATTCHG]);

	hd->register_source = g_idle_add(hfp_handsfree_register, hf);

	return 0;
}

static void hfp_handsfree_remove(struct ofono_handsfree *hf)
{
	struct hf_data *hd = ofono_handsfree_get_data(hf);

	if (hd->register_source != 0)
		g_source_remove(hd->register_source);

	ofono_handsfree_set_data(hf, NULL);

	g_at_chat_unref(hd->chat);
	g_free(hd);
}

static void hfp_request_phone_number_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_handsfree_phone_cb_t cb = cbd->cb;
	GAtResultIter iter;
	struct ofono_error error;
	const char *num;
	int type;
	struct ofono_phone_number phone_number;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+BINP:"))
		goto fail;

	if (!g_at_result_iter_next_string(&iter, &num))
		goto fail;

	if (!g_at_result_iter_next_number(&iter, &type))
		goto fail;

	DBG("AT+BINP=1 response: %s %d", num, type);

	strncpy(phone_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	phone_number.type = type;

	cb(&error, &phone_number, cbd->data);
	return;

fail:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void hfp_request_phone_number(struct ofono_handsfree *hf,
					ofono_handsfree_phone_cb_t cb,
					void *data)
{
	struct hf_data *hd = ofono_handsfree_get_data(hf);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(hd->chat, "AT+BINP=1", binp_prefix,
				hfp_request_phone_number_cb,
				cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void hfp_voice_recognition(struct ofono_handsfree *hf,
					ofono_bool_t enabled,
					ofono_handsfree_cb_t cb, void *data)
{
	struct hf_data *hd = ofono_handsfree_get_data(hf);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	snprintf(buf, sizeof(buf), "AT+BVRA=%d",
				(int)(enabled));

	if (g_at_chat_send(hd->chat, buf, bvra_prefix,
				hf_generic_set_cb,
				cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static struct ofono_handsfree_driver driver = {
	.name			= "hfpmodem",
	.probe			= hfp_handsfree_probe,
	.remove			= hfp_handsfree_remove,
	.request_phone_number	= hfp_request_phone_number,
	.voice_recognition	= hfp_voice_recognition,
};

void hfp_handsfree_init(void)
{
	ofono_handsfree_driver_register(&driver);
}

void hfp_handsfree_exit(void)
{
	ofono_handsfree_driver_unregister(&driver);
}
