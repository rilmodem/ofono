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
#include <sys/uio.h>

#include "message.h"
#include "modem.h"

struct _GIsiServer;
typedef struct _GIsiServer GIsiServer;

GIsiServer *g_isi_server_create(GIsiModem *modem, uint8_t resource,
				GIsiVersion *version);
uint8_t g_isi_server_resource(GIsiServer *server);
GIsiModem *g_isi_server_modem(GIsiServer *server);
void g_isi_server_destroy(GIsiServer *server);

int g_isi_server_send(GIsiServer *server, const GIsiMessage *req,
			const void *__restrict data, size_t len);

int g_isi_server_vsend(GIsiServer *server, const GIsiMessage *req,
			const struct iovec *iov, size_t iovlen);

GIsiPending *g_isi_server_handle(GIsiServer *server, uint8_t type,
					GIsiNotifyFunc notify, void *data);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_SERVER_H */
