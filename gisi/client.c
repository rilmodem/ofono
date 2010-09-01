/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <search.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>
#include "phonet.h"
#include <glib.h>

#include "socket.h"
#include "client.h"

#define PN_COMMGR			0x10
#define PNS_SUBSCRIBED_RESOURCES_IND	0x10

static const struct sockaddr_pn commgr = {
	.spn_family = AF_PHONET,
	.spn_resource = PN_COMMGR,
};

struct _GIsiRequest {
	unsigned int id; /* don't move, see g_isi_cmp */
	GIsiClient *client;
	guint timeout;
	GIsiResponseFunc func;
	void *data;
	GDestroyNotify notify;
};

struct _GIsiIndication {
	unsigned int type; /* don't move, see g_isi_cmp */
	GIsiIndicationFunc func;
	void *data;
};
typedef struct _GIsiIndication GIsiIndication;

struct _GIsiClient {
	uint8_t resource;
	struct {
		int major;
		int minor;
	} version;
	GIsiModem *modem;
	int error;

	/* Requests */
	struct {
		int fd;
		guint source;
		unsigned int last; /* last used transaction ID */
		void *pending;
	} reqs;

	/* Indications */
	struct {
		int fd;
		guint source;
		unsigned int count;
		void *subs;
	} inds;

	/* Debugging */
	GIsiDebugFunc debug_func;
	void *debug_data;
};

static gboolean g_isi_callback(GIOChannel *channel, GIOCondition cond,
				gpointer data);
static gboolean g_isi_timeout(gpointer data);

static void g_isi_vdebug(const struct iovec *__restrict iov,
				size_t iovlen, size_t total_len,
				GIsiDebugFunc func, void *data)
{
	uint8_t debug[total_len];
	uint8_t *ptr = debug;
	size_t i;

	for (i = 0; i < iovlen; i++) {
		memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
		ptr += iov[i].iov_len;
	}

	func(debug, total_len, data);
}


static int g_isi_cmp(const void *a, const void *b)
{
	const unsigned int *ua = (const unsigned int *)a;
	const unsigned int *ub = (const unsigned int *)b;

	return *ua - *ub;
}

/**
 * Create an ISI client.
 * @param resource PhoNet resource ID for the client
 * @return NULL on error (see errno), a GIsiClient pointer on success,
 */
GIsiClient *g_isi_client_create(GIsiModem *modem, uint8_t resource)
{
	GIsiClient *client;
	GIOChannel *channel;

	client  = g_try_new0(GIsiClient, 1);
	if (!client) {
		errno = ENOMEM;
		return NULL;
	}

	client->resource = resource;
	client->version.major = -1;
	client->version.minor = -1;
	client->modem = modem;
	client->error = 0;
	client->debug_func = NULL;

	client->reqs.last = 0;
	client->reqs.pending = NULL;

	client->inds.count = 0;
	client->inds.subs = NULL;

	channel = phonet_new(modem, resource);
	if (!channel) {
		g_free(client);
		return NULL;
	}
	client->reqs.fd = g_io_channel_unix_get_fd(channel);
	client->reqs.source = g_io_add_watch(channel,
					G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
					g_isi_callback, client);
	g_io_channel_unref(channel);

	return client;
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
	return client ? client->version.major : -1;
}

/**
 * Returns the ISI minor version of the resource associated with @a
 * client.
 * @param client client for the resource
 * @return minor version, -1 if not available
 */
int g_isi_version_minor(GIsiClient *client)
{
	return client ? client->version.minor : -1;
}

/**
 * Returns the resource associated with @a client
 * @param client client for the resource
 * @return PhoNet resource ID for the client
 */
uint8_t g_isi_client_resource(GIsiClient *client)
{
	return client ? client->resource : 0;
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

static void g_isi_cleanup_req(void *data)
{
	GIsiRequest *req = data;

	if (!req)
		return;

	/* Finalize any pending requests */
	req->client->error = ESHUTDOWN;
	if (req->func)
		req->func(req->client, NULL, 0, 0, req->data);
	req->client->error = 0;

	if (req->notify)
		req->notify(req->data);

	if (req->timeout > 0)
		g_source_remove(req->timeout);

	g_free(req);
}

static void g_isi_cleanup_ind(void *data)
{
	GIsiIndication *ind = data;

	if (!ind)
		return;

	g_free(ind);
}

static int g_isi_indication_init(GIsiClient *client)
{
	GIOChannel *channel;
	uint8_t msg[] = {
		0, PNS_SUBSCRIBED_RESOURCES_IND,
		1, client->resource,
	};

	channel = phonet_new(client->modem, PN_COMMGR);
	if (!channel)
		return errno;

	client->inds.fd = g_io_channel_unix_get_fd(channel);

	/* Subscribe by sending an indication */
	sendto(client->inds.fd, msg, 4, MSG_NOSIGNAL, (void *)&commgr,
		sizeof(commgr));
	client->inds.source = g_io_add_watch(channel,
					G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
					g_isi_callback, client);

	g_io_channel_unref(channel);
	return 0;
}

static void g_isi_indication_deinit(GIsiClient *client)
{
	uint8_t msg[] = {
		0, PNS_SUBSCRIBED_RESOURCES_IND,
		0,
	};

	/* Unsubscribe by sending an empty subscribe indication */
	sendto(client->inds.fd, msg, 3, MSG_NOSIGNAL, (void *)&commgr,
		sizeof(commgr));
}

/**
 * Destroys an ISI client, cancels all pending transactions and subscriptions.
 * @param client client to destroy (may be NULL)
 */
void g_isi_client_destroy(GIsiClient *client)
{
	if (!client)
		return;

	tdestroy(client->reqs.pending, g_isi_cleanup_req);
	tdestroy(client->inds.subs, g_isi_cleanup_ind);

	if (client->reqs.source > 0)
		g_source_remove(client->reqs.source);

	if (client->inds.source > 0)
		g_source_remove(client->inds.source);

	g_isi_indication_deinit(client);
	g_free(client);
}

/**
 * Make an ISI request and register a callback to process the response(s) to
 * the resulting transaction.
 * @param cl ISI client (from g_isi_client_create())
 * @param buf pointer to request payload
 * @param len request payload byte length
 * @param timeout timeout in seconds
 * @param cb callback to process response(s)
 * @param opaque data for the callback
 */
GIsiRequest *g_isi_request_make(GIsiClient *client, const void *__restrict buf,
				size_t len, unsigned timeout,
				GIsiResponseFunc cb, void *opaque)
{
	return g_isi_send(client, buf, len, timeout, cb, opaque, NULL);
}

/**
 * Make an ISI request and register a callback to process the response(s) to
 * the resulting transaction.
 * @param cl ISI client (from g_isi_client_create())
 * @param iov scatter-gather array to the request payload
 * @param iovlen number of vectors in the scatter-gather array
 * @param timeout timeout in seconds
 * @param cb callback to process response(s)
 * @param opaque data for the callback
 */
GIsiRequest *g_isi_request_vmake(GIsiClient *client, const struct iovec *iov,
					size_t iovlen, unsigned timeout,
					GIsiResponseFunc func, void *opaque)
{
	return g_isi_vsend(client, iov, iovlen, timeout, func, opaque, NULL);
}

/**
 * Send an ISI request and register a callback to process the response(s) to
 * the resulting transaction.
 *
 * @param cl ISI client (from g_isi_client_create())
 * @param buf pointer to request payload
 * @param len request payload byte length
 * @param timeout timeout in seconds
 * @param cb callback to process response(s)
 * @param opaque data for the callback
 * @param notify finalizer function for the @a opaque data (may be NULL)
 *
 * @return
 * A pointer to a newly created GIsiRequest.
 *
 * @errors
 * If an error occurs, @a errno is set accordingly and a NULL pointer is
 * returned.
 */
GIsiRequest *g_isi_send(GIsiClient *client,
			const void *__restrict buf, size_t len,
			unsigned timeout,
			GIsiResponseFunc cb, void *opaque,
			GDestroyNotify notify)
{
	const struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};

	return g_isi_vsend(client, &iov, 1, timeout, cb, opaque, notify);
}


/**
 * Send an ISI request and register a callback to process the response(s) to
 * the resulting transaction.
 *
 * @param cl ISI client (from g_isi_client_create())
 * @param iov scatter-gather array to the request payload
 * @param iovlen number of vectors in the scatter-gather array
 * @param timeout timeout in seconds
 * @param cb callback to process response(s)
 * @param opaque data for the callback
 * @param notify finalizer function for the @a opaque data (may be NULL)
 *
 * @return
 * A pointer to a newly created GIsiRequest.
 *
 * @errors
 * If an error occurs, @a errno is set accordingly and a NULL pointer is
 * returned.
 */
GIsiRequest *g_isi_vsend(GIsiClient *client,
				const struct iovec *__restrict iov,
				size_t iovlen, unsigned timeout,
				GIsiResponseFunc cb, void *opaque,
				GDestroyNotify notify)
{
	struct iovec _iov[1 + iovlen];
	struct sockaddr_pn dst = {
		.spn_family = AF_PHONET,
	};
	struct msghdr msg = {
		.msg_name = (void *)&dst,
		.msg_namelen = sizeof(dst),
		.msg_iov = _iov,
		.msg_iovlen = 1 + iovlen,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	ssize_t ret;
	size_t i, len;
	uint8_t id;

	GIsiRequest *req;
	GIsiRequest **old;

	if (!client) {
		errno = EINVAL;
		return NULL;
	}

	req = g_try_new0(GIsiRequest, 1);
	if (!req) {
		errno = ENOMEM;
		return NULL;
	}

	req->client = client;
	req->id = (client->reqs.last + 1) % 255;
	req->func = cb;
	req->data = opaque;
	req->notify = notify;

	old = tsearch(req, &client->reqs.pending, g_isi_cmp);
	if (!old) {
		errno = ENOMEM;
		goto error;
	}

	if (*old != req) {
		/* FIXME: perhaps retry with randomized access after
		 * initial miss. Although if the rate at which
		 * requests are sent is so high that the transaction
		 * ID wraps it's likely there is something wrong and
		 * we might as well fail here. */
		errno = EBUSY;
		goto error;
	}

	dst.spn_resource = client->resource,

	id = req->id;
	_iov[0].iov_base = &id;
	_iov[0].iov_len = 1;

	for (i = 0, len = 1; i < iovlen; i++) {
		_iov[1 + i] = iov[i];
		len += iov[i].iov_len;
	}

	if (client->debug_func)
		g_isi_vdebug(iov, iovlen, len - 1, client->debug_func,
				client->debug_data);

	ret = sendmsg(client->reqs.fd, &msg, MSG_NOSIGNAL);
	if (ret == -1)
		goto error;

	if (ret != (ssize_t)len) {
		errno = EMSGSIZE;
		goto error;
	}

	req->timeout = g_timeout_add_seconds(timeout, g_isi_timeout, req);
	client->reqs.last = req->id;
	return req;

error:
	tdelete(req, &client->reqs.pending, g_isi_cmp);
	g_free(req);

	return NULL;
}

/**
 * Cancels a pending request, i.e. stop waiting for responses and cancels the
 * timeout.
 * @param req request to cancel
 */
void g_isi_request_cancel(GIsiRequest *req)
{
	if (!req)
		return;

	if (req->timeout > 0)
		g_source_remove(req->timeout);

	tdelete(req, &req->client->reqs.pending, g_isi_cmp);

	if (req->notify)
		req->notify(req->data);

	g_free(req);
}

/**
 * Subscribe to a given indication type for the resource that an ISI client
 * is associated with. If the same type was already subscribed, the old
 * subscription is overriden.
 * @param cl ISI client (fomr g_isi_client_create())
 * @param type indication type
 * @param cb callback to process received indications
 * @param data data for the callback
 * @return 0 on success, a system error code otherwise.
 */
int g_isi_subscribe(GIsiClient *client, uint8_t type,
			GIsiIndicationFunc cb, void *data)
{
	GIsiIndication *ind;
	GIsiIndication **old;
	gboolean isnew = TRUE;

	if (cb == NULL)
		return -EINVAL;

	ind = g_try_new0(GIsiIndication, 1);
	if (!ind)
		return -ENOMEM;

	ind->type = type;

	old = tsearch(ind, &client->inds.subs, g_isi_cmp);
	if (!old) {
		g_free(ind);
		return -ENOMEM;
	}

	/* FIXME: This overrides any existing subscription. We should
	 * enable multiple subscriptions to a single indication in
	 * order to allow efficient client sharing. */
	if (*old != ind) {
		g_free(ind);
		ind = *old;
		isnew = FALSE;
	}

	ind->func = cb;
	ind->data = data;

	if (client->inds.count == 0) {
		int ret = g_isi_indication_init(client);
		if (ret) {
			tdelete(ind, &client->inds.subs, g_isi_cmp);
			g_free(ind);
			return ret;
		}
	}

	if (isnew)
		client->inds.count++;

	return 0;
}

/**
 * Unsubscribe from a given indication type.
 * @param client ISI client (from g_isi_client_create())
 * @param type indication type.
 */
void g_isi_unsubscribe(GIsiClient *client, uint8_t type)
{
	GIsiIndication *ind;
	unsigned int id = type;

	if (!client)
		return;

	ind = tdelete(&id, &client->inds.subs, g_isi_cmp);
	if (!ind)
		return;

	if (--client->inds.count == 0)
		g_isi_indication_deinit(client);

	g_free(ind);
}

static void g_isi_dispatch_indication(GIsiClient *client, uint16_t obj,
					uint8_t *msg, size_t len)
{
	void *ret;
	GIsiIndication *ind;
	unsigned type = msg[0];

	ret = tfind(&type, &client->inds.subs, g_isi_cmp);
	if (!ret)
		return;

	ind = *(GIsiIndication **)ret;

	if (ind->func)
		ind->func(client, msg, len, obj, ind->data);
}

static void g_isi_dispatch_response(GIsiClient *client, uint16_t obj,
					uint8_t *msg, size_t len)
{
	void *ret;
	GIsiRequest *req;
	unsigned id = msg[0];

	ret = tfind(&id, &client->reqs.pending, g_isi_cmp);
	if (!ret) {
		/* This could either be an unsolicited response, which
		 * we will ignore, or an incoming request, which we
		 * handle just like an incoming indication */
		g_isi_dispatch_indication(client, obj, msg + 1, len - 1);
		return;
	}

	req = *(GIsiRequest **)ret;

	if (!req->func || req->func(client, msg + 1, len - 1, obj, req->data))
		g_isi_request_cancel(req);
}

/* Data callback for both responses and indications */
static gboolean g_isi_callback(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GIsiClient *client = data;
	int fd = g_io_channel_unix_get_fd(channel);
	int len;

	if (cond & (G_IO_NVAL|G_IO_HUP)) {
		g_warning("Unexpected event on Phonet channel %p", channel);
		return FALSE;
	}

	len = phonet_peek_length(channel);

	if (len > 0) {
		uint32_t buf[(len + 3) / 4];
		uint8_t *msg;
		uint16_t obj;
		uint8_t res;

		len = phonet_read(channel, buf, len, &obj, &res);
		if (len < 2 || res != client->resource)
			return TRUE;

		msg = (uint8_t *)buf;

		if (client->debug_func)
			client->debug_func(msg + 1, len - 1,
						client->debug_data);

		if (fd == client->reqs.fd)
			g_isi_dispatch_response(client, obj, msg, len);
		else
			/* Transaction field at first byte is
			 * discarded with indications */
			g_isi_dispatch_indication(client, obj, msg + 1,
							len - 1);
	}
	return TRUE;
}

static gboolean g_isi_timeout(gpointer data)
{
	GIsiRequest *req = data;

	req->client->error = ETIMEDOUT;
	if (req->func)
		req->func(req->client, NULL, 0, 0, req->data);
	req->client->error = 0;

	g_isi_request_cancel(req);
	return FALSE;
}

int g_isi_client_error(const GIsiClient *client)
{
	return -client->error;
}
