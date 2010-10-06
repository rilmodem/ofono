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

#ifndef __GISI_CLIENT_H
#define __GISI_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <glib/gtypes.h>
#include <gisi/modem.h>
#include "phonet.h"

struct _GIsiClient;
typedef struct _GIsiClient GIsiClient;

struct _GIsiRequest;
typedef struct _GIsiRequest GIsiRequest;

typedef void (*GIsiVerifyFunc)(GIsiClient *client, gboolean alive,
				uint16_t object, void *opaque);

typedef gboolean (*GIsiResponseFunc)(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque);

typedef void (*GIsiIndicationFunc) (GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque);

GIsiClient *g_isi_client_create(GIsiModem *modem, uint8_t resource);

GIsiRequest *g_isi_verify(GIsiClient *client, GIsiVerifyFunc func,
				void *opaque);

GIsiRequest *g_isi_verify_resource(GIsiClient *client, uint8_t resource,
					GIsiVerifyFunc func, void *opaque);

uint8_t g_isi_client_resource(GIsiClient *client);

void g_isi_version_set(GIsiClient *client, int major, int minor);
int g_isi_version_major(GIsiClient *client);
int g_isi_version_minor(GIsiClient *client);

void g_isi_server_object_set(GIsiClient *client, uint16_t obj);
uint8_t g_isi_server_object(GIsiClient *client);

void g_isi_client_set_debug(GIsiClient *client, GIsiDebugFunc func,
				void *opaque);

void g_isi_client_destroy(GIsiClient *client);

int g_isi_client_error(const GIsiClient *client);

GIsiRequest *g_isi_request_make(GIsiClient *client, const void *data,
				size_t len, unsigned timeout,
				GIsiResponseFunc func, void *opaque);
struct iovec;
GIsiRequest *g_isi_request_vmake(GIsiClient *client, const struct iovec *iov,
					size_t iovlen, unsigned timeout,
					GIsiResponseFunc func, void *opaque);

GIsiRequest *g_isi_sendto(GIsiClient *client,
				struct sockaddr_pn *dst,
				const void *data, size_t len,
				unsigned timeout,
				GIsiResponseFunc func, void *opaque,
				GDestroyNotify notify);

GIsiRequest *g_isi_send(GIsiClient *client, const void *data, size_t len,
			unsigned timeout,
			GIsiResponseFunc func, void *opaque,
			GDestroyNotify notify);

GIsiRequest *g_isi_vsendto(GIsiClient *client,
				struct sockaddr_pn *dst,
				const struct iovec *iov, size_t iovlen,
				unsigned timeout,
				GIsiResponseFunc func, void *opaque,
				GDestroyNotify notify);

GIsiRequest *g_isi_vsend(GIsiClient *client,
				const struct iovec *iov, size_t iovlen,
				unsigned timeout,
				GIsiResponseFunc func, void *opaque,
				GDestroyNotify notify);

void g_isi_request_cancel(GIsiRequest *req);

int g_isi_commit_subscriptions(GIsiClient *client);
int g_isi_add_subscription(GIsiClient *client, uint8_t res, uint8_t type,
				GIsiIndicationFunc cb, void *data);
void g_isi_remove_subscription(GIsiClient *client, uint8_t res, uint8_t type);

int g_isi_subscribe(GIsiClient *client, uint8_t type,
			GIsiIndicationFunc func, void *opaque);
void g_isi_unsubscribe(GIsiClient *client, uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_CLIENT_H */
