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

#ifndef __GISI_SERVER_H
#define __GISI_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <gisi/modem.h>

struct _GIsiServer;
typedef struct _GIsiServer GIsiServer;

struct _GIsiIncoming;
typedef struct _GIsiIncoming GIsiIncoming;

typedef gboolean (*GIsiRequestFunc)(GIsiServer *server,
					const void *restrict data, size_t len,
					GIsiIncoming *, void *opaque);

GIsiServer *g_isi_server_create(GIsiModem *modem, uint8_t resource,
				uint8_t major, uint8_t minor);

uint8_t g_isi_server_resource(GIsiServer *server);

void g_isi_server_set_debug(GIsiServer *server, GIsiDebugFunc func,
				void *opaque);

void g_isi_server_destroy(GIsiServer *server);

void g_isi_server_add_name(GIsiServer *self);

int g_isi_respond(GIsiServer *server, const void *data, size_t len,
			GIsiIncoming *irq);

struct iovec;

int g_isi_vrespond(GIsiServer *server, const struct iovec *iov,
			size_t iovlen, GIsiIncoming *irq);

int g_isi_server_handle(GIsiServer *server, uint8_t type,
			GIsiRequestFunc func, void *opaque);

void g_isi_server_unhandle(GIsiServer *server, uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_SERVER_H */
