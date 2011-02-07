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

#ifndef __GISI_MODEM_H
#define __GISI_MODEM_H

#include <stdint.h>
#include <glib/gtypes.h>

#include "phonet.h"
#include "message.h"

#ifdef __cplusplus
extern "C" {
#endif

enum GIsiModemFlags {
	GISI_MODEM_FLAG_USE_LEGACY_SUBSCRIBE = 1,
};

struct _GIsiModem;
typedef struct _GIsiModem GIsiModem;

struct _GIsiPending;
typedef struct _GIsiPending GIsiPending;

typedef void (*GIsiNotifyFunc)(const GIsiMessage *msg, void *opaque);
typedef void (*GIsiDebugFunc)(const char *fmt, ...);

GIsiModem *g_isi_modem_create(unsigned index);
GIsiModem *g_isi_modem_create_by_name(const char *name);
void g_isi_modem_destroy(GIsiModem *modem);

unsigned g_isi_modem_index(GIsiModem *modem);

uint8_t g_isi_modem_device(GIsiModem *modem);
int g_isi_modem_set_device(GIsiModem *modem, uint8_t dev);

void g_isi_modem_set_trace(GIsiModem *modem, GIsiNotifyFunc notify);
void g_isi_modem_set_debug(GIsiModem *modem, GIsiDebugFunc debug);

void *g_isi_modem_set_userdata(GIsiModem *modem, void *data);
void *g_isi_modem_get_userdata(GIsiModem *modem);

unsigned long g_isi_modem_flags(GIsiModem *modem);
void g_isi_modem_set_flags(GIsiModem *modem, unsigned long flags);

GIsiPending *g_isi_request_send(GIsiModem *modem, uint8_t resource,
					const void *__restrict buf, size_t len,
					unsigned timeout, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy);

GIsiPending *g_isi_request_vsend(GIsiModem *modem, uint8_t resource,
					const struct iovec *__restrict iov,
					size_t iovlen, unsigned timeout,
					GIsiNotifyFunc notify, void *data,
					GDestroyNotify destroy);

GIsiPending *g_isi_request_sendto(GIsiModem *modem, struct sockaddr_pn *dst,
					const void *__restrict buf, size_t len,
					unsigned timeout, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy);

GIsiPending *g_isi_request_vsendto(GIsiModem *modem, struct sockaddr_pn *dst,
					const struct iovec *__restrict iov,
					size_t iovlen, unsigned timeout,
					GIsiNotifyFunc notify, void *data,
					GDestroyNotify destroy);

int g_isi_modem_send(GIsiModem *modem, uint8_t resource,
			const void *__restrict buf, size_t len);

int g_isi_modem_vsend(GIsiModem *modem, uint8_t resource,
				const struct iovec *__restrict iov,
				size_t iovlen);

int g_isi_modem_sendto(GIsiModem *modem, struct sockaddr_pn *dst,
			const void *__restrict buf, size_t len);

int g_isi_modem_vsendto(GIsiModem *modem, struct sockaddr_pn *dst,
				const struct iovec *__restrict iov,
				size_t iovlen);

uint8_t g_isi_request_utid(GIsiPending *resp);

GIsiPending *g_isi_ind_subscribe(GIsiModem *modem, uint8_t resource,
					uint8_t type, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy);

GIsiPending *g_isi_ntf_subscribe(GIsiModem *modem, uint8_t resource,
					uint8_t type, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy);

GIsiPending *g_isi_service_bind(GIsiModem *modem, uint8_t resource,
				uint8_t type, GIsiNotifyFunc notify,
				void *data, GDestroyNotify destroy);

int g_isi_response_send(GIsiModem *modem, const GIsiMessage *req,
			const void *__restrict buf, size_t len);

int g_isi_response_vsend(GIsiModem *modem, const GIsiMessage *req,
				const struct iovec *__restrict iov,
				size_t iovlen);

void g_isi_pending_remove(GIsiPending *op);
void g_isi_pending_set_owner(GIsiPending *op, gpointer owner);
void g_isi_remove_pending_by_owner(GIsiModem *modem, uint8_t resource,
					gpointer owner);

GIsiPending *g_isi_resource_ping(GIsiModem *modem, uint8_t resource,
					GIsiNotifyFunc notify, void *data,
					GDestroyNotify destroy);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_MODEM_H */
