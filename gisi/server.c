/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <glib.h>
#include "phonet.h"

#include "server.h"

struct _GIsiServer {
	GIsiModem *modem;
	GIsiVersion version;
	uint8_t resource;
};

uint8_t g_isi_server_resource(GIsiServer *server)
{
	return server != NULL ? server->resource : 0;
}

GIsiModem *g_isi_server_modem(GIsiServer *server)
{
	return server != NULL ? server->modem : 0;
}

GIsiServer *g_isi_server_create(GIsiModem *modem, uint8_t resource,
				GIsiVersion *version)
{
	GIsiServer *server;

	if (modem == NULL) {
		errno = EINVAL;
		return NULL;
	}

	server  = g_try_new0(GIsiServer, 1);
	if (server == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	if (version != NULL)
		memcpy(&server->version, version, sizeof(GIsiVersion));

	server->resource = resource;
	server->modem = modem;

	return server;
}

void g_isi_server_destroy(GIsiServer *server)
{
	if (server == NULL)
		return;

	g_isi_remove_pending_by_owner(server->modem, server->resource, server);

	g_free(server);
}

int g_isi_server_send(GIsiServer *server, const GIsiMessage *req,
			const void *__restrict buf, size_t len)
{
	if (server == NULL)
		return -EINVAL;

	return g_isi_response_send(server->modem, req, buf, len);
}

int g_isi_server_vsend(GIsiServer *server, const GIsiMessage *req,
			const struct iovec *iov, size_t iovlen)
{
	if (server == NULL)
		return -EINVAL;

	return g_isi_response_vsend(server->modem, req, iov, iovlen);
}

GIsiPending *g_isi_server_handle(GIsiServer *server, uint8_t type,
					GIsiNotifyFunc notify, void *data)
{
	GIsiPending *op;

	op = g_isi_service_bind(server->modem, server->resource, type,
				notify, data, NULL);

	g_isi_pending_set_owner(op, server);

	return op;
}
