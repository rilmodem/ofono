/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Rémi Denis-Courmont <remi.denis-courmont@nokia.com>
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

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>
#include "phonet.h"
#include <glib.h>

#include "socket.h"
#include "client.h"

struct _GIsiClient {
	uint8_t resource;
	struct {
		int major;
		int minor;
	} version;
	GIsiModem *modem;

	/* Requests */
	int fd;
	guint source;
	uint8_t prev[256], next[256];
	guint timeout[256];
	GIsiResponseFunc func[256];
	void *data[256];

	/* Indications */
	struct {
		int fd;
		guint source;
		uint16_t count;
		GIsiIndicationFunc func[256];
		void *data[256];
	} ind;

	/* Debugging */
	GIsiDebugFunc debug_func;
	void *debug_data;
};

static gboolean g_isi_callback(GIOChannel *channel, GIOCondition cond,
				gpointer data);
static gboolean g_isi_timeout(gpointer data);

static inline GIsiRequest *g_isi_req(GIsiClient *cl, uint8_t id)
{
	return (GIsiRequest *)(((uint8_t *)(void *)cl) + id);
}

static inline uint8_t g_isi_id(void *ptr)
{
	return ((uintptr_t)ptr) & 255;
}

static inline GIsiClient *g_isi_cl(void *ptr)
{
	return (GIsiClient *)(((uintptr_t)ptr) & ~255);
}

/**
 * Create an ISI client.
 * @param resource PhoNet resource ID for the client
 * @return NULL on error (see errno), a GIsiClient pointer on success,
 */
GIsiClient *g_isi_client_create(GIsiModem *modem, uint8_t resource)
{
	void *ptr;
	GIsiClient *cl;
	GIOChannel *channel;
	unsigned i;

	if (G_UNLIKELY(posix_memalign(&ptr, 256, sizeof(*cl))))
		abort();
	cl = ptr;
	cl->resource = resource;
	cl->version.major = -1;
	cl->version.minor = -1;
	cl->modem = modem;
	cl->debug_func = NULL;
	memset(cl->timeout, 0, sizeof(cl->timeout));
	for (i = 0; i < 256; i++) {
		cl->data[i] = cl->ind.data[i] = NULL;
		cl->func[i] = NULL;
		cl->ind.func[i] = NULL;
	}
	cl->ind.count = 0;

	/* Reserve 0 as head of available IDs, and 255 as head of busy ones */
	cl->prev[0] = 254;
	for (i = 0; i < 254; i++) {
		cl->next[i] = i + 1;
		cl->prev[i + 1] = i;
	}
	cl->next[254] = 0;
	cl->prev[255] = cl->next[255] = 255;

	channel = phonet_new(modem, resource);
	if (channel == NULL) {
		free(cl);
		return NULL;
	}
	cl->fd = g_io_channel_unix_get_fd(channel);
	cl->source = g_io_add_watch(channel,
					G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
					g_isi_callback, cl);
	g_io_channel_unref(channel);
	return cl;
}

/**
 * Set the ISI resource version of @a client.
 * @param client client for the resource
 * @param major ISI major version
 * @param minor ISI minor version
 */
void g_isi_version_set(GIsiClient *client, int major, int minor)
{
	if (!client)
		return;

	client->version.major = major;
	client->version.minor = minor;
}

/**
 * Returns the ISI major version of the resource associated with @a
 * client.
 * @param client client for the resource
 * @return major version, -1 if not available
 */
int g_isi_version_major(GIsiClient *client)
{
	return client->version.major;
}

/**
 * Returns the ISI minor version of the resource associated with @a
 * client.
 * @param client client for the resource
 * @return minor version, -1 if not available
 */
int g_isi_version_minor(GIsiClient *client)
{
	return client->version.minor;
}

/**
 * Returns the resource associated with @a client
 * @param client client for the resource
 * @return PhoNet resource ID for the client
 */
uint8_t g_isi_client_resource(GIsiClient *client)
{
	return client->resource;
}

/**
 * Set a debugging function for @a client. This function will be
 * called whenever an ISI protocol message is sent or received.
 * @param client client to debug
 * @param func debug function
 * @param opaque user data
 */
void g_isi_client_set_debug(GIsiClient *client, GIsiDebugFunc func,
				void *opaque)
{
	if (!client)
		return;

	client->debug_func = func;
	client->debug_data = opaque;
}

/**
 * Destroys an ISI client, cancels all pending transactions and subscriptions.
 * @param client client to destroy
 */
void g_isi_client_destroy(GIsiClient *client)
{
	unsigned id;

	g_source_remove(client->source);
	for (id = 0; id < 256; id++)
		if (client->timeout[id] > 0)
			g_source_remove(client->timeout[id]);
	if (client->ind.count > 0)
		g_source_remove(client->ind.source);
	free(client);
}

/**
 * Make an ISI request and register a callback to process the response(s) to
 * the resulting transaction.
 * @param cl ISI client (from g_isi_client_create())
 * @param buf pointer to request payload
 * @param len request payload byte length
 * @param cb callback to process response(s)
 * @param opaque data for the callback
 */
GIsiRequest *g_isi_request_make(GIsiClient *cl,	const void *__restrict buf,
				size_t len, unsigned timeout,
				GIsiResponseFunc cb, void *opaque)
{
	struct iovec iov[2];
	const struct sockaddr_pn dst = {
		.spn_family = AF_PHONET,
		.spn_resource = cl->resource,
	};
	const struct msghdr msg = {
		.msg_name = (struct sockaddr *)&dst,
		.msg_namelen = sizeof(dst),
		.msg_iov = (struct iovec *)iov,
		.msg_iovlen = 2,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	ssize_t ret;
	uint8_t id = cl->next[0];

	if (id == 0) {
		errno = EBUSY;
		return NULL;
	}
	if (cb == NULL) {
		errno = EINVAL;
		return NULL;
	}
	iov[0].iov_base = &id;
	iov[0].iov_len = 1;
	iov[1].iov_base = (void *)buf;
	iov[1].iov_len = len;
	ret = sendmsg(cl->fd, &msg, MSG_NOSIGNAL);
	if (ret == -1)
		return NULL;
	if (ret != (ssize_t)(len + 1)) {
		errno = EMSGSIZE;
		return NULL;
	}

	if (cl->debug_func)
		cl->debug_func(buf, len, cl->debug_data);

	cl->func[id] = cb;
	cl->data[id] = opaque;

	/* Remove transaction from available list */
	cl->next[0] = cl->next[id];
	cl->prev[cl->next[id]] = 0;
	/* Insert into busy list */
	cl->next[id] = cl->next[255];
	cl->prev[cl->next[id]] = id;
	cl->next[255] = id;
	cl->prev[id] = 255;

	if (timeout > 0)
		cl->timeout[id] = g_timeout_add_seconds(timeout,
							g_isi_timeout,
							g_isi_req(cl, id));
	else
		cl->timeout[id] = 0;
	return g_isi_req(cl, id);
}

/**
 * Cancels a pending request, i.e. stop waiting for responses and cancels the
 * timeout.
 * @param req request to cancel
 */
void g_isi_request_cancel(GIsiRequest *req)
{
	GIsiClient *cl = g_isi_cl(req);
	uint8_t id = g_isi_id(req);

	cl->func[id] = NULL;
	cl->data[id] = NULL;

	/* Remove transaction from pending circular list */
	cl->prev[cl->next[id]] = cl->prev[id];
	cl->next[cl->prev[id]] = cl->next[id];
	/* Insert transaction into available circular list */
	cl->prev[id] = cl->prev[0];
	cl->prev[0] = id;
	cl->next[id] = 0;
	cl->next[cl->prev[id]] = id;

	if (cl->timeout[id] > 0) {
		g_source_remove(cl->timeout[id]);
		cl->timeout[id] = 0;
	}
}

#define PN_COMMGR 0x10
#define PNS_SUBSCRIBED_RESOURCES_IND	0x10

static const struct sockaddr_pn commgr = {
	.spn_family = AF_PHONET,
	.spn_resource = PN_COMMGR,
};

static int g_isi_indication_init(GIsiClient *cl)
{
	uint8_t msg[] = {
		0, PNS_SUBSCRIBED_RESOURCES_IND, 1, cl->resource,
	};
	GIOChannel *channel = phonet_new(cl->modem, PN_COMMGR);

	if (channel == NULL)
		return errno;
	/* Send subscribe indication */
	cl->ind.fd = g_io_channel_unix_get_fd(channel);
	sendto(cl->ind.fd, msg, 4, MSG_NOSIGNAL,
		(const struct sockaddr *)&commgr, sizeof(commgr));
	cl->ind.source = g_io_add_watch(channel,
					G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
					g_isi_callback, cl);
	return 0;
}

static void g_isi_indication_deinit(GIsiClient *client)
{
	uint8_t msg[] = {
		0, PNS_SUBSCRIBED_RESOURCES_IND, 0,
	};

	/* Send empty subscribe indication */
	sendto(client->ind.fd, msg, 3, MSG_NOSIGNAL,
		(const struct sockaddr *)&commgr, sizeof(commgr));
	g_source_remove(client->ind.source);
}

/**
 * Subscribe to a given indication type for the resource that an ISI client
 * is associated with. If the same type was already subscrived, the old
 * subscription is overriden.
 * @param cl ISI client (fomr g_isi_client_create())
 * @param type indication type
 * @param cb callback to process received indications
 * @param data data for the callback
 * @return 0 on success, a system error code otherwise.
 */
int g_isi_subscribe(GIsiClient *cl, uint8_t type,
			GIsiIndicationFunc cb, void *data)
{
	if (cb == NULL)
		return EINVAL;

	if (cl->ind.func[type] == NULL) {
		if (cl->ind.count == 0) {
			int ret = g_isi_indication_init(cl);
			if (ret)
				return ret;
		}
		cl->ind.count++;
	}
	cl->ind.func[type] = cb;
	cl->ind.data[type] = data;
	return 0;
}

/**
 * Unsubscribe from a given indication type.
 * @param client ISI client (from g_isi_client_create())
 * @param type indication type.
 */
void g_isi_unsubscribe(GIsiClient *client, uint8_t type)
{
	/* Unsubscribe */
	if (client->ind.func[type] == NULL)
		return;
	client->ind.func[type] = NULL;
	if (--client->ind.count == 0)
		g_isi_indication_deinit(client);
}

/* Data callback for both responses and indications */
static gboolean g_isi_callback(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GIsiClient *cl = data;
	int fd = g_io_channel_unix_get_fd(channel);
	bool indication = (fd != cl->fd);
	int len;

	if (cond & (G_IO_NVAL|G_IO_HUP)) {
		g_warning("Unexpected event on Phonet channel %p", channel);
		return FALSE;
	}

	len = phonet_peek_length(channel);
	{
		uint32_t buf[(len + 3) / 4];
		uint8_t *msg;
		uint16_t obj;
		uint8_t res, id;

		len = phonet_read(channel, buf, len, &obj, &res);
		if (len < 2 || res != cl->resource)
			return TRUE;

		msg = (uint8_t *)buf;

		if (cl->debug_func)
			cl->debug_func(msg + 1, len - 1, cl->debug_data);

		if (indication) {
			/* Message ID at offset 1 */
			id = msg[1];
			if (cl->ind.func[id] == NULL)
				return TRUE; /* Unsubscribed indication */

			cl->ind.func[id](cl, msg + 1, len - 1, obj,
						cl->ind.data[id]);
		} else {
			/* Transaction ID at offset 0 */
			id = msg[0];
			if (cl->func[id] == NULL)
				return TRUE; /* Bad transaction ID */

			if ((cl->func[id])(cl, msg + 1, len - 1, obj,
						cl->data[id]))
				g_isi_request_cancel(g_isi_req(cl, id));
		}
	}
	return TRUE;
}

static gboolean g_isi_timeout(gpointer data)
{
	GIsiRequest *req = data;
	GIsiClient *cl = g_isi_cl(req);
	uint8_t id = g_isi_id(req);

	assert(cl->func[id]);
	(cl->func[id])(cl, NULL, 0, 0, cl->data[id]);
	g_isi_request_cancel(req);
	return FALSE;
}

int g_isi_client_error(const GIsiClient *client)
{	/* The only possible error at the moment */
	return -ETIMEDOUT;
}
