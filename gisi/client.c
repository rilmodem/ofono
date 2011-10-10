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

#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <glib.h>

#include "client.h"

struct _GIsiClient {
	GIsiModem *modem;
	unsigned timeout;
	uint8_t resource;
};

uint8_t g_isi_client_resource(GIsiClient *client)
{
	return client != NULL ? client->resource : 0;
}

GIsiModem *g_isi_client_modem(GIsiClient *client)
{
	return client != NULL ? client->modem : NULL;
}

GIsiClient *g_isi_client_create(GIsiModem *modem, uint8_t resource)
{
	GIsiClient *client;

	if (modem == NULL) {
		errno = EINVAL;
		return NULL;
	}

	client  = g_try_new0(GIsiClient, 1);
	if (client == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	client->timeout = G_ISI_CLIENT_DEFAULT_TIMEOUT;
	client->resource = resource;
	client->modem = modem;

	return client;
}

void g_isi_client_reset(GIsiClient *client)
{
	g_isi_remove_pending_by_owner(client->modem, client->resource, client);
};

void g_isi_client_destroy(GIsiClient *client)
{
	if (client == NULL)
		return;

	g_isi_client_reset(client);
	g_free(client);
}

void g_isi_client_set_timeout(GIsiClient *client, unsigned timeout)
{
	if (client == NULL)
		return;

	client->timeout = timeout;
}

gboolean g_isi_client_send(GIsiClient *client,
			const void *__restrict msg, size_t len,
			GIsiNotifyFunc notify, void *data,
			GDestroyNotify destroy)
{
	GIsiPending *op;

	op = g_isi_request_send(client->modem, client->resource, msg, len,
				client->timeout, notify, data, destroy);

	g_isi_pending_set_owner(op, client);

	return op != NULL;
}

gboolean g_isi_client_send_with_timeout(GIsiClient *client,
				const void *__restrict buf, size_t len,
				unsigned timeout,
				GIsiNotifyFunc notify, void *data,
				GDestroyNotify destroy)
{
	GIsiPending *op;

	op = g_isi_request_send(client->modem, client->resource, buf, len,
				timeout, notify, data, destroy);

	g_isi_pending_set_owner(op, client);

	return op != NULL;
}

gboolean g_isi_client_vsend(GIsiClient *client,
			const struct iovec *iov, size_t iovlen,
			GIsiNotifyFunc notify, void *data,
			GDestroyNotify destroy)
{
	GIsiPending *op;

	op = g_isi_request_vsend(client->modem, client->resource, iov, iovlen,
				client->timeout, notify, data, destroy);

	g_isi_pending_set_owner(op, client);

	return op != NULL;
}

gboolean g_isi_client_vsend_with_timeout(GIsiClient *client,
				const struct iovec *__restrict iov,
				size_t iovlen, unsigned timeout,
				GIsiNotifyFunc notify, void *data,
				GDestroyNotify destroy)
{
	GIsiPending *op;

	op = g_isi_request_vsend(client->modem, client->resource, iov, iovlen,
					timeout, notify, data, destroy);

	g_isi_pending_set_owner(op, client);

	return op != NULL;
}

gboolean g_isi_client_ind_subscribe(GIsiClient *client, uint8_t type,
					GIsiNotifyFunc notify, void *data)
{
	GIsiPending *op;

	op = g_isi_ind_subscribe(client->modem, client->resource, type,
					notify, data, NULL);

	g_isi_pending_set_owner(op, client);

	return op != NULL;
}

gboolean g_isi_client_ntf_subscribe(GIsiClient *client, uint8_t type,
					GIsiNotifyFunc notify, void *data)
{
	GIsiPending *op;

	op = g_isi_ntf_subscribe(client->modem, client->resource, type,
					notify, data, NULL);

	g_isi_pending_set_owner(op, client);

	return op != NULL;
}

gboolean g_isi_client_verify(GIsiClient *client, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy)
{
	GIsiPending *op;

	op = g_isi_resource_ping(client->modem, client->resource,
					notify, data, destroy);

	g_isi_pending_set_owner(op, client);

	return op != NULL;
}
