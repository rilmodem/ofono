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

#ifndef __GISI_CLIENT_H
#define __GISI_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "modem.h"

#define G_ISI_CLIENT_DEFAULT_TIMEOUT	(5)

struct _GIsiClient;
typedef struct _GIsiClient GIsiClient;

GIsiClient *g_isi_client_create(GIsiModem *modem, uint8_t resource);
GIsiModem *g_isi_client_modem(GIsiClient *client);
uint8_t g_isi_client_resource(GIsiClient *client);
void g_isi_client_reset(GIsiClient *client);
void g_isi_client_destroy(GIsiClient *client);

void g_isi_client_set_timeout(GIsiClient *client, unsigned timeout);

gboolean g_isi_client_send(GIsiClient *client,
			const void *__restrict msg, size_t len,
			GIsiNotifyFunc notify, void *data,
			GDestroyNotify destroy);

gboolean g_isi_client_vsend(GIsiClient *client,
			const struct iovec *iov, size_t iovlen,
			GIsiNotifyFunc notify, void *data,
			GDestroyNotify destroy);

gboolean g_isi_client_send_with_timeout(GIsiClient *client,
				const void *__restrict msg,
				size_t len, unsigned timeout,
				GIsiNotifyFunc notify, void *data,
				GDestroyNotify destroy);

gboolean g_isi_client_vsend_with_timeout(GIsiClient *client,
				const struct iovec *iov,
				size_t iovlen, unsigned timeout,
				GIsiNotifyFunc notify, void *data,
				GDestroyNotify destroy);

gboolean g_isi_client_ind_subscribe(GIsiClient *client, uint8_t type,
					GIsiNotifyFunc notify, void *data);
gboolean g_isi_client_ntf_subscribe(GIsiClient *client, uint8_t type,
					GIsiNotifyFunc notify, void *data);

gboolean g_isi_client_verify(GIsiClient *client, GIsiNotifyFunc notify,
				void *data, GDestroyNotify destroy);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_CLIENT_H */
