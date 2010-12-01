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
#include <gisi/modem.h>
#include <gisi/server.h>
#include <gisi/message.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/log.h>
#include <ofono/modem.h>

#include "info.h"
#include "infoserver.h"

struct isi_infoserver {
	GIsiServer *server;
	unsigned sv;	/* Software version in 0..98 */
};

static GIsiVersion isiversion = {
	.major = 0,
	.minor = 0,
};

static void send_error(GIsiServer *server, const GIsiMessage *req, uint8_t code)
{
	const uint8_t error[] = {
		INFO_SERIAL_NUMBER_READ_RESP,
		code,
		0
	};

	g_isi_server_send(server, req, error, sizeof(error));
}

static void send_response(GIsiServer *server, const GIsiMessage *req,
				unsigned sv)
{
	const uint8_t resp[] = {
		INFO_SERIAL_NUMBER_READ_RESP, INFO_OK, 1,
		INFO_SB_SN_IMEI_SV_TO_NET, 16,
		/* Mobile Identity IE, TS 24.008 section 10.5.1.4 */
		0, 9,
		/* F in place of IMEI digits and filler */
		0xf3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x0f | ((sv / 10) << 4),
		0xf0 | ((sv % 10) & 0x0f),
		/* Subblock filler */
		0, 0, 0,
	};

	g_isi_server_send(server, req, resp, sizeof(resp));
}

static void serial_number_read_req(const GIsiMessage *msg, void *data)
{
	struct isi_infoserver *self = data;
	uint8_t target;

	if (g_isi_msg_id(msg) != INFO_SERIAL_NUMBER_READ_REQ)
		return;

	if (!g_isi_msg_data_get_byte(msg, 0, &target)) {
		send_error(self->server, msg, INFO_FAIL);
		return;
	}

	if (target == INFO_SB_SN_IMEI_SV_TO_NET) {
		/* IMEISV defined in 3GPP TS 23.003 section 6.2.2 */
		send_response(self->server, msg, self->sv);
		return;
	}

	DBG("Unknown query target 0x%02X", target);
	send_error(self->server, msg, INFO_NOT_SUPPORTED);
}

struct isi_infoserver *isi_infoserver_create(struct ofono_modem *modem,
						void *data)
{
	struct isi_infoserver *self;
	GIsiModem *isimodem = data;

	if (isimodem == NULL) {
		errno = EINVAL;
		return NULL;
	}

	self = g_try_new0(struct isi_infoserver, 1);
	if (self == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	self->server = g_isi_server_create(isimodem, PN_EPOC_INFO, &isiversion);
	if (self->server == NULL) {
		g_free(self);
		errno = ENOMEM;
		return NULL;
	}

	g_isi_server_handle(self->server,
				INFO_SERIAL_NUMBER_READ_REQ,
				serial_number_read_req,
				self);

	return self;
}

void isi_infoserver_destroy(struct isi_infoserver *self)
{
	if (self == NULL)
		return;

	g_isi_server_destroy(self->server);
	g_free(self);
}
