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

#include <gisi/client.h>

#include "isimodem.h"
#include "call.h"
#include "debug.h"

struct audio_settings_data {
	GIsiClient *client;
};

static void isi_call_server_status_ind_cb(GIsiClient *client,
			void const *restrict data,
			size_t len,
			uint16_t object,
			void *_oas)
{
	struct ofono_audio_settings *oas = _oas;
	struct {
		uint8_t message_id, server_status, sub_blocks;
	} const *m = data;
	gboolean call_server_status;

	DBG("Call server status changed");

	if (len < 3)
		return;

	call_server_status = m->server_status & 0xf ? TRUE : FALSE;
	ofono_audio_settings_active_notify(oas, call_server_status);
}

static gboolean isi_call_register(gpointer _oas)
{
	struct ofono_audio_settings *oas = _oas;
	struct audio_settings_data *asd = ofono_audio_settings_get_data(oas);
	const char *debug = getenv("OFONO_ISI_DEBUG");

	if (debug && (strcmp(debug, "all") == 0) == 0)
		g_isi_client_set_debug(asd->client, call_debug, NULL);

	g_isi_subscribe(asd->client,
			CALL_SERVER_STATUS_IND, isi_call_server_status_ind_cb,
			oas);

	ofono_audio_settings_register(oas);

	return FALSE;
}

static void isi_call_verify_cb(GIsiClient *client,
				gboolean alive, uint16_t object, void *ovc)
{
	if (!alive) {
		DBG("Unable to bootstrap audio settings driver");
		return;
	}

	DBG("%s (v%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_idle_add(isi_call_register, ovc);
}

static int isi_audio_settings_probe(struct ofono_audio_settings *as,
					unsigned int vendor, void *data)
{
	GIsiModem *idx = data;
	struct audio_settings_data *asd =
		g_try_new0(struct audio_settings_data, 1);

	if (!asd)
		return -ENOMEM;

	asd->client = g_isi_client_create(idx, PN_CALL);
	if (!asd->client) {
		g_free(asd);
		return -ENOMEM;
	}

	ofono_audio_settings_set_data(as, asd);

	if (!g_isi_verify(asd->client, isi_call_verify_cb, as))
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_audio_settings_remove(struct ofono_audio_settings *as)
{
	struct audio_settings_data *asd = ofono_audio_settings_get_data(as);

	if (!asd)
		return;

	ofono_audio_settings_set_data(as, NULL);
	g_isi_client_destroy(asd->client);
	g_free(asd);
}

static struct ofono_audio_settings_driver driver = {
	.name		= "isimodem",
	.probe		= isi_audio_settings_probe,
	.remove		= isi_audio_settings_remove,
};

void isi_audio_settings_init()
{
	ofono_audio_settings_driver_register(&driver);
}

void isi_audio_settings_exit()
{
	ofono_audio_settings_driver_unregister(&driver);
}
