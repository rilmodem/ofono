/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/netlink.h>
#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "isi.h"

#define PN_CALL			0x01

struct voicecall_data {
	GIsiClient *client;
	struct isi_version version;
};

static void isi_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *number,
			enum ofono_clir_option clir, enum ofono_cug_option cug,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_hangup(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_list_calls(struct ofono_voicecall *vc,
			ofono_call_list_cb_t cb, void *data)
{
}

static void isi_hold_all_active(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_release_all_held(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_set_udub(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_release_all_active(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_release_specific(struct ofono_voicecall *vc, int id,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_private_chat(struct ofono_voicecall *vc, int id,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_create_multiparty(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_deflect(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_swap_without_accept(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
}

static void isi_send_tones(struct ofono_voicecall *vc, const char *tones,
			ofono_voicecall_cb_t cb, void *data)
{
}

static int isi_voicecall_probe(struct ofono_voicecall *call,
				unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct voicecall_data *data = g_try_new0(struct voicecall_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_CALL);
	if (!data->client)
		return -ENOMEM;

	ofono_voicecall_set_data(call, data);

	return 0;
}

static void isi_voicecall_remove(struct ofono_voicecall *call)
{
	struct voicecall_data *data = ofono_voicecall_get_data(call);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_voicecall_driver driver = {
	.name			= "isimodem",
	.probe			= isi_voicecall_probe,
	.remove			= isi_voicecall_remove,
	.dial			= isi_dial,
	.answer			= isi_answer,
	.hangup			= isi_hangup,
	.list_calls		= isi_list_calls,
	.hold_all_active	= isi_hold_all_active,
	.release_all_held	= isi_release_all_held,
	.set_udub		= isi_set_udub,
	.release_all_active	= isi_release_all_active,
	.release_specific	= isi_release_specific,
	.private_chat		= isi_private_chat,
	.create_multiparty	= isi_create_multiparty,
	.transfer		= isi_transfer,
	.deflect		= isi_deflect,
	.swap_without_accept	= isi_swap_without_accept,
	.send_tones		= isi_send_tones,
};

void isi_voicecall_init()
{
	ofono_voicecall_driver_register(&driver);
}

void isi_voicecall_exit()
{
	ofono_voicecall_driver_unregister(&driver);
}
