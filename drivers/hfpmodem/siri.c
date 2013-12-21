/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2013  Intel Corporation. All rights reserved.
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
#include <ofono/siri.h>

#include "hfpmodem.h"
#include "hfp.h"
#include "slc.h"

#define APPLE_SIRI_STATUS_FEATURE 8

static const char *xapl_prefix[] = { "+XAPL=", NULL };
static const char *aplsiri_prefix[] = { "+APLSIRI:", NULL };
static const char *aplefm_prefix[] = { "+APLEFM:", NULL };

struct siri_data
{
	GAtChat *chat;
};

static void aplsiri_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_siri *siri = user_data;
	GAtResultIter iter;
	gint value;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+APLSIRI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &value))
		return;

	ofono_siri_set_status(siri, value);
}

static void aplsiri_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_siri *siri = user_data;
	struct siri_data *sd = ofono_siri_get_data(siri);
	struct ofono_error error;
	GAtResultIter iter;
	gint value;

	if (!ok)
		goto fail;

	decode_at_error(&error, g_at_result_final_response(result));

	if (error.type != OFONO_ERROR_TYPE_NO_ERROR)
		goto fail;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+APLSIRI:"))
		goto fail;

	if (!g_at_result_iter_next_number(&iter, &value))
		goto fail;

	if (value == 0)
		goto fail;

	g_at_chat_register(sd->chat, "+APLSIRI:",
				aplsiri_notify, FALSE, siri, NULL);

	ofono_siri_register(siri);

	ofono_siri_set_status(siri, value);

	return;

fail:
	ofono_siri_remove(siri);
}

static void xapl_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_siri *siri = user_data;
	struct siri_data *sd = ofono_siri_get_data(siri);
	struct ofono_error error;

	if (!ok) {
		ofono_siri_remove(siri);
		return;
	}

	decode_at_error(&error, g_at_result_final_response(result));
	if (error.type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_siri_remove(siri);
		return;
	}

	g_at_chat_send(sd->chat, "AT+APLSIRI?",
				aplsiri_prefix, aplsiri_cb, siri, NULL);
}

static int hfp_siri_probe(struct ofono_siri *siri, unsigned int vendor,
				void *data)
{
	struct hfp_slc_info *info = data;
	struct siri_data *sd;
	char at_command[64];

	DBG("");

	sd = g_new0(struct siri_data, 1);
	sd->chat = g_at_chat_clone(info->chat);

	ofono_siri_set_data(siri, sd);

	snprintf(at_command, sizeof(at_command),
			"AT+XAPL=Linux-oFono-%s,%d",
			VERSION, APPLE_SIRI_STATUS_FEATURE);

	g_at_chat_send(sd->chat, at_command, xapl_prefix, xapl_cb, siri, NULL);

	return 0;
}

static void hfp_siri_remove(struct ofono_siri *siri)
{
	struct siri_data *sd = ofono_siri_get_data(siri);

	ofono_siri_set_data(siri, NULL);

	g_at_chat_unref(sd->chat);
	g_free(sd);
}

static void hfp_siri_eyes_free_mode_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_siri_cb_t cb = cbd->cb;
	struct ofono_siri *siri = cbd->data;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, siri);
}

static void hfp_siri_set_eyes_free_mode(struct ofono_siri *siri,
					ofono_siri_cb_t cb, unsigned int val)
{
	struct siri_data *sd = ofono_siri_get_data(siri);
	struct cb_data *cbd = cb_data_new(cb, siri);
	char at_command[16];

	snprintf(at_command, sizeof(at_command), "AT+APLEFM=%d", val);

	if (g_at_chat_send(sd->chat, at_command, aplefm_prefix,
				hfp_siri_eyes_free_mode_cb,
				cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL);
}

static struct ofono_siri_driver driver = {
	.name = "hfpmodem",
	.probe = hfp_siri_probe,
	.remove = hfp_siri_remove,
	.set_eyes_free_mode = hfp_siri_set_eyes_free_mode,
};

void hfp_siri_init(void)
{
	ofono_siri_driver_register(&driver);
}

void hfp_siri_exit(void)
{
	ofono_siri_driver_unregister(&driver);
}
