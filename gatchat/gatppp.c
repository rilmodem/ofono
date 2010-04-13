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
#include "crc-ccitt.h"
#include "ppp.h"

#define DEFAULT_MRU	1500

#define BUFFERSZ	(DEFAULT_MRU * 2)

#define PPP_ESC		0x7d
#define PPP_FLAG_SEQ 	0x7e
#define PPP_ADDR_FIELD	0xff
#define PPP_CTRL	0x03

struct _GAtPPP {
	gint ref_count;
	enum ppp_phase phase;
	struct pppcp_data *lcp;
	struct pppcp_data *ipcp;
	struct ppp_net *net;
	struct ppp_chap *chap;
	guint8 buffer[BUFFERSZ];
	int index;
	gint mru;
	char username[256];
	char password[256];
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

#define PPPINITFCS16    0xffff  /* Initial FCS value */
#define PPPGOODFCS16    0xf0b8  /* Good final FCS value */

struct frame_buffer {
	gsize len;
	guint8 bytes[0];
};

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
	fcs = crc_ccitt_byte(fcs, PPP_ADDR_FIELD);
	ppp_put(ppp, frame, &pos, PPP_ADDR_FIELD, lcp);
	fcs = crc_ccitt_byte(fcs, PPP_CTRL);
	ppp_put(ppp, frame, &pos, PPP_CTRL, lcp);

	/*
	 * for each byte, first calculate FCS, then do escaping if
	 * neccessary
	 */
	while (len--) {
		fcs = crc_ccitt_byte(fcs, data[i]);
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
	guint16 protocol = ppp_proto(frame->bytes);
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
		if (ppp->chap) {
			ppp_chap_process_packet(ppp->chap, packet);
			break;
		}
		/* fall through */
	default:
		pppcp_send_protocol_reject(ppp->lcp, frame->bytes, frame->len);
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

	while (frame[pos] != PPP_FLAG_SEQ) {
		/* Skip the characters in receive ACCM */
		if (frame[pos] < 0x20 &&
				(ppp->recv_accm & (1 << frame[pos])) != 0) {
			pos++;
			continue;
		}

		/* scan for escape character */
		if (frame[pos] == PPP_ESC) {
			/* skip that char */
			pos++;
			data[i] = frame[pos] ^ 0x20;
		} else
			data[i] = frame[pos];

		fcs = crc_ccitt_byte(fcs, data[i]);

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

static gboolean ppp_read_cb(GIOChannel *channel, GIOCondition cond,
								gpointer data)
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
		pppcp_signal_close(ppp->lcp);
		break;
	case PPP_PHASE_DEAD:
		ppp_dead(ppp);
		break;
	}
}

static void read_watcher_destroy_notify(GAtPPP *ppp)
{
	ppp->read_watch = 0;
	pppcp_signal_down(ppp->lcp);
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
	/* bring network phase up */
	ppp_net_open(ppp->net);

	if (ppp->connect_cb == NULL)
		return;

	ppp->connect_cb(G_AT_PPP_CONNECT_SUCCESS,
				ppp_net_get_interface(ppp->net),
				ip, dns1, dns2, ppp->connect_data);
}

void ppp_net_down_notify(GAtPPP *ppp)
{
	ppp_net_close(ppp->net);
}

void ppp_set_recv_accm(GAtPPP *ppp, guint32 accm)
{
	ppp->recv_accm = accm;
}

void ppp_set_xmit_accm(GAtPPP *ppp, guint32 accm)
{
	ppp->xmit_accm[0] = accm;
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

	if (ppp->record_fd > fileno(stderr)) {
		close(ppp->record_fd);
		ppp->record_fd = -1;
	}

	if (filename == NULL)
		return;

	ppp->record_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND,
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
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

	if (ppp->record_fd > fileno(stderr))
		close(ppp->record_fd);

	/* cleanup queue */
	g_queue_free(ppp->xmit_queue);

	/* cleanup modem channel */
	g_source_remove(ppp->read_watch);
	g_source_remove(ppp->write_watch);
	g_io_channel_unref(ppp->modem);

	lcp_free(ppp->lcp);
	ppp_chap_free(ppp->chap);
	ipcp_free(ppp->ipcp);
	ppp_net_free(ppp->net);

	g_free(ppp);
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
	ppp->recv_accm = ~0U;
	ppp->xmit_accm[0] = ~0U;
	ppp->xmit_accm[3] = 0x60000000; /* 0x7d, 0x7e */
	ppp->pfc = FALSE;
	ppp->acfc = FALSE;

	ppp->index = 0;

	/* intialize the queue */
	ppp->xmit_queue = g_queue_new();

	/* initialize the lcp state */
	ppp->lcp = lcp_new(ppp);

	/* initialize IPCP state */
	ppp->ipcp = ipcp_new(ppp);

	/* intialize the network state */
	ppp->net = ppp_net_new(ppp);

	/* start listening for packets from the modem */
	ppp->read_watch = g_io_add_watch_full(modem, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				ppp_read_cb, ppp,
				(GDestroyNotify)read_watcher_destroy_notify);

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

	ppp->write_watch = 0;

	if (ppp->phase == PPP_PHASE_DEAD)
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
