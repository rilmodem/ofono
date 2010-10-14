/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <gisi/server.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/log.h>
#include <ofono/modem.h>

#include "info.h"
#include "infoserver.h"

struct isi_infoserver {
	GIsiServer *server;
	unsigned sv;	/* Software version in 0..98 */
};

static gboolean serial_number_read_req(GIsiServer *server, void const *data,
					size_t len, GIsiIncoming *irq,
					void *opaque)
{
	struct isi_infoserver *self = opaque;
	struct {
		uint8_t mid;
		uint8_t target;
	} const *req = data;

	/* IMEISV defined in 3GPP TS 23.003 section 6.2.2 */

	if (req->target == INFO_SB_SN_IMEI_SV_TO_NET) {
		const uint8_t response[] = {
			INFO_SERIAL_NUMBER_READ_RESP, INFO_OK, 1,
			INFO_SB_SN_IMEI_SV_TO_NET, 16,
			/* Mobile Identity IE, TS 24.008 section 10.5.1.4 */
			0, 9,
			/* F in place of IMEI digits and filler */
			0xf3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0x0f | ((self->sv / 10) << 4),
			0xf0 | ((self->sv % 10) & 0x0f),

			/* Subblock filler */
			0, 0, 0
		};

		DBG("Sending IMEI SV code %02u to modem", self->sv);
		g_isi_respond(server, response, sizeof response, irq);

	} else {
		const uint8_t error[] = {
			INFO_SERIAL_NUMBER_READ_RESP,
			INFO_NOT_SUPPORTED,
			0
		};

		DBG("Unknown target 0x%02X", req->target);
		g_isi_respond(server, error, sizeof error, irq);
	}

	return TRUE;
}

struct isi_infoserver *isi_infoserver_create(struct ofono_modem *modem,
						void *data)
{
	struct isi_infoserver *self;

	self = g_new0(struct isi_infoserver, 1);
	if (!self)
		return NULL;

	self->server = g_isi_server_create(data, PN_EPOC_INFO, 0, 0);
	if (!self->server) {
		g_free(self);
		return NULL;
	}

	g_isi_server_add_name(self->server);

	g_isi_server_handle(self->server,
				INFO_SERIAL_NUMBER_READ_REQ,
				serial_number_read_req,
				self);

	DBG("created %p", self);

	return self;
}

void isi_infoserver_destroy(struct isi_infoserver *self)
{
	DBG("destroy %p", self);

	if (self) {
		g_isi_server_destroy(self->server);
		g_free(self);
	}
}
