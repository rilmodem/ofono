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

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <glib.h>
#include "phonet.h"

#include "server.h"

struct pending_data {
	GIsiServer *server;
	GIsiNotifyFunc notify;
	void *data;
};

struct _GIsiServer {
	GIsiModem *modem;
	GIsiVersion version;
	uint8_t resource;
	GSList *pending;
};

static void pending_notify(const GIsiMessage *msg, void *data)
{
	struct pending_data *pd = data;

	if (!pd)
		return;

	if (pd->notify)
		pd->notify(msg, pd->data);
}

uint8_t g_isi_server_resource(GIsiServer *server)
{
	return server ? server->resource : 0;
}

GIsiModem *g_isi_server_modem(GIsiServer *server)
{
	return server ? server->modem : 0;
}

GIsiServer *g_isi_server_create(GIsiModem *modem, uint8_t resource,
				GIsiVersion *version)
{
	GIsiServer *server;

	if (!modem) {
		errno = EINVAL;
		return NULL;
	}

	server  = g_try_new0(GIsiServer, 1);
	if (!server) {
		errno = ENOMEM;
		return NULL;
	}

	if (version)
		memcpy(&server->version, version, sizeof(GIsiVersion));

	server->resource = resource;
	server->modem = modem;
	server->pending = NULL;

	return server;
}

static void foreach_destroy(gpointer value, gpointer user)
{
	GIsiPending *op = value;
	GIsiServer *server = user;

	if (!op || !server)
		return;

	server->pending = g_slist_remove(server->pending, op);
	g_isi_pending_remove(op);
}

void g_isi_server_destroy(GIsiServer *server)
{
	if (!server)
		return;

	g_slist_foreach(server->pending, foreach_destroy, server);
	g_slist_free(server->pending);
	g_free(server);
}

int g_isi_server_send(GIsiServer *server, const GIsiMessage *req,
			const void *__restrict buf, size_t len)
{
	if (!server)
		return -EINVAL;

	return g_isi_response_send(server->modem, req, buf, len);
}

int g_isi_server_vsend(GIsiServer *server, const GIsiMessage *req,
			const struct iovec *iov, size_t iovlen)
{
	if (!server)
		return -EINVAL;

	return g_isi_response_vsend(server->modem, req, iov, iovlen);
}

static struct pending_data *pending_data_create(GIsiServer *server,
						GIsiNotifyFunc notify,
						void *data)
{
	struct pending_data *pd;

	if (!server) {
		errno = EINVAL;
		return NULL;
	}

	pd = g_try_new0(struct pending_data, 1);
	if (!pd) {
		errno = ENOMEM;
		return NULL;
	}

	pd->server = server;
	pd->notify = notify;
	pd->data = data;

	return pd;
}

GIsiPending *g_isi_server_handle(GIsiServer *server, uint8_t type,
					GIsiNotifyFunc notify, void *data)
{
	struct pending_data *pd;
	GIsiPending *op;

	pd = pending_data_create(server, notify, data);
	if (!pd)
		return NULL;

	op = g_isi_service_bind(server->modem, server->resource, type,
				pending_notify, pd, g_free);
	if (!op) {
		g_free(pd);
		return NULL;
	}

	server->pending = g_slist_append(server->pending, op);
	return op;
}
