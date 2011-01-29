/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010 Nokia Corporation. All rights reserved.
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
#include <ofono/cdma-voicecall.h>

#include "gatchat.h"
#include "gatresult.h"

#include "cdmamodem.h"

static const char *none_prefix[] = { NULL };

struct voicecall_data {
	GAtChat *chat;
	unsigned int vendor;
};

static void cdma_template(const char *cmd, struct ofono_cdma_voicecall *vc,
				GAtResultFunc result_cb,
				ofono_cdma_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_cdma_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = vc;

	if (g_at_chat_send(vd->chat, cmd, none_prefix,
				result_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void cdma_generic_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_cdma_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void cdma_dial(struct ofono_cdma_voicecall *vc,
			const struct ofono_cdma_phone_number *ph,
			ofono_cdma_voicecall_cb_t cb, void *data)
{
	char buf[OFONO_CDMA_MAX_PHONE_NUMBER_LENGTH + 8];

	snprintf(buf, sizeof(buf), "AT+CDV=%s", ph->number);
	cdma_template(buf, vc, cdma_generic_cb, cb, data);
}

static void cdma_hangup_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;

	cdma_generic_cb(ok, result, user_data);

	/* TODO: this should come from a modem solicited notification */
	ofono_cdma_voicecall_disconnected(cbd->user,
					OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
					NULL);
}

static void cdma_hangup(struct ofono_cdma_voicecall *vc,
				ofono_cdma_voicecall_cb_t cb, void *data)
{
	/* Hangup active call */
	cdma_template("AT+CHV", vc, cdma_hangup_cb, cb, data);
}

static gboolean cdma_voicecall_initialized(gpointer user_data)
{
	struct ofono_cdma_voicecall *vc = user_data;

	ofono_cdma_voicecall_register(vc);

	return FALSE;
}

static int cdma_voicecall_probe(struct ofono_cdma_voicecall *vc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->chat = g_at_chat_clone(chat);
	vd->vendor = vendor;

	ofono_cdma_voicecall_set_data(vc, vd);
	g_idle_add(cdma_voicecall_initialized, vc);

	return 0;
}

static void cdma_voicecall_remove(struct ofono_cdma_voicecall *vc)
{
	struct voicecall_data *vd = ofono_cdma_voicecall_get_data(vc);

	ofono_cdma_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static struct ofono_cdma_voicecall_driver driver = {
	.name			= "cdmamodem",
	.probe			= cdma_voicecall_probe,
	.remove			= cdma_voicecall_remove,
	.dial			= cdma_dial,
	.hangup			= cdma_hangup,
};

void cdma_voicecall_init(void)
{
	ofono_cdma_voicecall_driver_register(&driver);
}

void cdma_voicecall_exit(void)
{
	ofono_cdma_voicecall_driver_unregister(&driver);
}
