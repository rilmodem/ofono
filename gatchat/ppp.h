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
#define MD5		5

#define DBG(p, fmt, arg...) do {				\
	char *str = g_strdup_printf("%s:%s() " fmt, __FILE__,	\
					__FUNCTION__ , ## arg); \
	ppp_debug(p, str);					\
	g_free(str);						\
} while (0)

struct ppp_chap;
struct ppp_net;

struct ppp_header {
	guint8 address;
	guint8 control;
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

static inline void __put_unaligned_short(void *p, guint16 val)
{
	struct packed_short *ptr = p;
	ptr->s = val;
}

#define get_host_long(p) \
	(ntohl(__get_unaligned_long(p)))

#define get_host_short(p) \
	(ntohs(__get_unaligned_short(p)))

#define put_network_short(p, val) \
	(__put_unaligned_short(p, htons(val)))

#define ppp_proto(packet) \
	(get_host_short(packet + 2))

/* LCP related functions */
struct pppcp_data *lcp_new(GAtPPP *ppp, gboolean dormant);
void lcp_free(struct pppcp_data *lcp);
void lcp_protocol_reject(struct pppcp_data *lcp, guint8 *packet, gsize len);
void lcp_set_acfc_enabled(struct pppcp_data *pppcp, gboolean enabled);
void lcp_set_pfc_enabled(struct pppcp_data *pppcp, gboolean enabled);

/* IPCP related functions */
struct pppcp_data *ipcp_new(GAtPPP *ppp, gboolean is_server, guint32 ip);
void ipcp_free(struct pppcp_data *data);
void ipcp_set_server_info(struct pppcp_data *ipcp, guint32 peer_addr,
				guint32 dns1, guint32 dns2);

/* CHAP related functions */
struct ppp_chap *ppp_chap_new(GAtPPP *ppp, guint8 method);
void ppp_chap_free(struct ppp_chap *chap);
void ppp_chap_process_packet(struct ppp_chap *chap, const guint8 *new_packet,
				gsize len);

/* TUN / Network related functions */
struct ppp_net *ppp_net_new(GAtPPP *ppp, int fd);
const char *ppp_net_get_interface(struct ppp_net *net);
void ppp_net_process_packet(struct ppp_net *net, const guint8 *packet,
				gsize len);
void ppp_net_free(struct ppp_net *net);
gboolean ppp_net_set_mtu(struct ppp_net *net, guint16 mtu);
void ppp_net_suspend_interface(struct ppp_net *net);
void ppp_net_resume_interface(struct ppp_net *net);

/* PPP functions related to main GAtPPP object */
void ppp_debug(GAtPPP *ppp, const char *str);
void ppp_transmit(GAtPPP *ppp, guint8 *packet, guint infolen);
void ppp_set_auth(GAtPPP *ppp, const guint8 *auth_data);
void ppp_auth_notify(GAtPPP *ppp, gboolean success);
void ppp_ipcp_up_notify(GAtPPP *ppp, const char *local, const char *peer,
					const char *dns1, const char *dns2);
void ppp_ipcp_down_notify(GAtPPP *ppp);
void ppp_ipcp_finished_notify(GAtPPP *ppp);
void ppp_lcp_up_notify(GAtPPP *ppp);
void ppp_lcp_down_notify(GAtPPP *ppp);
void ppp_lcp_finished_notify(GAtPPP *ppp);
void ppp_set_recv_accm(GAtPPP *ppp, guint32 accm);
void ppp_set_xmit_accm(GAtPPP *ppp, guint32 accm);
void ppp_set_mtu(GAtPPP *ppp, const guint8 *data);
void ppp_set_xmit_acfc(GAtPPP *ppp, gboolean acfc);
void ppp_set_xmit_pfc(GAtPPP *ppp, gboolean pfc);
struct ppp_header *ppp_packet_new(gsize infolen, guint16 protocol);
