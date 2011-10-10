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
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-volume.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *clvl_prefix[] = { "+CLVL:", NULL };
static const char *cmut_prefix[] = { "+CMUT:", NULL };
static const char *none_prefix[] = { NULL };

struct cv_data {
	int clvl_min;
	int clvl_max;
	GAtChat *chat;
};

static void cmut_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	GAtResultIter iter;
	int muted;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CMUT:"))
		return;

	if (g_at_result_iter_next_number(&iter, &muted) == FALSE)
		return;

	ofono_call_volume_set_muted(cv, muted);
}

static void clvl_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *cvd = ofono_call_volume_get_data(cv);
	GAtResultIter iter;
	int lvl;
	int percent;

	if (!ok)
		return;

	if (cvd->clvl_max == 0 && cvd->clvl_min == 0)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLVL:"))
		return;

	if (g_at_result_iter_next_number(&iter, &lvl) == FALSE)
		return;

	percent = ((lvl - cvd->clvl_min) * 100) /
				(cvd->clvl_max - cvd->clvl_min);

	ofono_call_volume_set_speaker_volume(cv, percent);
	ofono_call_volume_register(cv);
}

static void clvl_range_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *cvd = ofono_call_volume_get_data(cv);
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLVL:"))
		return;

	/* Try opening the list, but don't fail */
	g_at_result_iter_open_list(&iter);
	g_at_result_iter_next_range(&iter, &cvd->clvl_min, &cvd->clvl_max);
	g_at_result_iter_close_list(&iter);
}

static void cv_generic_set_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_volume_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_call_volume_speaker_volume(struct ofono_call_volume *cv,
						unsigned char percent,
						ofono_call_volume_cb_t cb,
						void *data)
{
	struct cv_data *cvd = ofono_call_volume_get_data(cv);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];
	int level;

	level = ((cvd->clvl_max - cvd->clvl_min) *
			percent) / 100 + cvd->clvl_min;

	snprintf(buf, sizeof(buf), "AT+CLVL=%d", level);

	if (g_at_chat_send(cvd->chat, buf, none_prefix,
				cv_generic_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_call_volume_mute(struct ofono_call_volume *cv, int muted,
				ofono_call_volume_cb_t cb, void *data)
{
	struct cv_data *cvd = ofono_call_volume_get_data(cv);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	snprintf(buf, sizeof(buf), "AT+CMUT=%d", muted);

	if (g_at_chat_send(cvd->chat, buf, none_prefix,
				cv_generic_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static int at_call_volume_probe(struct ofono_call_volume *cv,
				unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct cv_data *cvd;

	DBG("%p", cv);

	cvd = g_new0(struct cv_data, 1);
	cvd->chat = g_at_chat_clone(chat);

	ofono_call_volume_set_data(cv, cvd);

	g_at_chat_send(cvd->chat, "AT+CMUT?", cmut_prefix,
			cmut_query, cv, NULL);
	g_at_chat_send(cvd->chat, "AT+CLVL=?", clvl_prefix,
			clvl_range_query, cv, NULL);
	g_at_chat_send(cvd->chat, "AT+CLVL?", clvl_prefix,
			clvl_query, cv, NULL);

	/* Generic driver does not support microphone level */
	ofono_call_volume_set_microphone_volume(cv, 100);

	return 0;
}

static void at_call_volume_remove(struct ofono_call_volume *cv)
{
	struct cv_data *cvd = ofono_call_volume_get_data(cv);

	ofono_call_volume_set_data(cv, NULL);

	g_at_chat_unref(cvd->chat);
	g_free(cvd);
}

static struct ofono_call_volume_driver driver = {
	.name = "atmodem",
	.probe = at_call_volume_probe,
	.remove = at_call_volume_remove,
	.speaker_volume = at_call_volume_speaker_volume,
	.mute = at_call_volume_mute,
};

void at_call_volume_init(void)
{
	ofono_call_volume_driver_register(&driver);
}

void at_call_volume_exit(void)
{
	ofono_call_volume_driver_unregister(&driver);
}
