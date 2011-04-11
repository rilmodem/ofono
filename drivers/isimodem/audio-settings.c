/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <ofono/audio-settings.h>

#include <gisi/modem.h>
#include <gisi/client.h>
#include <gisi/message.h>

#include "isiutil.h"
#include "isimodem.h"
#include "call.h"
#include "debug.h"

struct audio_settings_data {
	GIsiClient *client;
};

static void isi_call_server_status_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_audio_settings *oas = data;
	uint8_t status;

	if (g_isi_msg_id(msg) != CALL_SERVER_STATUS_IND)
		return;

	if (!g_isi_msg_data_get_byte(msg, 0, &status))
		return;

	ofono_audio_settings_active_notify(oas, status ? TRUE : FALSE);
}

static void isi_call_verify_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_audio_settings *as = data;
	struct audio_settings_data *asd = ofono_audio_settings_get_data(as);

	if (g_isi_msg_error(msg) < 0) {
		ofono_audio_settings_remove(as);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	g_isi_client_ind_subscribe(asd->client, CALL_SERVER_STATUS_IND,
					isi_call_server_status_ind_cb,
					as);

	ofono_audio_settings_register(as);
}

static int isi_audio_settings_probe(struct ofono_audio_settings *as,
					unsigned int vendor, void *data)
{
	GIsiModem *modem = data;
	struct audio_settings_data *asd;

	asd = g_try_new0(struct audio_settings_data, 1);
	if (asd == NULL)
		return -ENOMEM;

	asd->client = g_isi_client_create(modem, PN_CALL);
	if (asd->client == NULL) {
		g_free(asd);
		return -ENOMEM;
	}

	ofono_audio_settings_set_data(as, asd);

	g_isi_client_verify(asd->client, isi_call_verify_cb, as, NULL);

	return 0;
}

static void isi_audio_settings_remove(struct ofono_audio_settings *as)
{
	struct audio_settings_data *asd = ofono_audio_settings_get_data(as);

	ofono_audio_settings_set_data(as, NULL);

	if (asd == NULL)
		return;

	g_isi_client_destroy(asd->client);
	g_free(asd);
}

static struct ofono_audio_settings_driver driver = {
	.name		= "isimodem",
	.probe		= isi_audio_settings_probe,
	.remove		= isi_audio_settings_remove,
};

void isi_audio_settings_init(void)
{
	ofono_audio_settings_driver_register(&driver);
}

void isi_audio_settings_exit(void)
{
	ofono_audio_settings_driver_unregister(&driver);
}
