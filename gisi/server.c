/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <sys/ioctl.h>
#include <errno.h>
#include "phonet.h"
#include <glib.h>

#include "socket.h"
#include "server.h"

#define PN_NAMESERVICE		0xDB
#define PNS_NAME_ADD_REQ	0x05

struct _GIsiIncoming {
	struct sockaddr_pn spn;
	uint8_t trans_id;
};

struct _GIsiServer {
	GIsiModem *modem;
	uint8_t resource;
	struct {
		int major;
		int minor;
	} version;

	/* Callbacks */
	int fd;
	guint source;
	GIsiRequestFunc func[256];
	void *data[256];

	/* Debugging */
	GIsiDebugFunc debug_func;
	void *debug_data;
};

static gboolean g_isi_server_callback(GIOChannel *channel, GIOCondition cond,
					gpointer data);

/**
 * Create an ISI server.
 * @param resource PhoNet resource ID for the server
 * @return NULL on error (see errno), a GIsiServer pointer on success,
 */
GIsiServer *g_isi_server_create(GIsiModem *modem, uint8_t resource,
				uint8_t major, uint8_t minor)
{
	void *ptr;
	GIsiServer *self;
	GIOChannel *channel;

	if (G_UNLIKELY(posix_memalign(&ptr, 256, sizeof(*self))))
		abort();

	self = ptr;
	memset(self, 0, sizeof(*self));
	self->resource = resource;
	self->version.major = major;
	self->version.minor = minor;
	self->modem = modem;
	self->debug_func = NULL;

	channel = phonet_new(modem, resource);
	if (channel == NULL) {
		free(self);
		return NULL;
	}

	self->fd = g_io_channel_unix_get_fd(channel);
	self->source = g_io_add_watch(channel,
					G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
					g_isi_server_callback, self);
	g_io_channel_unref(channel);
	return self;
}

/**
 * Returns the resource associated with @a server
 * @param server server for the resource
 * @return PhoNet resource ID for the server
 */
uint8_t g_isi_server_resource(GIsiServer *server)
{
	return server->resource;
}

/**
 * Set a debugging function for @a server. This function will be
 * called whenever an ISI protocol message is sent or received.
 * @param server server to debug
 * @param func debug function
 * @param opaque user data
 */
void g_isi_server_set_debug(GIsiServer *server, GIsiDebugFunc func,
				void *opaque)
{
	if (!server)
		return;

	server->debug_func = func;
	server->debug_data = opaque;
}

/**
 * Destroys an ISI server, cancels all pending transactions and subscriptions.
 * @param server server to destroy
 */
void g_isi_server_destroy(GIsiServer *server)
{
	if (!server)
		return;

	g_source_remove(server->source);
	free(server);
}

/**
 * Request the server name from the name server.
 */
void
g_isi_server_add_name(GIsiServer *self)
{
	uint16_t object = 0;

	if (!self)
		return;

	if (ioctl(self->fd, SIOCPNGETOBJECT, &object) < 0) {
		g_warning("%s: %s", "ioctl(SIOCPNGETOBJECT)", strerror(errno));
	} else {
		struct sockaddr_pn spn = {
			.spn_family = PF_PHONET,
			.spn_dev = 0,	/* PN_DEV_HOST */
			.spn_resource = PN_NAMESERVICE,
		};
		uint8_t req[] = {
			0, PNS_NAME_ADD_REQ, 0, 0,
			0, 0, 0, self->resource,	/* name */
			object >> 8, object & 0xff,	/* device/object */
			0, 0,
		};

		if (sendto(self->fd, req, sizeof(req), 0,
				(void *)&spn, sizeof(spn)) != sizeof(req)) {
			g_warning("%s: %s", "sendto(PN_NAMESERVICE)",
				  strerror(errno));
		}
	}
}

/**
 * Make an ISI request and register a callback to process the response(s) to
 * the resulting transaction.
 * @param self ISI server (from g_isi_server_create())
 * @param buf pointer to request payload
 * @param len request payload byte length
 * @param irq information from incoming request
 */
int g_isi_respond(GIsiServer *self, const void *data, size_t len,
			GIsiIncoming *irq)
{
	const struct iovec iov = {
		.iov_base = (void *)data,
		.iov_len = len,
	};

	if (self->debug_func)
		self->debug_func(data, len, self->debug_data);

	return g_isi_vrespond(self, &iov, 1, irq);
}

/**
 * Make an ISI request and register a callback to process the response(s) to
 * the resulting transaction.
 * @param self ISI server (from g_isi_server_create())
 * @param iov scatter-gather array to the request payload
 * @param iovlen number of vectors in the scatter-gather array
 * @param irq information from incoming request
 */
int g_isi_vrespond(GIsiServer *self, const struct iovec *iov, size_t iovlen,
			GIsiIncoming *irq)
{
	struct iovec _iov[1 + iovlen];
	const struct msghdr msg = {
		.msg_name = (void *)&irq->spn,
		.msg_namelen = sizeof(irq->spn),
		.msg_iov = (struct iovec *)_iov,
		.msg_iovlen = 1 + iovlen,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	ssize_t ret;
	size_t i, len;

	if (self == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (irq == NULL) {
		errno = EINVAL;
		return -1;
	}

	_iov[0].iov_base = &irq->trans_id;
	_iov[0].iov_len = 1;
	for (i = 0, len = 1; i < iovlen; i++) {
		_iov[1 + i] = iov[i];
		len += iov[i].iov_len;
	}

	ret = sendmsg(self->fd, &msg, MSG_NOSIGNAL);

	g_free(irq);

	return ret;
}

/**
 * Prepare to handle given request type for the resource that an ISI server
 * is associated with. If the same type was already handled, the old
 * handler is overriden.
 * @param self ISI server (from g_isi_server_create())
 * @param type request message type
 * @param cb callback to process received requests
 * @param data data for the callback
 * @return 0 on success, -1 upon an error.
 */
int g_isi_server_handle(GIsiServer *self, uint8_t type,
			GIsiRequestFunc cb, void *data)
{
	if (self == NULL || cb == NULL) {
		errno = EINVAL;
		return -1;
	}

	self->func[type] = cb;
	self->data[type] = data;
	return 0;
}

/**
 * Remove handler from a given request type.
 * @param server ISI server (from g_isi_server_create())
 * @param type indication type.
 */
void g_isi_server_unhandle(GIsiServer *self, uint8_t type)
{
	if (self)
		self->func[type] = NULL;
}


static void generic_error_response(GIsiServer *self,
			uint8_t trans_id, uint8_t error, uint8_t message_id,
			void *addr, socklen_t addrlen)
{
	uint8_t common[] = { trans_id, 0xF0, error, message_id };

	sendto(self->fd, common, sizeof(common), MSG_NOSIGNAL, addr, addrlen);
}

static void process_message(GIsiServer *self, int len)
{
	uint8_t msg[len + 1];
	struct sockaddr_pn addr;
	socklen_t addrlen = sizeof(addr);
	uint8_t message_id;
	GIsiRequestFunc func;
	void *data;

	len = recvfrom(self->fd, msg, sizeof(msg), MSG_DONTWAIT,
			(void *)&addr, &addrlen);

	if (len < 2 || addr.spn_resource != self->resource)
		return;

	if (self->debug_func)
		self->debug_func(msg + 1, len - 1, self->debug_data);

	message_id = msg[1];
	func = self->func[message_id];
	data = self->data[message_id];

	if (func) {
		GIsiIncoming *irq = g_new0(GIsiIncoming, 1);

		if (irq) {
			irq->spn = addr;
			irq->trans_id = msg[0];
			func(self, msg + 1, len - 1, irq, data);
			return;
		}
	}

	/* Respond with COMMON MESSAGE COMM_SERVICE_NOT_AUTHENTICATED_RESP */
	generic_error_response(self, msg[0], 0x17, msg[1], &addr, addrlen);
}

/* Data callback */
static gboolean g_isi_server_callback(GIOChannel *channel, GIOCondition cond,
					gpointer opaque)
{
	if (cond & (G_IO_NVAL|G_IO_HUP)) {
		g_warning("Unexpected event on Phonet channel %p", channel);
		return FALSE;
	}

	process_message(opaque, phonet_peek_length(channel));

	return TRUE;
}
