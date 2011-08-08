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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/emulator.h>

#include <drivers/atmodem/atutil.h>

#include "slc.h"

static const char *brsf_prefix[] = { "+BRSF:", NULL };
static const char *cind_prefix[] = { "+CIND:", NULL };
static const char *cmer_prefix[] = { "+CMER:", NULL };
static const char *chld_prefix[] = { "+CHLD:", NULL };

struct slc_establish_data {
	gint ref_count;
	struct hfp_slc_info *info;
	hfp_slc_cb_t failed_cb;
	hfp_slc_cb_t connect_cb;
	gpointer userdata;
};

void hfp_slc_info_init(struct hfp_slc_info *info, guint16 version)
{
	info->ag_features = 0;
	info->ag_mpty_features = 0;

	info->hf_features = HFP_HF_FEATURE_3WAY;
	info->hf_features |= HFP_HF_FEATURE_CLIP;
	info->hf_features |= HFP_HF_FEATURE_REMOTE_VOLUME_CONTROL;

	if (version < HFP_VERSION_1_5)
		goto done;

	info->hf_features |= HFP_HF_FEATURE_ENHANCED_CALL_STATUS;
	info->hf_features |= HFP_HF_FEATURE_ENHANCED_CALL_CONTROL;

done:
	memset(info->cind_val, 0, sizeof(info->cind_val));
	memset(info->cind_pos, 0, sizeof(info->cind_pos));
}

static void slc_establish_data_unref(gpointer userdata)
{
	struct slc_establish_data *sed = userdata;

	if (g_atomic_int_dec_and_test(&sed->ref_count))
		g_free(sed);
}

static void slc_establish_data_ref(struct slc_establish_data *sed)
{
	g_atomic_int_inc(&sed->ref_count);
}

static void slc_failed(struct slc_establish_data *sed)
{
	sed->failed_cb(sed->userdata);
}

static void slc_established(struct slc_establish_data *sed)
{
	struct hfp_slc_info *info = sed->info;

	g_at_chat_send(info->chat, "AT+CMEE=1", NULL, NULL, NULL, NULL);
	sed->connect_cb(sed->userdata);
}

static void chld_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct slc_establish_data *sed = user_data;
	struct hfp_slc_info *info = sed->info;
	unsigned int ag_mpty_feature = 0;
	GAtResultIter iter;
	const char *str;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CHLD:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto error;

	while (g_at_result_iter_next_unquoted_string(&iter, &str)) {
		if (!strcmp(str, "0"))
			ag_mpty_feature |= AG_CHLD_0;
		else if (!strcmp(str, "1"))
			ag_mpty_feature |= AG_CHLD_1;
		else if (!strcmp(str, "1x"))
			ag_mpty_feature |= AG_CHLD_1x;
		else if (!strcmp(str, "2"))
			ag_mpty_feature |= AG_CHLD_2;
		else if (!strcmp(str, "2x"))
			ag_mpty_feature |= AG_CHLD_2x;
		else if (!strcmp(str, "3"))
			ag_mpty_feature |= AG_CHLD_3;
		else if (!strcmp(str, "4"))
			ag_mpty_feature |= AG_CHLD_4;
	}

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	info->ag_mpty_features = ag_mpty_feature;

	slc_established(sed);
	return;

error:
	slc_failed(sed);
}

static void cmer_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct slc_establish_data *sed = user_data;
	struct hfp_slc_info *info = sed->info;

	if (!ok) {
		slc_failed(sed);
		return;
	}

	if (info->ag_features & HFP_AG_FEATURE_3WAY) {
		slc_establish_data_ref(sed);
		g_at_chat_send(info->chat, "AT+CHLD=?", chld_prefix,
				chld_cb, sed, slc_establish_data_unref);
	} else
		slc_established(sed);
}

static void cind_status_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct slc_establish_data *sed = user_data;
	struct hfp_slc_info *info = sed->info;
	GAtResultIter iter;
	int index;
	int value;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	while (g_at_result_iter_next_number(&iter, &value)) {
		int i;

		for (i = 0; i < HFP_INDICATOR_LAST; i++) {
			if (index != info->cind_pos[i])
				continue;

			info->cind_val[i] = value;
		}

		index += 1;
	}

	slc_establish_data_ref(sed);
	g_at_chat_send(info->chat, "AT+CMER=3,0,0,1", cmer_prefix,
				cmer_cb, sed, slc_establish_data_unref);
	return;

error:
	slc_failed(sed);
}

static void cind_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct slc_establish_data *sed = user_data;
	struct hfp_slc_info *info = sed->info;
	GAtResultIter iter;
	const char *str;
	int index;
	int min, max;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	while (g_at_result_iter_open_list(&iter)) {
		if (!g_at_result_iter_next_string(&iter, &str))
			goto error;

		if (!g_at_result_iter_open_list(&iter))
			goto error;

		while (g_at_result_iter_next_range(&iter, &min, &max))
			;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (g_str_equal("service", str) == TRUE)
			info->cind_pos[HFP_INDICATOR_SERVICE] = index;
		else if (g_str_equal("call", str) == TRUE)
			info->cind_pos[HFP_INDICATOR_CALL] = index;
		else if (g_str_equal("callsetup", str) == TRUE)
			info->cind_pos[HFP_INDICATOR_CALLSETUP] = index;
		else if (g_str_equal("callheld", str) == TRUE)
			info->cind_pos[HFP_INDICATOR_CALLHELD] = index;
		else if (g_str_equal("signal", str) == TRUE)
			info->cind_pos[HFP_INDICATOR_SIGNAL] = index;
		else if (g_str_equal("roam", str) == TRUE)
			info->cind_pos[HFP_INDICATOR_ROAM] = index;
		else if (g_str_equal("battchg", str) == TRUE)
			info->cind_pos[HFP_INDICATOR_BATTCHG] = index;

		index += 1;
	}

	slc_establish_data_ref(sed);
	g_at_chat_send(info->chat, "AT+CIND?", cind_prefix,
			cind_status_cb, sed, slc_establish_data_unref);
	return;

error:
	slc_failed(sed);
}

static void brsf_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct slc_establish_data *sed = user_data;
	struct hfp_slc_info *info = sed->info;
	GAtResultIter iter;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+BRSF:"))
		goto error;

	g_at_result_iter_next_number(&iter, (gint *)&info->ag_features);

	slc_establish_data_ref(sed);
	g_at_chat_send(info->chat, "AT+CIND=?", cind_prefix,
				cind_cb, sed, slc_establish_data_unref);
	return;

error:
	slc_failed(sed);
}

void hfp_slc_establish(struct hfp_slc_info *info, hfp_slc_cb_t connect_cb,
			hfp_slc_cb_t failed_cb, void *userdata)
{
	char buf[64];
	struct slc_establish_data *sed = g_new0(struct slc_establish_data, 1);

	sed->ref_count = 1;
	sed->connect_cb = connect_cb;
	sed->failed_cb = failed_cb;
	sed->userdata = userdata;
	sed->info = info;

	snprintf(buf, sizeof(buf), "AT+BRSF=%d", info->hf_features);
	g_at_chat_send(info->chat, buf, brsf_prefix,
				brsf_cb, sed, slc_establish_data_unref);
}
