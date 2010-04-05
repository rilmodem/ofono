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
#include "gatppp.h"
#include "ppp.h"

#define DEFAULT_MRU	1500
#define DEFAULT_ACCM	0x00000000

#define BUFFERSZ	(DEFAULT_MRU * 2)

#define PPP_ESC		0x7d
#define PPP_FLAG_SEQ 	0x7e
#define PPP_ADDR_FIELD	0xff
#define PPP_CTRL	0x03

enum ppp_phase {
	PPP_DEAD = 0,
	PPP_ESTABLISHMENT,
	PPP_AUTHENTICATION,
	PPP_NETWORK,
	PPP_TERMINATION,
};

struct _GAtPPP {
	gint ref_count;
	enum ppp_phase phase;
	struct pppcp_data *lcp;
	struct auth_data *auth;
	struct pppcp_data *ipcp;
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
	GAtPPPConnectFunc connect_cb;
	gpointer connect_data;
	GAtDisconnectFunc disconnect_cb;
	gpointer disconnect_data;
	gint read_watch;
	gint write_watch;
	GAtDebugFunc debugf;
	gpointer debug_data;
	int record_fd;
	GQueue *xmit_queue;
};

void ppp_debug(GAtPPP *ppp, const char *str)
{
	if (!ppp || !ppp->debugf)
		return;

	ppp->debugf(str, ppp->debug_data);
}

void ppp_connect_cb(GAtPPP *ppp, GAtPPPConnectStatus success,
			const char *ip, const char *dns1, const char *dns2)
{
	if (ppp->connect_cb == NULL)
		return;

	ppp->connect_cb(success, ppp->net->if_name,
				ip, dns1, dns2,  ppp->connect_data);
}

#define PPPINITFCS16    0xffff  /* Initial FCS value */
#define PPPGOODFCS16    0xf0b8  /* Good final FCS value */

struct frame_buffer {
	gsize len;
	guint8 bytes[0];
};

/*
 * FCS lookup table copied from rfc1662.
 */
static guint16 fcstab[256] = {
	0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
	0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
	0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
	0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
	0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
	0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
	0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
	0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
	0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
	0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
	0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
	0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
	0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
	0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
	0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
	0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
	0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
	0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
	0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
	0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
	0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
	0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
	0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
	0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
	0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
	0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
	0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
	0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
	0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
	0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
	0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
	0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

/*
 * Calculate a new fcs given the current fcs and the new data.
 * copied from rfc1662
 *
 *     The FCS field is calculated over all bits of the Address, Control,
 *     Protocol, Information and Padding fields, not including any start
 *     and stop bits (asynchronous) nor any bits (synchronous) or octets
 *     (asynchronous or synchronous) inserted for transparency.  This
 *     also does not include the Flag Sequences nor the FCS field itself.
 */
static guint16 ppp_fcs(guint16 fcs, guint8 c)
{
	guint16 new_fcs;

	new_fcs = (fcs >> 8) ^ fcstab[(fcs ^ c) & 0xff];
	return new_fcs;
}

/*
 * escape any chars less than 0x20, and check the transmit accm table to
 * see if this character should be escaped.
 */
static gboolean ppp_escape(GAtPPP *ppp, guint8 c, gboolean lcp)
{
	if ((lcp && c < 0x20) || (ppp->xmit_accm[c >> 5] & (1 << (c & 0x1f))))
		return TRUE;
	return FALSE;
}

static void ppp_put(GAtPPP *ppp, guint8 *buf, int *pos,
			guint8 c, gboolean lcp)
{
	int i = *pos;

	/* escape characters if needed,  copy into buf, increment pos */
	if (ppp_escape(ppp, c, lcp)) {
		buf[i++] = PPP_ESC;
		buf[i++] = c ^ 0x20;
	} else
		buf[i++] = c;
	*pos = i;
}

/* XXX implement PFC and ACFC */
static struct frame_buffer *ppp_encode(GAtPPP *ppp, guint8 *data, int len)
{
	int pos = 0;
	int i = 0;
	guint16 fcs = PPPINITFCS16;
	guint16 proto = get_host_short(data);
	gboolean lcp = (proto == LCP_PROTOCOL);
	guint8 *frame;
	struct frame_buffer *fb =
		g_try_malloc0(BUFFERSZ + sizeof(struct frame_buffer));

	if (!fb)
		return NULL;
	frame = fb->bytes;

	/* copy in the HDLC framing */
	frame[pos++] = PPP_FLAG_SEQ;

	/* from here till end flag, calculate FCS over each character */
	fcs = ppp_fcs(fcs, PPP_ADDR_FIELD);
	ppp_put(ppp, frame, &pos, PPP_ADDR_FIELD, lcp);
	fcs = ppp_fcs(fcs, PPP_CTRL);
	ppp_put(ppp, frame, &pos, PPP_CTRL, lcp);

	/*
	 * for each byte, first calculate FCS, then do escaping if
	 * neccessary
	 */
	while (len--) {
		fcs = ppp_fcs(fcs, data[i]);
		ppp_put(ppp, frame, &pos, data[i++], lcp);
	}

	/* add FCS */
	fcs ^= 0xffff;		/* complement */
	ppp_put(ppp, frame, &pos, (guint8)(fcs & 0x00ff), lcp);
	ppp_put(ppp, frame, &pos, (guint8)((fcs >> 8) & 0x00ff), lcp);

	/* add flag */
	frame[pos++] = PPP_FLAG_SEQ;

	fb->len = pos;
	return fb;
}

/* called when we have received a complete ppp frame */
static void ppp_recv(GAtPPP *ppp, struct frame_buffer *frame)
{
	guint protocol = ppp_proto(frame->bytes);
	guint8 *packet = ppp_info(frame->bytes);

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
		if (ppp->auth->proto == protocol &&
				ppp->auth->proto_data != NULL) {
			ppp->auth->process_packet(ppp->auth, packet);
			break;
		}
		/* fall through */
	default:
		lcp_protocol_reject(ppp->lcp, frame->bytes, frame->len);
		break;
	};
}

/* XXX - Implement PFC and ACFC */
static struct frame_buffer *ppp_decode(GAtPPP *ppp, guint8 *frame)
{
	guint8 *data;
	guint pos;
	int i;
	guint16 fcs;
	struct frame_buffer *fb;

	fb = g_try_malloc0(sizeof(struct frame_buffer) + ppp->mru + 10);
	if (!fb)
		return NULL;
	data = fb->bytes;

	/* skip the first flag char */
	pos = 1;

	fcs = PPPINITFCS16;
	i = 0;

	/* TBD - how to deal with recv_accm */
	while (frame[pos] != PPP_FLAG_SEQ) {
		/* scan for escape character */
		if (frame[pos] == PPP_ESC) {
			/* skip that char */
			pos++;
			data[i] = frame[pos] ^ 0x20;
		} else
			data[i] = frame[pos];

		fcs = ppp_fcs(fcs, data[i]);

		i++; pos++;
	}

	fb->len = i;

	/* see if we have a good FCS */
	if (fcs != PPPGOODFCS16) {
		g_free(fb);
		return NULL;
	}

	return fb;
}

static void ppp_feed(GAtPPP *ppp, guint8 *data, gsize len)
{
	guint pos = 0;
	struct frame_buffer *frame;

	/* collect bytes until we detect we have received a complete frame */
	/* examine the data.  If we are at the beginning of a new frame,
	 * allocate memory to buffer the frame.
	 */

	for (pos = 0; pos < len; pos++) {
		if (data[pos] == PPP_FLAG_SEQ) {
			if (ppp->index != 0) {
				/* store last flag character & decode */
				ppp->buffer[ppp->index++] = data[pos];
				frame = ppp_decode(ppp, ppp->buffer);
				if (frame) {
					/* process receive frame */
					ppp_recv(ppp, frame);
					g_free(frame);
				}

				/* zero buffer */
				memset(ppp->buffer, 0, BUFFERSZ);
				ppp->index = 0;
				continue;
			}
		}
		/* copy byte to buffer */
		if (ppp->index < BUFFERSZ)
			ppp->buffer[ppp->index++] = data[pos];
	}
}

static void ppp_record(GAtPPP *ppp, gboolean in, guint8 *data, guint16 length)
{
	guint16 len = htons(length);
	guint32 ts;
	struct timeval now;
	unsigned char id;
	int err;

	if (ppp->record_fd < 0)
		return;

	gettimeofday(&now, NULL);
	ts = htonl(now.tv_sec & 0xffffffff);

	id = 0x07;
	err = write(ppp->record_fd, &id, 1);
	err = write(ppp->record_fd, &ts, 4);

	id = in ? 0x02 : 0x01;
	err = write(ppp->record_fd, &id, 1);
	err = write(ppp->record_fd, &len, 2);
	err = write(ppp->record_fd, data, length);
}

static gboolean ppp_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	GAtPPP *ppp = data;
	GIOStatus status;
	gchar buf[256];
	gsize bytes_read;
	GError *error = NULL;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	if (cond & G_IO_IN) {
		status = g_io_channel_read_chars(channel, buf, 256,
				&bytes_read, &error);
		if (bytes_read > 0) {
			ppp_record(ppp, TRUE, (guint8 *) buf, bytes_read);
			ppp_feed(ppp, (guint8 *) buf, bytes_read);
		}
		if (status != G_IO_STATUS_NORMAL && status != G_IO_STATUS_AGAIN)
			return FALSE;
	}

	return TRUE;
}

static void ppp_dead(GAtPPP *ppp)
{
	if (ppp->write_watch)
		return;

	/* notify interested parties */
	if (ppp->disconnect_cb)
		ppp->disconnect_cb(ppp->disconnect_data);

	if (g_atomic_int_get(&ppp->ref_count))
		return;

	/* cleanup queue */
	g_queue_free(ppp->xmit_queue);

	/* cleanup modem channel */
	g_source_remove(ppp->read_watch);
	g_source_remove(ppp->write_watch);
	g_io_channel_unref(ppp->modem);

	lcp_free(ppp->lcp);
	auth_free(ppp->auth);
	ipcp_free(ppp->ipcp);
	ppp_net_free(ppp->net);

	g_free(ppp);
}

static void ppp_transition_phase(GAtPPP *ppp, enum ppp_phase phase)
{
	/* don't do anything if we're already there */
	if (ppp->phase == phase)
		return;

	/* set new phase */
	ppp->phase = phase;

	switch (phase) {
	case PPP_ESTABLISHMENT:
		/* signal UP event to LCP */
		lcp_establish(ppp->lcp);
		break;
	case PPP_AUTHENTICATION:
		/* we don't do authentication right now, so send NONE */
		if (!ppp->auth_proto)
			ppp_generate_event(ppp, PPP_NONE);
		/* otherwise we need to wait for the peer to send us a challenge */
		break;
	case PPP_TERMINATION:
		lcp_terminate(ppp->lcp);
		break;
	case PPP_DEAD:
		ppp_dead(ppp);
		break;
	case PPP_NETWORK:
		/* bring network phase up */
		ppp_net_open(ppp->net);
		pppcp_signal_open(ppp->ipcp);
		break;
	}
}

/*
 * send the event handler a new event to process
 */
void ppp_generate_event(GAtPPP *ppp, enum ppp_event event)
{
	switch (event) {
	case PPP_UP:
		/* causes transition to ppp establishment */
		ppp_transition_phase(ppp, PPP_ESTABLISHMENT);
		break;
	case PPP_OPENED:
		ppp_transition_phase(ppp, PPP_AUTHENTICATION);
		break;
	case PPP_CLOSING:
		/* causes transition to termination phase */
		ppp_transition_phase(ppp, PPP_TERMINATION);
		break;
	case PPP_DOWN:
		/* cases transition to dead phase */
		ppp_transition_phase(ppp, PPP_DEAD);
		break;
	case PPP_NONE:
	case PPP_SUCCESS:
		/* causes transition to network phase */
		ppp_transition_phase(ppp, PPP_NETWORK);
		break;
	case PPP_FAIL:
		if (ppp->phase == PPP_ESTABLISHMENT)
			ppp_transition_phase(ppp, PPP_DEAD);
		else if (ppp->phase == PPP_AUTHENTICATION)
			ppp_transition_phase(ppp, PPP_TERMINATION);
		break;
	}
}

void ppp_set_auth(GAtPPP *ppp, guint8* auth_data)
{
	guint16 proto = get_host_short(auth_data);

	switch (proto) {
	case CHAP_PROTOCOL:
		/* get the algorithm */
		auth_set_proto(ppp->auth, proto, auth_data[2]);
		break;
	default:
		g_printerr("unknown authentication proto\n");
		break;
	}
}

void ppp_set_recv_accm(GAtPPP *ppp, guint32 accm)
{
	ppp->recv_accm = accm;
}

guint32 ppp_get_xmit_accm(GAtPPP *ppp)
{
	return ppp->xmit_accm[0];
}

void ppp_set_pfc(GAtPPP *ppp, gboolean pfc)
{
	ppp->pfc = pfc;
}

gboolean ppp_get_pfc(GAtPPP *ppp)
{
	return ppp->pfc;
}

void ppp_set_acfc(GAtPPP *ppp, gboolean acfc)
{
	ppp->acfc = acfc;
}

gboolean ppp_get_acfc(GAtPPP *ppp)
{
	return ppp->acfc;
}

/* Administrative Open */
void g_at_ppp_open(GAtPPP *ppp)
{
	/* send an OPEN event to the lcp layer */
	lcp_open(ppp->lcp);
}

void g_at_ppp_set_credentials(GAtPPP *ppp, const char *username,
						const char *passwd)
{
	auth_set_credentials(ppp->auth, username, passwd);
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

void g_at_ppp_set_recording(GAtPPP *ppp, const char *filename)
{
	if (ppp == NULL)
		return;

	if (ppp->record_fd > fileno(stderr))
		close(ppp->record_fd);

	if (filename == NULL)
		return;

	ppp->record_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND,
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
}

void g_at_ppp_shutdown(GAtPPP *ppp)
{
	ppp_generate_event(ppp, PPP_CLOSING);
}

void g_at_ppp_ref(GAtPPP *ppp)
{
	g_atomic_int_inc(&ppp->ref_count);
}

void g_at_ppp_unref(GAtPPP *ppp)
{
	if (g_atomic_int_dec_and_test(&ppp->ref_count))
		g_at_ppp_shutdown(ppp);

	/*
	 * we can't free the link yet, because we need to terminate
	 * the link first.
	 */

	if (ppp->record_fd > fileno(stderr))
		close(ppp->record_fd);
}

GAtPPP *g_at_ppp_new(GIOChannel *modem)
{
	GAtPPP *ppp;

	ppp = g_try_malloc0(sizeof(GAtPPP));
	if (!ppp)
		return NULL;

	ppp->modem = g_io_channel_ref(modem);
	if (!g_at_util_setup_io(ppp->modem, G_IO_FLAG_NONBLOCK)) {
		g_io_channel_unref(modem);
		g_free(ppp);
		return NULL;
	}
	g_io_channel_set_buffered(modem, FALSE);

	ppp->ref_count = 1;

	/* set options to defaults */
	ppp->mru = DEFAULT_MRU;
	ppp->recv_accm = DEFAULT_ACCM;
	ppp->xmit_accm[0] = DEFAULT_ACCM;
	ppp->xmit_accm[3] = 0x60000000; /* 0x7d, 0x7e */
	ppp->pfc = FALSE;
	ppp->acfc = FALSE;

	ppp->index = 0;

	/* intialize the queue */
	ppp->xmit_queue = g_queue_new();

	/* initialize the lcp state */
	ppp->lcp = lcp_new(ppp);

	/* initialize the autentication state */
	ppp->auth = auth_new(ppp);

	/* initialize IPCP state */
	ppp->ipcp = ipcp_new(ppp);

	/* intialize the network state */
	ppp->net = ppp_net_new(ppp);

	/* start listening for packets from the modem */
	ppp->read_watch = g_io_add_watch(modem,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			ppp_cb, ppp);

	ppp->record_fd = -1;

	return ppp;
}

static gboolean ppp_xmit_cb(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GAtPPP *ppp = data;
	struct frame_buffer *fb;
	GError *error = NULL;
	GIOStatus status;
	gsize bytes_written;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (cond & G_IO_OUT) {
		while ((fb = g_queue_peek_head(ppp->xmit_queue))) {
			status = g_io_channel_write_chars(ppp->modem,
					(gchar *) fb->bytes, fb->len,
					&bytes_written, &error);
			if (status != G_IO_STATUS_NORMAL &&
				status != G_IO_STATUS_AGAIN)
				return FALSE;

			if (bytes_written < fb->len)
				return TRUE;

			ppp_record(ppp, FALSE, fb->bytes, bytes_written);
			g_free(g_queue_pop_head(ppp->xmit_queue));
		}
	}
	return FALSE;
}

static void ppp_xmit_destroy_notify(gpointer destroy_data)
{
	GAtPPP *ppp = destroy_data;

	g_print("%s\n", __FUNCTION__);

	ppp->write_watch = 0;
	if (ppp->phase == PPP_DEAD)
		ppp_dead(ppp);
}

/*
 * transmit out through the lower layer interface
 *
 * infolen - length of the information part of the packet
 */
void ppp_transmit(GAtPPP *ppp, guint8 *packet, guint infolen)
{
	struct frame_buffer *fb;

	/*
	 * do the octet stuffing.  Add 2 bytes to the infolen to
	 * include the protocol field.
	 */
	fb = ppp_encode(ppp, packet, infolen + 2);
	if (!fb) {
		g_printerr("Failed to encode packet to transmit\n");
		return;
	}
	/* push decoded frame onto xmit queue */
	g_queue_push_tail(ppp->xmit_queue, fb);

	/* transmit this whenever we can write without blocking */
	ppp->write_watch = g_io_add_watch_full(ppp->modem, G_PRIORITY_DEFAULT,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				ppp_xmit_cb, ppp, ppp_xmit_destroy_notify);
}
