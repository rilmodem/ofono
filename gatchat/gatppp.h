/*
 *
 *  PPP library with GLib integration
 *
 *  Copyright (C) 2009-2010  Intel Corporation. All rights reserved.
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

#ifndef __G_AT_PPP_H
#define __G_AT_PPP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gat.h"
#include "gathdlc.h"

struct _GAtPPP;

typedef struct _GAtPPP GAtPPP;

typedef enum _GAtPPPDisconnectReason {
	G_AT_PPP_REASON_UNKNOWN,
	G_AT_PPP_REASON_AUTH_FAIL,	/* Failed to authenticate */
	G_AT_PPP_REASON_IPCP_FAIL,	/* Failed to negotiate IPCP */
	G_AT_PPP_REASON_NET_FAIL,	/* Failed to create tun */
	G_AT_PPP_REASON_PEER_CLOSED,	/* Peer initiated a close */
	G_AT_PPP_REASON_LINK_DEAD,	/* Link to the peer died */
	G_AT_PPP_REASON_LOCAL_CLOSE,	/* Normal user close */
} GAtPPPDisconnectReason;

typedef void (*GAtPPPConnectFunc)(const char *iface, const char *local,
					const char *peer,
					const char *dns1, const char *dns2,
					gpointer user_data);
typedef void (*GAtPPPDisconnectFunc)(GAtPPPDisconnectReason reason,
					gpointer user_data);

GAtPPP *g_at_ppp_new(void);
GAtPPP *g_at_ppp_server_new(const char *local);
GAtPPP *g_at_ppp_server_new_full(const char *local, int fd);

gboolean g_at_ppp_open(GAtPPP *ppp, GAtIO *io);
gboolean g_at_ppp_listen(GAtPPP *ppp, GAtIO *io);
void g_at_ppp_set_connect_function(GAtPPP *ppp, GAtPPPConnectFunc callback,
					gpointer user_data);
void g_at_ppp_set_disconnect_function(GAtPPP *ppp, GAtPPPDisconnectFunc func,
					gpointer user_data);
void g_at_ppp_set_suspend_function(GAtPPP *ppp, GAtSuspendFunc func,
					gpointer user_data);
void g_at_ppp_set_debug(GAtPPP *ppp, GAtDebugFunc func, gpointer user_data);
void g_at_ppp_shutdown(GAtPPP *ppp);
void g_at_ppp_suspend(GAtPPP *ppp);
void g_at_ppp_resume(GAtPPP *ppp);
void g_at_ppp_ref(GAtPPP *ppp);
void g_at_ppp_unref(GAtPPP *ppp);

gboolean g_at_ppp_set_credentials(GAtPPP *ppp, const char *username,
						const char *passwd);
const char *g_at_ppp_get_username(GAtPPP *ppp);
const char *g_at_ppp_get_password(GAtPPP *ppp);

void g_at_ppp_set_recording(GAtPPP *ppp, const char *filename);

void g_at_ppp_set_server_info(GAtPPP *ppp, const char *remote_ip,
				const char *dns1, const char *dns2);

void g_at_ppp_set_acfc_enabled(GAtPPP *ppp, gboolean enabled);
void g_at_ppp_set_pfc_enabled(GAtPPP *ppp, gboolean enabled);

#ifdef __cplusplus
}
#endif

#endif /* __G_AT_PPP_H */
