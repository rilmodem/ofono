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

#include "ppp_cp.h"

#define LCP_PROTOCOL	0xc021
#define CHAP_PROTOCOL	0xc223
#define IPCP_PROTO	0x8021
#define PPP_IP_PROTO	0x0021

enum ppp_event {
	PPP_UP = 1,
	PPP_OPENED,
	PPP_SUCCESS,
	PPP_NONE,
	PPP_CLOSING,
	PPP_FAIL,
	PPP_DOWN
};

struct ppp_header {
	guint16 proto;
	guint8 info[0];
} __attribute__((packed));

struct packed_short {
	guint16 s;
} __attribute__((packed));

struct packed_long {
	guint32 l;
} __attribute__((packed));

static inline guint32 __get_unaligned_long(const void *p)
{
	const struct packed_long *ptr = p;
	return ptr->l;
}

static inline guint16 __get_unaligned_short(const void *p)
{
	const struct packed_short *ptr = p;
	return ptr->s;
}

#define get_host_long(p) \
	(ntohl(__get_unaligned_long(p)))

#define get_host_short(p) \
	(ntohs(__get_unaligned_short(p)))

#define ppp_info(packet) \
	(packet + 4)

#define ppp_proto(packet) \
	(get_host_short(packet + 2))

struct auth_data {
	guint16 proto;
	gpointer proto_data;
	void (*process_packet)(struct auth_data *data, guint8 *packet);
	char *username;
	char *password;
	GAtPPP *ppp;
};

struct ppp_net_data {
	GAtPPP *ppp;
	char *if_name;
	GIOChannel *channel;
	gint watch;
};

void ppp_debug(GAtPPP *ppp, const char *str);
void ppp_generate_event(GAtPPP *ppp, enum ppp_event event);
void ppp_transmit(GAtPPP *ppp, guint8 *packet, guint infolen);
void ppp_set_auth(GAtPPP *ppp, const guint8 *auth_data);
void ppp_set_recv_accm(GAtPPP *ppp, guint32 accm);
void ppp_set_xmit_accm(GAtPPP *ppp, guint32 accm);
void ppp_set_pfc(GAtPPP *ppp, gboolean pfc);
gboolean ppp_get_pfc(GAtPPP *ppp);
void ppp_set_acfc(GAtPPP *ppp, gboolean acfc);
gboolean ppp_get_acfc(GAtPPP *ppp);
struct pppcp_data * lcp_new(GAtPPP *ppp);
void lcp_free(struct pppcp_data *lcp);
void lcp_protocol_reject(struct pppcp_data *lcp, guint8 *packet, gsize len);
void auth_process_packet(struct auth_data *data, guint8 *new_packet);
void auth_set_credentials(struct auth_data *data, const char *username,
				const char *passwd);
void auth_set_proto(struct auth_data *data, guint16 proto, guint8 method);
struct auth_data *auth_new(GAtPPP *ppp);
void auth_free(struct auth_data *auth);
struct ppp_net_data *ppp_net_new(GAtPPP *ppp);
void ppp_net_open(struct ppp_net_data *data);
void ppp_net_process_packet(struct ppp_net_data *data, guint8 *packet);
void ppp_net_close(struct ppp_net_data *data);
void ppp_net_free(struct ppp_net_data *data);
struct pppcp_data *ipcp_new(GAtPPP *ppp);
void ipcp_free(struct pppcp_data *data);
void ppp_connect_cb(GAtPPP *ppp, GAtPPPConnectStatus success,
			const char *ip, const char *dns1, const char *dns2);
