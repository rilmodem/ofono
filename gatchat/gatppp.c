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
	GAtDisconnectFunc disconnect_cb;
	gpointer disconnect_data;
	GAtDebugFunc debugf;
	gpointer debug_data;
};

void ppp_debug(GAtPPP *ppp, const char *str)
{
	if (!ppp || !ppp->debugf)
		return;

	ppp->debugf(str, ppp->debug_data);
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
		if (ppp->net == NULL)
			return TRUE;
		break;
	}

	return FALSE;
}

static void ppp_receive(const unsigned char *buf, gsize len, void *data)
{
	GAtPPP *ppp = data;
	guint16 protocol = ppp_proto(buf);
	const guint8 *packet = ppp_info(buf);

	if (ppp_drop_packet(ppp, protocol))
		return;

	switch (protocol) {
	case PPP_IP_PROTO:
		ppp_net_process_packet(ppp->net, packet);
		break;
	case LCP_PROTOCOL:
		pppcp_process_packet(ppp->lcp, packet);
		break;
	case IPCP_PROTO:
		pppcp_process_packet(ppp->ipcp, packet);
		break;
	case CHAP_PROTOCOL:
		if (ppp->chap) {
			ppp_chap_process_packet(ppp->chap, packet);
			break;
		}
		/* fall through */
	default:
		pppcp_send_protocol_reject(ppp->lcp, buf, len);
		break;
	};
}

/*
 * transmit out through the lower layer interface
 *
 * infolen - length of the information part of the packet
 */
void ppp_transmit(GAtPPP *ppp, guint8 *packet, guint infolen)
{
	guint16 proto = get_host_short(packet);
	guint8 code;
	gboolean lcp = (proto == LCP_PROTOCOL);
	guint32 xmit_accm = 0;

	/*
	 * all LCP Link Configuration, Link Termination, and Code-Reject
	 * packets must be sent with the default sending ACCM
	 */
	if (lcp) {
		code = pppcp_get_code(packet);
		lcp = code > 0 && code < 8;
	}

	if (lcp) {
		xmit_accm = g_at_hdlc_get_xmit_accm(ppp->hdlc);
		g_at_hdlc_set_xmit_accm(ppp->hdlc, ~0U);
	}

	if (g_at_hdlc_send(ppp->hdlc, packet, infolen + 2) == FALSE)
		g_print("Failed to send a frame\n");

	if (lcp)
		g_at_hdlc_set_xmit_accm(ppp->hdlc, xmit_accm);
}

static void ppp_dead(GAtPPP *ppp)
{
	/* notify interested parties */
	if (ppp->disconnect_cb)
		ppp->disconnect_cb(ppp->disconnect_data);
}

void ppp_enter_phase(GAtPPP *ppp, enum ppp_phase phase)
{
	/* don't do anything if we're already there */
	if (ppp->phase == phase)
		return;

	/* set new phase */
	ppp->phase = phase;

	g_print("Entering new phase: %d\n", phase);

	switch (phase) {
	case PPP_PHASE_ESTABLISHMENT:
		/* send an UP & OPEN events to the lcp layer */
		pppcp_signal_up(ppp->lcp);
		pppcp_signal_open(ppp->lcp);
		break;
	case PPP_PHASE_AUTHENTICATION:
		/* If we don't expect auth, move on to network phase */
		if (ppp->chap == NULL)
			ppp_enter_phase(ppp, PPP_PHASE_NETWORK);

		/* otherwise wait for the peer to send us a challenge */
		break;
	case PPP_PHASE_NETWORK:
		/* Send UP & OPEN events to the IPCP layer */
		pppcp_signal_open(ppp->ipcp);
		pppcp_signal_up(ppp->ipcp);
		break;
	case PPP_PHASE_TERMINATION:
		pppcp_signal_down(ppp->ipcp);
		pppcp_signal_close(ppp->ipcp);
		break;
	case PPP_PHASE_DEAD:
		ppp_dead(ppp);
		break;
	}
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
		g_printerr("unknown authentication proto\n");
		break;
	}
}

void ppp_auth_notify(GAtPPP *ppp, gboolean success)
{
	if (success)
		ppp_enter_phase(ppp, PPP_PHASE_NETWORK);
	else
		ppp_enter_phase(ppp, PPP_PHASE_TERMINATION);
}

void ppp_net_up_notify(GAtPPP *ppp, const char *ip,
					const char *dns1, const char *dns2)
{
	ppp->net = ppp_net_new(ppp);

	if (ppp_net_set_mtu(ppp->net, ppp->mtu) == FALSE)
		g_printerr("Unable to set MTU\n");

	if (ppp->connect_cb == NULL)
		return;

	if (ppp->net == NULL)
		ppp->connect_cb(G_AT_PPP_CONNECT_FAIL, NULL,
					NULL, NULL, NULL, ppp->connect_data);
	else
		ppp->connect_cb(G_AT_PPP_CONNECT_SUCCESS,
					ppp_net_get_interface(ppp->net),
					ip, dns1, dns2, ppp->connect_data);
}

void ppp_net_down_notify(GAtPPP *ppp)
{
	/* Most likely we failed to create the interface */
	if (ppp->net == NULL)
		return;

	ppp_net_free(ppp->net);
	ppp->net = NULL;
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

static void io_disconnect(gpointer user_data)
{
	GAtPPP *ppp = user_data;

	pppcp_signal_down(ppp->lcp);
	ppp_enter_phase(ppp, PPP_PHASE_DEAD);
}

/* Administrative Open */
void g_at_ppp_open(GAtPPP *ppp)
{
	ppp_enter_phase(ppp, PPP_PHASE_ESTABLISHMENT);
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

void g_at_ppp_set_disconnect_function(GAtPPP *ppp, GAtDisconnectFunc func,
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

void g_at_ppp_shutdown(GAtPPP *ppp)
{
	pppcp_signal_close(ppp->lcp);
}

void g_at_ppp_ref(GAtPPP *ppp)
{
	g_atomic_int_inc(&ppp->ref_count);
}

void g_at_ppp_unref(GAtPPP *ppp)
{
	gboolean is_zero;

	is_zero = g_atomic_int_dec_and_test(&ppp->ref_count);

	if (is_zero == FALSE)
		return;

	if (ppp->net)
		ppp_net_free(ppp->net);

	if (ppp->chap)
		ppp_chap_free(ppp->chap);

	lcp_free(ppp->lcp);
	ipcp_free(ppp->ipcp);

	g_free(ppp);
}

GAtPPP *g_at_ppp_new(GIOChannel *modem)
{
	GAtPPP *ppp;

	ppp = g_try_malloc0(sizeof(GAtPPP));
	if (!ppp)
		return NULL;

	ppp->hdlc = g_at_hdlc_new(modem);

	if (ppp->hdlc == NULL) {
		g_free(ppp);
		return NULL;
	}

	ppp->ref_count = 1;

	/* set options to defaults */
	ppp->mru = DEFAULT_MRU;
	ppp->mtu = DEFAULT_MTU;

	/* initialize the lcp state */
	ppp->lcp = lcp_new(ppp);

	/* initialize IPCP state */
	ppp->ipcp = ipcp_new(ppp);

	g_at_hdlc_set_receive(ppp->hdlc, ppp_receive, ppp);
	g_at_io_set_disconnect_function(g_at_hdlc_get_io(ppp->hdlc),
						io_disconnect, ppp);

	return ppp;
}
