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
#include <errno.h>
#include <unistd.h>

#include <glib.h>
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-volume.h>

#include "hfpmodem.h"
#include "slc.h"

#define HFP_CALL_VOLUME_MAX 15

static const char *vgs_prefix[] = { "+VGS:", NULL };
static const char *vgm_prefix[] = { "+VGM:", NULL };

struct cv_data {
	GAtChat *chat;
	unsigned char sp_volume;
	unsigned char mic_volume;
};

static void cv_generic_set_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_volume_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void hfp_speaker_volume(struct ofono_call_volume *cv,
					unsigned char percent,
					ofono_call_volume_cb_t cb,
					void *data)
{
	struct cv_data *vd = ofono_call_volume_get_data(data);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	vd->sp_volume = percent;

	snprintf(buf, sizeof(buf), "AT+VGS=%d",
				(int)(percent*HFP_CALL_VOLUME_MAX/100));

	if (g_at_chat_send(vd->chat, buf, vgs_prefix,
				cv_generic_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void hfp_microphone_volume(struct ofono_call_volume *cv,
					unsigned char percent,
					ofono_call_volume_cb_t cb,
					void *data)
{
	struct cv_data *vd = ofono_call_volume_get_data(data);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	vd->mic_volume = percent;

	snprintf(buf, sizeof(buf), "AT+VGM=%d",
				(int)(percent*HFP_CALL_VOLUME_MAX/100));

	if (g_at_chat_send(vd->chat, buf, vgm_prefix,
				cv_generic_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void vgs_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *vd = ofono_call_volume_get_data(cv);
	GAtResultIter iter;
	gint value;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+VGS:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	vd->sp_volume = (unsigned char)(value*100/HFP_CALL_VOLUME_MAX);
	ofono_call_volume_set_speaker_volume(cv, vd->sp_volume);
}

static void vgm_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *vd = ofono_call_volume_get_data(cv);
	GAtResultIter iter;
	gint value;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+VGM:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	vd->mic_volume = (unsigned char)(value*100/HFP_CALL_VOLUME_MAX);
	ofono_call_volume_set_microphone_volume(cv, vd->mic_volume);
}

static void sync_speaker_volume_cb(const struct ofono_error *error,
					void *user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *vd = ofono_call_volume_get_data(cv);

	ofono_call_volume_set_speaker_volume(cv, vd->sp_volume);
}

static void sync_microphone_volume_cb(const struct ofono_error *error,
					void *user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *vd = ofono_call_volume_get_data(cv);

	ofono_call_volume_set_microphone_volume(cv, vd->mic_volume);
}

static void hfp_call_volume_initialized(gpointer user_data)
{
	struct ofono_call_volume *cv = user_data;
	struct cv_data *vd = ofono_call_volume_get_data(cv);

	DBG("");

	g_at_chat_register(vd->chat, "+VGS:", vgs_notify, FALSE, cv, NULL);
	g_at_chat_register(vd->chat, "+VGM:", vgm_notify, FALSE, cv, NULL);

	ofono_call_volume_register(cv);

	/* set sp and mic volume at 50 percents by default */
	hfp_speaker_volume(cv, 50, sync_speaker_volume_cb, cv);
	hfp_microphone_volume(cv, 50, sync_microphone_volume_cb, cv);
}

static int hfp_call_volume_probe(struct ofono_call_volume *cv,
					unsigned int vendor, void *data)
{
	struct hfp_slc_info *info = data;
	struct cv_data *vd;

	DBG("");
	vd = g_new0(struct cv_data, 1);
	vd->chat = g_at_chat_clone(info->chat);

	ofono_call_volume_set_data(cv, vd);

	hfp_call_volume_initialized(cv);

	return 0;
}

static void hfp_call_volume_remove(struct ofono_call_volume *cv)
{
	struct cv_data *vd = ofono_call_volume_get_data(cv);

	ofono_call_volume_set_data(cv, NULL);

	g_free(vd);
}

static struct ofono_call_volume_driver driver = {
	.name			= "hfpmodem",
	.probe			= hfp_call_volume_probe,
	.remove			= hfp_call_volume_remove,
	.speaker_volume		= hfp_speaker_volume,
	.microphone_volume	= hfp_microphone_volume,
	.mute			= NULL,
};

void hfp_call_volume_init(void)
{
	ofono_call_volume_driver_register(&driver);
}

void hfp_call_volume_exit(void)
{
	ofono_call_volume_driver_unregister(&driver);
}

