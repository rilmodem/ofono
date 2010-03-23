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

struct _GAtPPP;
typedef struct _GAtPPP GAtPPP;

typedef enum _GAtPPPConnectStatus {
	G_AT_PPP_CONNECT_SUCCESS,
	G_AT_PPP_CONNECT_FAIL
} GAtPPPConnectStatus;

typedef void (*GAtPPPConnectFunc)(GAtPPP *ppp, GAtPPPConnectStatus success,
				 guint32 ip_address,
				 guint32 dns1, guint32 dns2,
				 gpointer user_data);

typedef void (*GAtPPPDisconnectFunc)(GAtPPP *ppp, gpointer user_data);

GAtPPP * g_at_ppp_new(GIOChannel *modem);
void g_at_ppp_open(GAtPPP *ppp);
void g_at_ppp_set_connect_function(GAtPPP *ppp,
			       GAtPPPConnectFunc callback, gpointer user_data);
void g_at_ppp_set_disconnect_function(GAtPPP *ppp,
				  GAtPPPDisconnectFunc callback,
				  gpointer user_data);
void g_at_ppp_shutdown(GAtPPP *ppp);
void g_at_ppp_ref(GAtPPP *ppp);
void g_at_ppp_unref(GAtPPP *ppp);
void g_at_ppp_set_credentials(GAtPPP *ppp, const char *username,
				const char *passwd);
#ifdef __cplusplus
}
#endif

#endif /* __G_AT_PPP_H */
