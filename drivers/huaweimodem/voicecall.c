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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "gatchat.h"
#include "gatresult.h"

#include "huaweimodem.h"

static const char *none_prefix[] = { NULL };

struct voicecall_data {
	GAtChat *chat;
};

static void cring_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	const char *line;
	int type;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRING:"))
		return;

	line = g_at_result_iter_raw_line(&iter);

	if (line == NULL)
		return;

	/* Ignore everything that is not voice for now */
	if (!strcasecmp(line, "VOICE"))
		type = 0;
	else
		type = 9;

	/* Assume the CLIP always arrives, and we signal the call there */
	DBG("%d", type);
}

static void clip_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	const char *num;
	int type, validity;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CLIP:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &type))
		return;

	if (strlen(num) > 0)
		validity = 0;
	else
		validity = 2;

	/* Skip subaddr, satype and alpha */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	/* If we have CLI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("%s %d %d", num, type, validity);
}

static void ccwa_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	const char *num;
	int num_type, validity, cls;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CCWA:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &num_type))
		return;

	if (!g_at_result_iter_next_number(&iter, &cls))
		return;

	/* Skip alpha field */
	g_at_result_iter_skip_next(&iter);

	if (strlen(num) > 0)
		validity = 0;
	else
		validity = 2;

	/* If we have CLI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("%s %d %d %d", num, num_type, cls, validity);
}

static void huawei_voicecall_initialized(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	DBG("registering to notifications");

	g_at_chat_register(vd->chat, "+CRING:", cring_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CLIP:", clip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CCWA:", ccwa_notify, FALSE, vc, NULL);

	ofono_voicecall_register(vc);
}

static int huawei_voicecall_probe(struct ofono_voicecall *vc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (!vd)
		return -ENOMEM;

	vd->chat = g_at_chat_clone(chat);

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "AT+CRC=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CLIP=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+COLP=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT+CCWA=1", none_prefix,
				huawei_voicecall_initialized, vc, NULL);

	return 0;
}

static void huawei_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "huaweimodem",
	.probe			= huawei_voicecall_probe,
	.remove			= huawei_voicecall_remove,
};

void huawei_voicecall_init()
{
	ofono_voicecall_driver_register(&driver);
}

void huawei_voicecall_exit()
{
	ofono_voicecall_driver_unregister(&driver);
}
