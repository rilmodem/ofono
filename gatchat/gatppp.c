/*
 *
 *  PPP library with GLib integration
 *
 *  Copyright (C) 2009-2011  Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include <glib.h>

#include "gatutil.h"
#include "gathdlc.h"
#include "gatppp.h"
#include "crc-ccitt.h"
#include "ppp.h"

#define DEFAULT_MRU	1500
#define DEFAULT_MTU	1500

#define PPP_ADDR_FIELD	0xff
#define PPP_CTRL	0x03

#define GUARD_TIMEOUTS 1500

enum ppp_phase {
	PPP_PHASE_DEAD = 0,		/* Link dead */
	PPP_PHASE_ESTABLISHMENT,	/* LCP started */
	PPP_PHASE_AUTHENTICATION,	/* Auth started */
	PPP_PHASE_NETWORK,		/* IPCP started */
	PPP_PHASE_LINK_UP,		/* IPCP negotiation ok, link up */
	PPP_PHASE_TERMINATION,		/* LCP Terminate phase */
};

struct _GAtPPP {
	gint ref_count;
	enum ppp_phase phase;
	struct pppcp_data *lcp;
	struct pppcp_data *ipcp;
	struct ppp_net *net;
	struct ppp_chap *chap;
	GAtHDLC *hdlc;
	gint mru;
	gint mtu;
	char username[256];
	char password[256];
	GAtPPPConnectFunc connect_cb;
	gpointer connect_data;
	GAtPPPDisconnectFunc disconnect_cb;
	gpointer disconnect_data;
	GAtPPPDisconnectReason disconnect_reason;
	GAtDebugFunc debugf;
	gpointer debug_data;
	gboolean sta_pending;
	guint ppp_dead_source;
	GAtSuspendFunc suspend_func;
	gpointer suspend_data;
	int fd;
	guint guard_timeout_source;
	gboolean suspended;
	gboolean xmit_acfc;
	gboolean xmit_pfc;
};

void ppp_debug(GAtPPP *ppp, const char *str)
{
	if (ppp == NULL || ppp->debugf == NULL)
		return;

	ppp->debugf(str, ppp->debug_data);
}

static gboolean ppp_dead(gpointer userdata)
{
	GAtPPP *ppp = userdata;

	DBG(ppp, "");

	ppp->ppp_dead_source = 0;

	/* notify interested parties */
	if (ppp->disconnect_cb)
		ppp->disconnect_cb(ppp->disconnect_reason,
					ppp->disconnect_data);

	return FALSE;
}

static void sta_sent(gpointer userdata)
{
	GAtPPP *ppp = userdata;

	DBG(ppp, "");

	ppp->sta_pending = FALSE;

	if (ppp->phase == PPP_PHASE_DEAD)
		ppp_dead(ppp);
}

struct ppp_header *ppp_packet_new(gsize infolen, guint16 protocol)
{
	struct ppp_header *ppp_packet;

	ppp_packet = g_try_malloc0(infolen + sizeof(*ppp_packet));
	if (ppp_packet == NULL)
		return NULL;

	ppp_packet->proto = htons(protocol);
	ppp_packet->address = PPP_ADDR_FIELD;
	ppp_packet->control = PPP_CTRL;

	return ppp_packet;
}

/*
 * Silently discard packets which are received when they shouldn't be
 */
static inline gboolean ppp_drop_packet(GAtPPP *ppp, guint16 protocol)
{
	switch (ppp->phase) {
	case PPP_PHASE_ESTABLISHMENT:
	case PPP_PHASE_TERMINATION:
		if (protocol != LCP_PROTOCOL)
			return TRUE;
		break;
	case PPP_PHASE_AUTHENTICATION:
		if (protocol != LCP_PROTOCOL && protocol != CHAP_PROTOCOL)
			return TRUE;
		break;
	case PPP_PHASE_DEAD:
		return TRUE;
	case PPP_PHASE_NETWORK:
		if (protocol != LCP_PROTOCOL && protocol != CHAP_PROTOCOL &&
					protocol != IPCP_PROTO)
			return TRUE;
		break;
	case PPP_PHASE_LINK_UP:
		break;
	}

	return FALSE;
}

static void ppp_receive(const unsigned char *buf, gsize len, void *data)
{
	GAtPPP *ppp = data;
	unsigned int offset = 0;
	guint16 protocol;
	const guint8 *packet;

	if (len == 0)
		return;

	if (buf[0] == PPP_ADDR_FIELD && len >= 2 && buf[1] == PPP_CTRL)
		offset = 2;

	if (len < offset + 1)
		return;

	/* From RFC 1661:
	 * the Protocol field uses an extension mechanism consistent with the
	 * ISO 3309 extension mechanism for the Address field; the Least
	 * Significant Bit (LSB) of each octet is used to indicate extension
	 * of the Protocol field.  A binary "0" as the LSB indicates that the
	 * Protocol field continues with the following octet.  The presence
	 * of a binary "1" as the LSB marks the last octet of the Protocol
	 * field.
	 *
	 * To check for compression we simply check the LSB of the first
	 * protocol byte.
	 */

	if (buf[offset] & 0x1) {
		protocol = buf[offset];
		offset += 1;
	} else {
		if (len < offset + 2)
			return;

		protocol = get_host_short(buf + offset);
		offset += 2;
	}

	if (ppp_drop_packet(ppp, protocol))
		return;

	packet = buf + offset;

	switch (protocol) {
	case PPP_IP_PROTO:
		ppp_net_process_packet(ppp->net, packet, len - offset);
		break;
	case LCP_PROTOCOL:
		pppcp_process_packet(ppp->lcp, packet, len - offset);
		break;
	case IPCP_PROTO:
		pppcp_process_packet(ppp->ipcp, packet, len - offset);
		break;
	case CHAP_PROTOCOL:
		if (ppp->chap) {
			ppp_chap_process_packet(ppp->chap, packet,
							len - offset);
			break;
		}
		/* fall through */
	default:
		pppcp_send_protocol_reject(ppp->lcp, buf, len);
		break;
	};
}

static void ppp_send_lcp_frame(GAtPPP *ppp, guint8 *packet, guint infolen)
{
	struct ppp_header *header = (struct ppp_header *) packet;
	guint8 code;
	guint32 xmit_accm = 0;
	gboolean sta = FALSE;
	gboolean lcp;

	/*
	 * all LCP Link Configuration, Link Termination, and Code-Reject
	 * packets must be sent with the default sending ACCM
	 */
	code = pppcp_get_code(packet);
	lcp = code > 0 && code < 8;

	/*
	 * If we're going down, we try to make sure to send the final
	 * ack before informing the upper layers via the ppp_disconnect
	 * function.  Once we enter PPP_DEAD phase, no further packets
	 * will be sent
	 */
	if (code == PPPCP_CODE_TYPE_TERMINATE_ACK)
		sta = TRUE;

	if (lcp) {
		xmit_accm = g_at_hdlc_get_xmit_accm(ppp->hdlc);
		g_at_hdlc_set_xmit_accm(ppp->hdlc, ~0U);
	}

	header->address = PPP_ADDR_FIELD;
	header->control = PPP_CTRL;

	if (g_at_hdlc_send(ppp->hdlc, packet,
			infolen + sizeof(*header)) == TRUE) {
		if (sta) {
			GAtIO *io = g_at_hdlc_get_io(ppp->hdlc);

			ppp->sta_pending = TRUE;
			g_at_io_set_write_done(io, sta_sent, ppp);
		}
	} else
		DBG(ppp, "Failed to send a frame\n");

	if (lcp)
		g_at_hdlc_set_xmit_accm(ppp->hdlc, xmit_accm);
}

static void ppp_send_acfc_frame(GAtPPP *ppp, guint8 *packet,
					guint infolen)
{
	struct ppp_header *header = (struct ppp_header *) packet;
	guint offset = 0;

	if (ppp->xmit_acfc)
		offset = 2;

	/* We remove the only address and control field */
	if (g_at_hdlc_send(ppp->hdlc, packet + offset,
				infolen + sizeof(*header) - offset)
			== FALSE)
		DBG(ppp, "Failed to send a frame\n");
}

static void ppp_send_acfc_pfc_frame(GAtPPP *ppp, guint8 *packet,
					guint infolen)
{
	struct ppp_header *header = (struct ppp_header *) packet;
	guint offset = 0;

	if (ppp->xmit_acfc && ppp->xmit_pfc)
		offset = 3;
	else if (ppp->xmit_acfc)
		offset = 2;
	else if (ppp->xmit_pfc) {
		/* Shuffle AC bytes in place of the first protocol byte */
		packet[2] = packet[1];
		packet[1] = packet[0];
		offset = 1;
	}

	if (g_at_hdlc_send(ppp->hdlc, packet + offset,
				infolen + sizeof(*header) - offset)
			== FALSE)
		DBG(ppp, "Failed to send a frame\n");
}

/*
 * transmit out through the lower layer interface
 *
 * infolen - length of the information part of the packet
 */
void ppp_transmit(GAtPPP *ppp, guint8 *packet, guint infolen)
{
	guint16 proto = ppp_proto(packet);

	if (proto == LCP_PROTOCOL) {
		ppp_send_lcp_frame(ppp, packet, infolen);
		return;
	}

	/*
	 * If the upper 8 bits of the protocol are 0, then send
	 * with PFC if enabled
	 */
	if ((proto & 0xff00) == 0)
		ppp_send_acfc_pfc_frame(ppp, packet, infolen);
	else
		ppp_send_acfc_frame(ppp, packet, infolen);
}

static inline void ppp_enter_phase(GAtPPP *ppp, enum ppp_phase phase)
{
	DBG(ppp, "%d", phase);
	ppp->phase = phase;

	if (phase == PPP_PHASE_DEAD && ppp->sta_pending == FALSE)
		ppp->ppp_dead_source = g_idle_add(ppp_dead, ppp);
}

void ppp_set_auth(GAtPPP *ppp, const guint8* auth_data)
{
	guint16 proto = get_host_short(auth_data);

	switch (proto) {
	case CHAP_PROTOCOL:
		if (ppp->chap)
			ppp_chap_free(ppp->chap);

		ppp->chap = ppp_chap_new(ppp, auth_data[2]);
		break;
	default:
		DBG(ppp, "unknown authentication proto");
		break;
	}
}

void ppp_auth_notify(GAtPPP *ppp, gboolean success)
{
	if (success == FALSE) {
		ppp->disconnect_reason = G_AT_PPP_REASON_AUTH_FAIL;
		pppcp_signal_close(ppp->lcp);
		return;
	}

	ppp_enter_phase(ppp, PPP_PHASE_NETWORK);

	/* Send UP & OPEN events to the IPCP layer */
	pppcp_signal_open(ppp->ipcp);
	pppcp_signal_up(ppp->ipcp);
}

void ppp_ipcp_up_notify(GAtPPP *ppp, const char *local, const char *peer,
					const char *dns1, const char *dns2)
{
	ppp->net = ppp_net_new(ppp, ppp->fd);

	/*
	 * ppp_net_new took control over the fd, whatever happens is out of
	 * our hands now
	 */
	ppp->fd = -1;

	if (ppp->net == NULL) {
		ppp->disconnect_reason = G_AT_PPP_REASON_NET_FAIL;
		pppcp_signal_close(ppp->lcp);
		return;
	}

	if (ppp_net_set_mtu(ppp->net, ppp->mtu) == FALSE)
		DBG(ppp, "Unable to set MTU");

	ppp_enter_phase(ppp, PPP_PHASE_LINK_UP);

	if (ppp->connect_cb)
		ppp->connect_cb(ppp_net_get_interface(ppp->net),
					local, peer, dns1, dns2,
					ppp->connect_data);
}

void ppp_ipcp_down_notify(GAtPPP *ppp)
{
	/* Most likely we failed to create the interface */
	if (ppp->net == NULL)
		return;

	ppp_net_free(ppp->net);
	ppp->net = NULL;
}

void ppp_ipcp_finished_notify(GAtPPP *ppp)
{
	if (ppp->phase != PPP_PHASE_NETWORK)
		return;

	/* Our IPCP parameter negotiation failed */
	ppp->disconnect_reason = G_AT_PPP_REASON_IPCP_FAIL;
	pppcp_signal_close(ppp->ipcp);
	pppcp_signal_close(ppp->lcp);
}

void ppp_lcp_up_notify(GAtPPP *ppp)
{
	/* Wait for the peer to send us a challenge if we expect auth */
	if (ppp->chap != NULL) {
		ppp_enter_phase(ppp, PPP_PHASE_AUTHENTICATION);
		return;
	}

	/* Otherwise proceed as if auth succeeded */
	ppp_auth_notify(ppp, TRUE);
}

void ppp_lcp_down_notify(GAtPPP *ppp)
{
	if (ppp->phase == PPP_PHASE_NETWORK || ppp->phase == PPP_PHASE_LINK_UP)
		pppcp_signal_down(ppp->ipcp);

	if (ppp->disconnect_reason == G_AT_PPP_REASON_UNKNOWN)
		ppp->disconnect_reason = G_AT_PPP_REASON_PEER_CLOSED;

	ppp_enter_phase(ppp, PPP_PHASE_TERMINATION);
}

void ppp_lcp_finished_notify(GAtPPP *ppp)
{
	ppp_enter_phase(ppp, PPP_PHASE_DEAD);
}

void ppp_set_recv_accm(GAtPPP *ppp, guint32 accm)
{
	g_at_hdlc_set_recv_accm(ppp->hdlc, accm);
}

void ppp_set_xmit_accm(GAtPPP *ppp, guint32 accm)
{
	g_at_hdlc_set_xmit_accm(ppp->hdlc, accm);
}

/*
 * The only time we use other than default MTU is when we are in
 * the network phase.
 */
void ppp_set_mtu(GAtPPP *ppp, const guint8 *data)
{
	guint16 mtu = get_host_short(data);

	ppp->mtu = mtu;
}

void ppp_set_xmit_acfc(GAtPPP *ppp, gboolean acfc)
{
	ppp->xmit_acfc = acfc;
}

void ppp_set_xmit_pfc(GAtPPP *ppp, gboolean pfc)
{
	ppp->xmit_pfc = pfc;
}

static void io_disconnect(gpointer user_data)
{
	GAtPPP *ppp = user_data;

	if (ppp->phase == PPP_PHASE_DEAD)
		return;

	ppp->disconnect_reason = G_AT_PPP_REASON_LINK_DEAD;
	pppcp_signal_down(ppp->lcp);
	pppcp_signal_close(ppp->lcp);
}

static void ppp_proxy_suspend_net_interface(gpointer user_data)
{
	GAtPPP *ppp = user_data;

	ppp->suspended = TRUE;
	ppp_net_suspend_interface(ppp->net);

	if (ppp->suspend_func)
		ppp->suspend_func(ppp->suspend_data);
}

gboolean g_at_ppp_listen(GAtPPP *ppp, GAtIO *io)
{
	ppp->hdlc = g_at_hdlc_new_from_io(io);
	if (ppp->hdlc == NULL)
		return FALSE;

	ppp->suspended = FALSE;
	g_at_hdlc_set_receive(ppp->hdlc, ppp_receive, ppp);
	g_at_hdlc_set_suspend_function(ppp->hdlc,
					ppp_proxy_suspend_net_interface, ppp);
	g_at_io_set_disconnect_function(io, io_disconnect, ppp);

	ppp_enter_phase(ppp, PPP_PHASE_ESTABLISHMENT);

	return TRUE;
}

/* Administrative Open */
gboolean g_at_ppp_open(GAtPPP *ppp, GAtIO *io)
{
	ppp->hdlc = g_at_hdlc_new_from_io(io);
	if (ppp->hdlc == NULL)
		return FALSE;

	ppp->suspended = FALSE;
	g_at_hdlc_set_receive(ppp->hdlc, ppp_receive, ppp);
	g_at_hdlc_set_suspend_function(ppp->hdlc,
					ppp_proxy_suspend_net_interface, ppp);
	g_at_hdlc_set_no_carrier_detect(ppp->hdlc, TRUE);
	g_at_io_set_disconnect_function(io, io_disconnect, ppp);

	/* send an UP & OPEN events to the lcp layer */
	pppcp_signal_up(ppp->lcp);
	pppcp_signal_open(ppp->lcp);

	ppp_enter_phase(ppp, PPP_PHASE_ESTABLISHMENT);

	return TRUE;
}

gboolean g_at_ppp_set_credentials(GAtPPP *ppp, const char *username,
					const char *password)
{
	if (username && strlen(username) > 255)
		return FALSE;

	if (password && strlen(password) > 255)
		return FALSE;

	memset(ppp->username, 0, sizeof(ppp->username));
	memset(ppp->password, 0, sizeof(ppp->password));

	if (username)
		strcpy(ppp->username, username);

	if (password)
		strcpy(ppp->password, password);

	return TRUE;
}

const char *g_at_ppp_get_username(GAtPPP *ppp)
{
	return ppp->username;
}

const char *g_at_ppp_get_password(GAtPPP *ppp)
{
	return ppp->password;
}

void g_at_ppp_set_recording(GAtPPP *ppp, const char *filename)
{
	if (ppp == NULL)
		return;

	g_at_hdlc_set_recording(ppp->hdlc, filename);
}

void g_at_ppp_set_connect_function(GAtPPP *ppp, GAtPPPConnectFunc func,
							gpointer user_data)
{
	if (func == NULL)
		return;

	ppp->connect_cb = func;
	ppp->connect_data = user_data;
}

void g_at_ppp_set_disconnect_function(GAtPPP *ppp, GAtPPPDisconnectFunc func,
							gpointer user_data)
{
	if (func == NULL)
		return;

	ppp->disconnect_cb = func;
	ppp->disconnect_data = user_data;
}

void g_at_ppp_set_debug(GAtPPP *ppp, GAtDebugFunc func, gpointer user_data)
{
	if (ppp == NULL)
		return;

	ppp->debugf = func;
	ppp->debug_data = user_data;
}

void g_at_ppp_set_suspend_function(GAtPPP *ppp, GAtSuspendFunc func,
					gpointer user_data)
{
	if (ppp == NULL)
		return;

	ppp->suspend_func = func;
	ppp->suspend_data = user_data;

	if (ppp->hdlc != NULL)
		g_at_hdlc_set_suspend_function(ppp->hdlc,
					ppp_proxy_suspend_net_interface, ppp);
}

void g_at_ppp_shutdown(GAtPPP *ppp)
{
	if (ppp->phase == PPP_PHASE_DEAD || ppp->phase == PPP_PHASE_TERMINATION)
		return;

	ppp->disconnect_reason = G_AT_PPP_REASON_LOCAL_CLOSE;
	pppcp_signal_close(ppp->lcp);
}

static gboolean call_suspend_cb(gpointer user_data)
{
	GAtPPP *ppp = user_data;

	ppp->guard_timeout_source = 0;

	if (ppp->suspend_func)
		ppp->suspend_func(ppp->suspend_data);

	return FALSE;
}

static gboolean send_escape_sequence(gpointer user_data)
{
	GAtPPP *ppp = user_data;
	GAtIO *io = g_at_hdlc_get_io(ppp->hdlc);

	g_at_io_write(io, "+++", 3);
	ppp->guard_timeout_source  = g_timeout_add(GUARD_TIMEOUTS,
						call_suspend_cb, ppp);

	return FALSE;
}

void g_at_ppp_suspend(GAtPPP *ppp)
{
	if (ppp == NULL)
		return;

	ppp->suspended = TRUE;
	ppp_net_suspend_interface(ppp->net);
	g_at_hdlc_suspend(ppp->hdlc);
	ppp->guard_timeout_source = g_timeout_add(GUARD_TIMEOUTS,
						send_escape_sequence, ppp);
}

void g_at_ppp_resume(GAtPPP *ppp)
{
	if (ppp == NULL)
		return;

	if (g_at_hdlc_get_io(ppp->hdlc) == NULL) {
		io_disconnect(ppp);
		return;
	}

	ppp->suspended = FALSE;
	g_at_io_set_disconnect_function(g_at_hdlc_get_io(ppp->hdlc),
							io_disconnect, ppp);
	ppp_net_resume_interface(ppp->net);
	g_at_hdlc_resume(ppp->hdlc);
}

void g_at_ppp_ref(GAtPPP *ppp)
{
	g_atomic_int_inc(&ppp->ref_count);
}

void g_at_ppp_unref(GAtPPP *ppp)
{
	gboolean is_zero;

	if (ppp == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&ppp->ref_count);

	if (is_zero == FALSE)
		return;

	if (ppp->suspended == FALSE)
		g_at_io_set_disconnect_function(g_at_hdlc_get_io(ppp->hdlc),
							NULL, NULL);

	if (ppp->net)
		ppp_net_free(ppp->net);
	else if (ppp->fd >= 0)
		close(ppp->fd);

	if (ppp->chap)
		ppp_chap_free(ppp->chap);

	lcp_free(ppp->lcp);
	ipcp_free(ppp->ipcp);

	if (ppp->ppp_dead_source) {
		g_source_remove(ppp->ppp_dead_source);
		ppp->ppp_dead_source = 0;
	}

	if (ppp->guard_timeout_source) {
		g_source_remove(ppp->guard_timeout_source);
		ppp->guard_timeout_source = 0;
	}

	g_at_hdlc_unref(ppp->hdlc);

	g_free(ppp);
}

void g_at_ppp_set_server_info(GAtPPP *ppp, const char *remote,
				const char *dns1, const char *dns2)
{
	guint32 r = 0;
	guint32 d1 = 0;
	guint32 d2 = 0;

	inet_pton(AF_INET, remote, &r);
	inet_pton(AF_INET, dns1, &d1);
	inet_pton(AF_INET, dns2, &d2);

	ipcp_set_server_info(ppp->ipcp, r, d1, d2);
}

void g_at_ppp_set_acfc_enabled(GAtPPP *ppp, gboolean enabled)
{
	lcp_set_acfc_enabled(ppp->lcp, enabled);
}

void g_at_ppp_set_pfc_enabled(GAtPPP *ppp, gboolean enabled)
{
	lcp_set_pfc_enabled(ppp->lcp, enabled);
}

static GAtPPP *ppp_init_common(gboolean is_server, guint32 ip)
{
	GAtPPP *ppp;

	ppp = g_try_malloc0(sizeof(GAtPPP));
	if (ppp == NULL)
		return NULL;

	ppp->ref_count = 1;
	ppp->suspended = TRUE;
	ppp->fd = -1;

	/* set options to defaults */
	ppp->mru = DEFAULT_MRU;
	ppp->mtu = DEFAULT_MTU;

	/* initialize the lcp state */
	ppp->lcp = lcp_new(ppp, is_server);

	/* initialize IPCP state */
	ppp->ipcp = ipcp_new(ppp, is_server, ip);

	return ppp;
}

GAtPPP *g_at_ppp_new(void)
{
	return ppp_init_common(FALSE, 0);
}

GAtPPP *g_at_ppp_server_new_full(const char *local, int fd)
{
	GAtPPP *ppp;
	guint32 ip;

	if (local == NULL)
		ip = 0;
	else if (inet_pton(AF_INET, local, &ip) != 1)
		return NULL;

	ppp = ppp_init_common(TRUE, ip);

	if (ppp != NULL)
		ppp->fd = fd;

	return ppp;
}

GAtPPP *g_at_ppp_server_new(const char *local)
{
	return g_at_ppp_server_new_full(local, -1);
}
