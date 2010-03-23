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

#define DEFAULT_MRU	1500
#define BUFFERSZ	DEFAULT_MRU*2
#define DEFAULT_ACCM	0x00000000
#define PPP_ESC		0x7d
#define PPP_FLAG_SEQ 	0x7e
#define PPP_ADDR_FIELD	0xff
#define PPP_CTRL	0x03
#define LCP_PROTOCOL	0xc021
#define CHAP_PROTOCOL	0xc223
#define PPP_HEADROOM	2
#define HDLC_HEADROOM	3
#define HDLC_TAIL	3
#define MD5		5

enum ppp_phase {
	PPP_DEAD = 0,
	PPP_ESTABLISHMENT,
	PPP_AUTHENTICATION,
	PPP_NETWORK,
	PPP_TERMINATION,
};

enum ppp_event {
	PPP_UP = 1,
	PPP_OPENED,
	PPP_SUCCESS,
	PPP_NONE,
	PPP_CLOSING,
	PPP_FAIL,
	PPP_DOWN
};

struct ppp_packet_handler {
	guint16 proto;
	void (*handler)(gpointer priv, guint8 *packet);
	gpointer priv;
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

static inline guint32 __get_unaligned_long(const gpointer p)
{
	const struct packed_long *ptr = p;
	return ptr->l;
}

static inline guint16 __get_unaligned_short(const gpointer p)
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
	struct pppcp_data *ipcp;
};

struct _GAtPPP {
	gint ref_count;
	enum ppp_phase phase;
	struct pppcp_data *lcp;
	struct auth_data *auth;
	struct ppp_net_data *net;
	guint8 buffer[BUFFERSZ];
	int index;
	gint mru;
	guint16 auth_proto;
	char user_name[256];
	char passwd[256];
	gboolean pfc;
	gboolean acfc;
	guint32 xmit_accm[8];
	guint32 recv_accm;
	GIOChannel *modem;
	GQueue *event_queue;
	GQueue *recv_queue;
	GAtPPPConnectFunc connect_cb;
	gpointer connect_data;
	GAtPPPDisconnectFunc disconnect_cb;
	gpointer disconnect_data;
	gint modem_watch;
};

gboolean ppp_cb(GIOChannel *channel, GIOCondition cond, gpointer data);
void ppp_close(GAtPPP *ppp);
void ppp_generate_event(GAtPPP *ppp, enum ppp_event event);
void ppp_register_packet_handler(struct ppp_packet_handler *handler);
void ppp_transmit(GAtPPP *ppp, guint8 *packet, guint infolen);
void ppp_set_auth(GAtPPP *ppp, guint8 *auth_data);
void ppp_set_recv_accm(GAtPPP *ppp, guint32 accm);
guint32 ppp_get_xmit_accm(GAtPPP *ppp);
void ppp_set_pfc(GAtPPP *ppp, gboolean pfc);
gboolean ppp_get_pfc(GAtPPP *ppp);
void ppp_set_acfc(GAtPPP *ppp, gboolean acfc);
gboolean ppp_get_acfc(GAtPPP *ppp);
struct pppcp_data * lcp_new(GAtPPP *ppp);
void lcp_free(struct pppcp_data *lcp);
void lcp_open(struct pppcp_data *data);
void lcp_close(struct pppcp_data *data);
void lcp_establish(struct pppcp_data *data);
void lcp_terminate(struct pppcp_data *data);
void auth_set_credentials(struct auth_data *data, const char *username,
				const char *passwd);
void auth_set_proto(struct auth_data *data, guint16 proto, guint8 method);
struct auth_data *auth_new(GAtPPP *ppp);
void auth_free(struct auth_data *auth);
struct ppp_net_data *ppp_net_new(GAtPPP *ppp);
void ppp_net_open(struct ppp_net_data *data);
void ppp_net_free(struct ppp_net_data *data);
void ppp_net_close(struct ppp_net_data *data);
