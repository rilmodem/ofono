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

#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <glib.h>

#include "client.h"

struct pending_data {
	GIsiClient *client;
	GIsiNotifyFunc notify;
	void *data;
	GDestroyNotify destroy;
};

struct _GIsiClient {
	GIsiModem *modem;
	uint8_t resource;
	GSList *pending;
};

static void pending_destroy(gpointer data)
{
	struct pending_data *pd = data;

	if (!pd)
		return;

	if (pd->destroy)
		pd->destroy(pd->data);

	g_free(pd);
}

static void pending_resp_notify(const GIsiMessage *msg, void *data)
{
	struct pending_data *pd = data;

	if (!pd)
		return;

	if (pd->notify)
		pd->notify(msg, pd->data);

	pd->client->pending = g_slist_remove(pd->client->pending,
						g_isi_pending_from_msg(msg));
}

static void pending_notify(const GIsiMessage *msg, void *data)
{
	struct pending_data *pd = data;

	if (!pd)
		return;

	if (pd->notify)
		pd->notify(msg, pd->data);
}

uint8_t g_isi_client_resource(GIsiClient *client)
{
	return client ? client->resource : 0;
}

GIsiModem *g_isi_client_modem(GIsiClient *client)
{
	return client ? client->modem : NULL;
}

GIsiClient *g_isi_client_create(GIsiModem *modem, uint8_t resource)
{
	GIsiClient *client;

	if (!modem) {
		errno = EINVAL;
		return NULL;
	}

	client  = g_try_new0(GIsiClient, 1);
	if (!client) {
		errno = ENOMEM;
		return NULL;
	}

	client->resource = resource;
	client->modem = modem;
	client->pending = NULL;

	return client;
}

static void foreach_destroy(gpointer value, gpointer user)
{
	GIsiPending *op = value;
	GIsiClient *client = user;

	if (!op || !client)
		return;

	client->pending = g_slist_remove(client->pending, op);
	g_isi_pending_remove(op);
}

void g_isi_client_destroy(GIsiClient *client)
{
	if (client == NULL)
		return;

	g_slist_foreach(client->pending, foreach_destroy, client);
	g_slist_free(client->pending);
	g_free(client);
}

static struct pending_data *pending_data_create(GIsiClient *client,
						GIsiNotifyFunc notify,
						void *data,
						GDestroyNotify destroy)
{
	struct pending_data *pd;

	if (client == NULL) {
		errno = EINVAL;
		return NULL;
	}

	pd = g_try_new0(struct pending_data, 1);
	if (!pd) {
		errno = ENOMEM;
		return NULL;
	}

	pd->client = client;
	pd->notify = notify;
	pd->data = data;
	pd->destroy = destroy;

	return pd;
}

GIsiPending *g_isi_client_send(GIsiClient *client, const void *__restrict buf,
				size_t len, unsigned timeout,
				GIsiNotifyFunc notify, void *data,
				GDestroyNotify destroy)
{
	struct pending_data *pd;
	GIsiPending *op;

	pd = pending_data_create(client, notify, data, destroy);
	if (!pd)
		return NULL;

	op = g_isi_request_send(client->modem, client->resource, buf, len, timeout,
				pending_resp_notify, pd, pending_destroy);
	if (!op) {
		g_free(pd);
		return NULL;
	}

	client->pending = g_slist_append(client->pending, op);
	return op;
}

GIsiPending *g_isi_client_vsend(GIsiClient *client,
				const struct iovec *__restrict iov,
				size_t iovlen, unsigned timeout,
				GIsiNotifyFunc notify, void *data,
				GDestroyNotify destroy)
{
	struct pending_data *pd;
	GIsiPending *op;

	pd = pending_data_create(client, notify, data, destroy);
	if (!pd)
		return NULL;

	op = g_isi_request_vsend(client->modem, client->resource, iov, iovlen,
					timeout, pending_resp_notify, pd,
					pending_destroy);
	if (!op) {
		g_free(pd);
		return NULL;
	}

	client->pending = g_slist_append(client->pending, op);
	return op;
}

GIsiPending *g_isi_client_ind_subscribe(GIsiClient *client, uint8_t type,
					GIsiNotifyFunc notify, void *data)
{
	struct pending_data *pd;
	GIsiPending *op;

	pd = pending_data_create(client, notify, data, NULL);
	if (!pd)
		return NULL;

	op = g_isi_ind_subscribe(client->modem, client->resource, type,
					pending_notify, pd, pending_destroy);
	if (!op) {
		g_free(pd);
		return NULL;
	}

	client->pending = g_slist_append(client->pending, op);
	return op;
}

GIsiPending *g_isi_client_ntf_subscribe(GIsiClient *client, uint8_t type,
					GIsiNotifyFunc notify, void *data)
{
	struct pending_data *pd;
	GIsiPending *op;

	pd = pending_data_create(client, notify, data, NULL);
	if (!pd)
		return NULL;

	op = g_isi_ntf_subscribe(client->modem, client->resource, type,
					pending_notify, pd, pending_destroy);
	if (!op) {
		g_free(pd);
		return NULL;
	}

	client->pending = g_slist_append(client->pending, op);
	return op;
}

GIsiPending *g_isi_client_verify(GIsiClient *client, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy)
{
	struct pending_data *pd;
	GIsiPending *op;

	pd = pending_data_create(client, notify, data, destroy);
	if (!pd)
		return NULL;

	op = g_isi_resource_ping(client->modem, client->resource,
					pending_resp_notify, pd,
					pending_destroy);
	if (!op) {
		g_free(pd);
		return NULL;
	}

	client->pending = g_slist_append(client->pending, op);
	return op;
}
