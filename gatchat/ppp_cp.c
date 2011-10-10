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
#include <glib.h>
#include <arpa/inet.h>

#include "gatppp.h"
#include "ppp.h"

static const char *pppcp_state_strings[] = {
	"INITIAL", "STARTING", "CLOSED", "STOPPED", "CLOSING", "STOPPING",
	"REQSENT", "ACKRCVD", "ACKSENT", "OPENED"
};

static const char *pppcp_event_strings[] = {
	"Up", "Down", "Open", "Close", "TO+", "TO-", "RCR+", "RCR-",
	"RCA", "RCN", "RTR", "RTA", "RUC", "RXJ+", "RXJ-", "RXR"
};

#define pppcp_trace(p) do { \
	char *str = g_strdup_printf("%s: %s: current state %d:%s", \
				p->driver->name, __FUNCTION__, \
				p->state, pppcp_state_strings[p->state]); \
	ppp_debug(p->ppp, str); \
	g_free(str); \
} while (0);

#define pppcp_trace_event(p, type, actions, state) do { \
	char *str = g_strdup_printf("event: %d (%s), " \
				"action: %x, new_state: %d (%s)", \
				type, pppcp_event_strings[type], \
				actions, state, pppcp_state_strings[state]); \
	ppp_debug(p->ppp, str); \
	g_free(str); \
} while (0);

#define pppcp_to_ppp_packet(p) \
	(((guint8 *) p) - sizeof(struct ppp_header))

#define INITIAL_RESTART_TIMEOUT	3	/* restart interval in seconds */
#define MAX_TERMINATE		2
#define MAX_CONFIGURE		10
#define MAX_FAILURE		5
#define CP_HEADER_SZ		4

enum pppcp_state {
	INITIAL		= 0,
	STARTING	= 1,
	CLOSED		= 2,
	STOPPED		= 3,
	CLOSING		= 4,
	STOPPING	= 5,
	REQSENT		= 6,
	ACKRCVD		= 7,
	ACKSENT		= 8,
	OPENED		= 9,
};

enum actions {
	INV = 0x10,
	IRC = 0x20,
	ZRC = 0x40,
	TLU = 0x100,
	TLD = 0x200,
	TLS = 0x400,
	TLF = 0x800,
	SCR = 0x1000,
	SCA = 0x2000,
	SCN = 0x4000,
	STR = 0x8000,
	STA = 0x10000,
	SCJ = 0x20000,
	SER = 0x40000,
};

/*
 * Transition table straight from RFC 1661 Section 4.1
 * Y coordinate is the events, while X coordinate is the state
 *
 * Magic of bitwise operations allows the table to describe all state
 * transitions defined in the specification
 */
static int cp_transitions[16][10] = {
/* Up */
{ 2, IRC|SCR|6, INV, INV, INV, INV, INV, INV, INV, INV },
/* Down */
{ INV, INV, 0, TLS|1, 0, 1, 1, 1, 1, TLD|1 },
/* Open */
{ TLS|1, 1, IRC|SCR|6, 3, 5, 5, 6, 7, 8, 9 },
/* Close */
{ 0, TLF|0, 2, 2, 4, 4, IRC|STR|4, IRC|STR|4, IRC|STR|4, TLD|IRC|STR|4 },
/* TO+ */
{ INV, INV, INV, INV, STR|4, STR|5, SCR|6, SCR|6, SCR|8, INV },
/* TO- */
{ INV, INV, INV, INV, TLF|2, TLF|3, TLF|3, TLF|3, TLF|3, INV },
/* RCR+ */
{ INV, INV, STA|2, IRC|SCR|SCA|8, 4, 5, SCA|8, SCA|TLU|9, SCA|8, TLD|SCR|SCA|8 },
/* RCR- */
{ INV, INV, STA|2, IRC|SCR|SCN|6, 4, 5, SCN|6, SCN|7, SCN|6, TLD|SCR|SCN|6 },
/* RCA */
{ INV, INV, STA|2, STA|3, 4, 5, IRC|7, SCR|6, IRC|TLU|9, TLD|SCR|6 },
/* RCN */
{ INV, INV, STA|2, STA|3, 4, 5, IRC|SCR|6, SCR|6, IRC|SCR|8, TLD|SCR|6 },
/* RTR */
{ INV, INV, STA|2, STA|3, STA|4, STA|5, STA|6, STA|6, STA|6, TLD|ZRC|STA|5 },
/* RTA */
{ INV, INV, 2, 3, TLF|2, TLF|3, 6, 6, 8, TLD|SCR|6 },
/* RUC */
{ INV, INV, SCJ|2, SCJ|3, SCJ|4, SCJ|5, SCJ|6, SCJ|7, SCJ|8, SCJ|9 },
/* RXJ+ */
{ INV, INV, 2, 3, 4, 5, 6, 6, 8, 9 },
/* RXJ- */
{ INV, INV, TLF|2, TLF|3, TLF|2, TLF|3, TLF|3, TLF|3, TLF|3, TLD|IRC|STR|5 },
/* RXR */
{ INV, INV, 2, 3, 4, 5, 6, 7, 8, SER|9 },
};

enum pppcp_event_type {
	UP		= 0,
	DOWN		= 1,
	OPEN		= 2,
	CLOSE		= 3,
	TO_PLUS		= 4,
	TO_MINUS	= 5,
	RCR_PLUS	= 6,
	RCR_MINUS	= 7,
	RCA		= 8,
	RCN		= 9,
	RTR		= 10,
	RTA		= 11,
	RUC		= 12,
	RXJ_PLUS	= 13,
	RXJ_MINUS	= 14,
	RXR		= 15,
};

struct pppcp_timer_data {
	struct pppcp_data *data;
	guint restart_counter;
	guint restart_interval;
	guint max_counter;
	guint restart_timer;
};

struct pppcp_data {
	unsigned char state;
	struct pppcp_timer_data config_timer_data;
	struct pppcp_timer_data terminate_timer_data;
	guint max_failure;
	guint failure_counter;
	GAtPPP *ppp;
	guint8 config_identifier;
	guint8 terminate_identifier;
	guint8 reject_identifier;
	const guint8 *local_options;
	guint16 local_options_len;
	guint8 *peer_options;
	guint16 peer_options_len;
	gboolean send_reject;
	const struct pppcp_proto *driver;
	gpointer priv;
};

static void pppcp_generate_event(struct pppcp_data *data,
				enum pppcp_event_type event_type,
				const guint8 *packet, guint len);

static void pppcp_packet_free(struct pppcp_packet *packet)
{
	g_free(pppcp_to_ppp_packet(packet));
}

static struct pppcp_packet *pppcp_packet_new(struct pppcp_data *data,
						guint type, guint bufferlen)
{
	struct pppcp_packet *packet;
	struct ppp_header *ppp_packet;
	guint16 packet_length = bufferlen + sizeof(*packet);

	ppp_packet = ppp_packet_new(packet_length, data->driver->proto);
	if (ppp_packet == NULL)
		return NULL;

	/* advance past protocol to add CP header information */
	packet = (struct pppcp_packet *) (ppp_packet->info);

	packet->length = htons(packet_length);
	packet->code = type;
	return packet;
}

void ppp_option_iter_init(struct ppp_option_iter *iter,
					const struct pppcp_packet *packet)
{
	iter->max = ntohs(packet->length) - CP_HEADER_SZ;
	iter->pdata = packet->data;
	iter->pos = 0;
	iter->type = 0;
	iter->len = 0;
	iter->option_data = NULL;
}

gboolean ppp_option_iter_next(struct ppp_option_iter *iter)
{
	const guint8 *cur = iter->pdata + iter->pos;
	const guint8 *end = iter->pdata + iter->max;

	if (cur + 1 > end)
		return FALSE;

	if (cur + cur[1] > end)
		return FALSE;

	iter->type = cur[0];
	iter->len = cur[1] - 2;
	iter->option_data = cur + 2;

	iter->pos += cur[1];

	return TRUE;
}

guint8 ppp_option_iter_get_type(struct ppp_option_iter *iter)
{
	return iter->type;
}

guint8 ppp_option_iter_get_length(struct ppp_option_iter *iter)
{
	return iter->len;
}

const guint8 *ppp_option_iter_get_data(struct ppp_option_iter *iter)
{
	return iter->option_data;
}

guint8 pppcp_get_code(const guint8 *data)
{
	struct ppp_header *ppp_packet = (struct ppp_header *) data;
	struct pppcp_packet *packet = (struct pppcp_packet *) ppp_packet->info;

	return packet->code;
}

static gboolean pppcp_timeout(gpointer user_data)
{
	struct pppcp_timer_data *timer_data = user_data;

	pppcp_trace(timer_data->data);

	timer_data->restart_timer = 0;

	if (timer_data->restart_counter > 0)
		pppcp_generate_event(timer_data->data, TO_PLUS, NULL, 0);
	else
		pppcp_generate_event(timer_data->data, TO_MINUS, NULL, 0);

	return FALSE;
}

static void pppcp_stop_timer(struct pppcp_timer_data *timer_data)
{
	if (timer_data->restart_timer > 0) {
		g_source_remove(timer_data->restart_timer);
		timer_data->restart_timer = 0;
	}
}

static void pppcp_start_timer(struct pppcp_timer_data *timer_data)
{
	pppcp_stop_timer(timer_data);

	timer_data->restart_timer =
		g_timeout_add_seconds(timer_data->restart_interval,
				pppcp_timeout, timer_data);
}

static gboolean is_first_request(struct pppcp_timer_data *timer_data)
{
	return (timer_data->restart_counter == timer_data->max_counter);
}

/* actions */
/* log an illegal event, but otherwise do nothing */
static void pppcp_illegal_event(GAtPPP *ppp, guint8 state, guint8 type)
{
	DBG(ppp, "Illegal event %d while in state %d", type, state);
}

static void pppcp_this_layer_up(struct pppcp_data *data)
{
	if (data->driver->this_layer_up)
		data->driver->this_layer_up(data);
}

static void pppcp_this_layer_down(struct pppcp_data *data)
{
	if (data->driver->this_layer_down)
		data->driver->this_layer_down(data);
}

static void pppcp_this_layer_started(struct pppcp_data *data)
{
	if (data->driver->this_layer_started)
		data->driver->this_layer_started(data);
}

static void pppcp_this_layer_finished(struct pppcp_data *data)
{
	pppcp_trace(data);
	if (data->driver->this_layer_finished)
		data->driver->this_layer_finished(data);
}

/*
 * set the restart counter to either max-terminate
 * or max-configure.  The counter is decremented for
 * each transmission, including the first.
 */
static void pppcp_initialize_restart_count(struct pppcp_timer_data *timer_data)
{
	struct pppcp_data *data = timer_data->data;

	pppcp_trace(data);

	timer_data->restart_counter = timer_data->max_counter;
}

/*
 * set restart counter to zero
 */
static void pppcp_zero_restart_count(struct pppcp_timer_data *timer_data)
{
	timer_data->restart_counter = 0;
}

/*
 * TBD - generate new identifier for packet
 */
static guint8 new_identity(struct pppcp_data *data, guint prev_identifier)
{
	return prev_identifier + 1;
}

/*
 * transmit a Configure-Request packet
 * start the restart timer
 * decrement the restart counter
 */
static void pppcp_send_configure_request(struct pppcp_data *pppcp)
{
	struct pppcp_packet *packet;
	struct pppcp_timer_data *timer_data = &pppcp->config_timer_data;

	pppcp_trace(pppcp);

	packet = pppcp_packet_new(pppcp, PPPCP_CODE_TYPE_CONFIGURE_REQUEST,
					pppcp->local_options_len);
	memcpy(packet->data, pppcp->local_options, pppcp->local_options_len);

	/*
	 * if this is the first request, we need a new identifier.
	 * if this is a retransmission, leave the identifier alone.
	 */
	if (is_first_request(timer_data))
		pppcp->config_identifier =
			new_identity(pppcp, pppcp->config_identifier);
	packet->identifier = pppcp->config_identifier;

	ppp_transmit(pppcp->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);

	/* start timer for retransmission */
	timer_data->restart_counter--;
	pppcp_start_timer(timer_data);
}

/*
 * transmit a Configure-Ack packet
 */
static void pppcp_send_configure_ack(struct pppcp_data *pppcp,
					const guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *cr_req = (struct pppcp_packet *) request;
	guint16 len;

	pppcp_trace(pppcp);

	pppcp->failure_counter = 0;

	/* subtract for header. */
	len = ntohs(cr_req->length) - CP_HEADER_SZ;

	packet = pppcp_packet_new(pppcp, PPPCP_CODE_TYPE_CONFIGURE_ACK, len);

	memcpy(packet->data, cr_req->data, len);
	packet->identifier = cr_req->identifier;
	ppp_transmit(pppcp->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));
	pppcp_packet_free(packet);
}

/*
 * transmit a Configure-Nak or Configure-Reject packet
 */
static void pppcp_send_configure_nak(struct pppcp_data *pppcp,
					const guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *cr_req = (struct pppcp_packet *) request;

	pppcp_trace(pppcp);

	/*
	 * if we have exceeded our Max-Failure counter, we simply reject all
	 * the options.
	 */
	if (pppcp->failure_counter >= pppcp->max_failure) {
		guint16 len = ntohs(cr_req->length) - CP_HEADER_SZ;

		packet = pppcp_packet_new(pppcp,
					PPPCP_CODE_TYPE_CONFIGURE_REJECT, len);
		memcpy(packet->data, cr_req->data, len);
	} else {
		enum pppcp_code code = PPPCP_CODE_TYPE_CONFIGURE_NAK;

		if (pppcp->send_reject == TRUE)
			code = PPPCP_CODE_TYPE_CONFIGURE_REJECT;
		else
			pppcp->failure_counter++;

		packet = pppcp_packet_new(pppcp, code, pppcp->peer_options_len);
		memcpy(packet->data, pppcp->peer_options,
						pppcp->peer_options_len);
	}

	packet->identifier = cr_req->identifier;
	ppp_transmit(pppcp->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);

	g_free(pppcp->peer_options);
	pppcp->peer_options = NULL;
	pppcp->peer_options_len = 0;
}

/*
 * transmit a Terminate-Request packet.
 * start the restart timer.
 * decrement the restart counter
 */
static void pppcp_send_terminate_request(struct pppcp_data *data)
{
	struct pppcp_packet *packet;
	struct pppcp_timer_data *timer_data = &data->terminate_timer_data;

	pppcp_trace(data);

	/*
	 * the data field can be used by the sender (us).
	 * leave this empty for now.
	 */
	packet = pppcp_packet_new(data, PPPCP_CODE_TYPE_TERMINATE_REQUEST, 0);

	/*
	 * Is this a retransmission?  If so, do not change
	 * the identifier.  If not, we need a fresh identity.
	 */
	if (is_first_request(timer_data))
		data->terminate_identifier =
			new_identity(data, data->terminate_identifier);
	packet->identifier = data->terminate_identifier;
	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);
	timer_data->restart_counter--;
	pppcp_start_timer(timer_data);
}

/*
 * transmit a Terminate-Ack packet
 */
static void pppcp_send_terminate_ack(struct pppcp_data *data,
					const guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *pppcp_header = (struct pppcp_packet *) request;
	struct pppcp_timer_data *timer_data = &data->terminate_timer_data;

	pppcp_trace(data);

	packet = pppcp_packet_new(data, PPPCP_CODE_TYPE_TERMINATE_ACK, 0);

	/* match identifier of the request */
	packet->identifier = pppcp_header->identifier;

	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(pppcp_header->length));

	pppcp_packet_free(packet);
	pppcp_start_timer(timer_data);
}

/*
 * transmit a Code-Reject packet
 *
 * XXX this seg faults.
 */
static void pppcp_send_code_reject(struct pppcp_data *data,
					const guint8 *rejected_packet)
{
	struct pppcp_packet *packet;
	const struct pppcp_packet *old_packet =
				(const struct pppcp_packet *) rejected_packet;

	pppcp_trace(data);

	packet = pppcp_packet_new(data, PPPCP_CODE_TYPE_CODE_REJECT,
						ntohs(old_packet->length));

	/*
	 * Identifier must be changed for each Code-Reject sent
	 */
	packet->identifier = new_identity(data, data->reject_identifier);

	/*
	 * rejected packet should be copied in, but it should be
	 * truncated if it needs to be to comply with mtu requirement
	 */
	memcpy(packet->data, rejected_packet,
			ntohs(packet->length) - CP_HEADER_SZ);

	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);
}

/*
 * transmit an Echo-Reply packet
 */
static void pppcp_send_echo_reply(struct pppcp_data *data,
						const guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *header = (struct pppcp_packet *) request;

	/*
	 * 0 bytes for data, 4 bytes for magic number
	 */
	packet = pppcp_packet_new(data, PPPCP_CODE_TYPE_ECHO_REPLY, 4);

	/*
	 * match identifier of request
	 */
	packet->identifier = header->identifier;

	/* magic number will always be zero */
	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);
}

static void pppcp_transition_state(enum pppcp_state new_state,
					struct pppcp_data *data)
{
	/*
	 * if switching from a state where
	 * TO events occur, to one where they
	 * may not, shut off the timer
	 */
	switch (new_state) {
	case INITIAL:
	case STARTING:
	case CLOSED:
	case STOPPED:
	case OPENED:
		pppcp_stop_timer(&data->config_timer_data);
		pppcp_stop_timer(&data->terminate_timer_data);
		break;
	case CLOSING:
	case STOPPING:
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		break;
	}
	data->state = new_state;
}

/*
 * send the event handler a new event to process
 */
static void pppcp_generate_event(struct pppcp_data *data,
				enum pppcp_event_type event_type,
				const guint8 *packet, guint len)
{
	int actions;
	unsigned char new_state;

	if (event_type > RXR)
		goto error;

	pppcp_trace(data);

	actions = cp_transitions[event_type][data->state];
	new_state = actions & 0xf;

	pppcp_trace_event(data, event_type, actions, new_state);

	if (actions & INV)
		goto error;

	if (actions & IRC) {
		struct pppcp_timer_data *timer_data;

		if (new_state == CLOSING || new_state == STOPPING)
			timer_data = &data->terminate_timer_data;
		else
			timer_data = &data->config_timer_data;

		pppcp_initialize_restart_count(timer_data);
	} else if (actions & ZRC)
		pppcp_zero_restart_count(&data->terminate_timer_data);

	if (actions & SCR)
		pppcp_send_configure_request(data);

	if (actions & SCA)
		pppcp_send_configure_ack(data, packet);
	else if (actions & SCN)
		pppcp_send_configure_nak(data, packet);

	if (actions & STR)
		pppcp_send_terminate_request(data);
	else if (actions & STA)
		pppcp_send_terminate_ack(data, packet);

	if (actions & SCJ)
		pppcp_send_code_reject(data, packet);

	if (actions & SER)
		pppcp_send_echo_reply(data, packet);

	pppcp_transition_state(new_state, data);

	if (actions & TLS)
		pppcp_this_layer_started(data);
	else if (actions & TLU)
		pppcp_this_layer_up(data);
	else if (actions & TLD)
		pppcp_this_layer_down(data);
	else if (actions & TLF)
		pppcp_this_layer_finished(data);

	return;

error:
	pppcp_illegal_event(data->ppp, data->state, event_type);
}

void pppcp_signal_open(struct pppcp_data *data)
{
	pppcp_generate_event(data, OPEN, NULL, 0);
}

void pppcp_signal_close(struct pppcp_data *data)
{
	pppcp_generate_event(data, CLOSE, NULL, 0);
}

void pppcp_signal_up(struct pppcp_data *data)
{
	pppcp_generate_event(data, UP, NULL, 0);
}

void pppcp_signal_down(struct pppcp_data *data)
{
	pppcp_generate_event(data, DOWN, NULL, 0);
}

static guint8 pppcp_process_configure_request(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet)
{
	pppcp_trace(pppcp);

	if (pppcp->failure_counter >= pppcp->max_failure)
		return RCR_MINUS;

	if (pppcp->driver->rcr) {
		enum rcr_result res;

		res = pppcp->driver->rcr(pppcp, packet,
						&pppcp->peer_options,
						&pppcp->peer_options_len);

		if (res == RCR_REJECT) {
			pppcp->send_reject = TRUE;
			return RCR_MINUS;
		} else if (res == RCR_NAK) {
			pppcp->send_reject = FALSE;
			return RCR_MINUS;
		}
	}

	return RCR_PLUS;
}

static guint8 pppcp_process_configure_ack(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet)
{
	gint len;

	pppcp_trace(pppcp);

	len = ntohs(packet->length) - CP_HEADER_SZ;

	/* if identifiers don't match, we should silently discard */
	if (packet->identifier != pppcp->config_identifier) {
		return 0;
	}

	/*
	 * First we must sanity check that all config options acked are
	 * equal to the config options sent and are in the same order.
	 * If this is not the case, then silently drop the packet
	 */
	if (pppcp->local_options_len != len)
		return 0;

	if (memcmp(pppcp->local_options, packet->data, len))
		return 0;

	/* Otherwise, apply local options */
	if (pppcp->driver->rca)
		pppcp->driver->rca(pppcp, packet);

	return RCA;
}

static guint8 pppcp_process_configure_nak(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet)
{
	pppcp_trace(pppcp);

	/* if identifiers don't match, we should silently discard */
	if (packet->identifier != pppcp->config_identifier)
		return 0;

	if (pppcp->driver->rcn_nak)
		pppcp->driver->rcn_nak(pppcp, packet);

	return RCN;
}

static guint8 pppcp_process_configure_reject(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet)
{
	pppcp_trace(pppcp);

	/*
	 * make sure identifier matches that of last sent configure
	 * request
	 */
	if (packet->identifier != pppcp->config_identifier)
		return 0;

	/*
	 * check to see which options were rejected
	 * Rejected options must be a subset of requested
	 * options and in the same order.
	 *
	 * when a new configure-request is sent, we may
	 * not request any of these options be negotiated
	 */
	if (pppcp->driver->rcn_rej)
		pppcp->driver->rcn_rej(pppcp, packet);

	return RCN;
}

static guint8 pppcp_process_terminate_request(struct pppcp_data *data,
					const struct pppcp_packet *packet)
{
	pppcp_trace(data);

	return RTR;
}

static guint8 pppcp_process_terminate_ack(struct pppcp_data *data,
					const struct pppcp_packet *packet)
{
	/*
	 * if we wind up using the data field for anything, then
	 * we'd want to check the identifier.
	 * even if the identifiers don't match, we still handle
	 * a terminate ack, as it is allowed to be unelicited
	 */
	pppcp_trace(data);

	return RTA;
}

static guint8 pppcp_process_code_reject(struct pppcp_data *data,
					const struct pppcp_packet *packet)
{
	/*
	 * determine if the code reject is catastrophic or not.
	 * return RXJ_PLUS if this reject is acceptable, RXJ_MINUS if
	 * it is catastrophic.
	 *
	 * for now we always return RXJ_MINUS.  Any code
	 * reject will be catastrophic, since we only support the
	 * bare minimum number of codes necessary to function.
	 */
	return RXJ_MINUS;
}

static guint8 pppcp_process_protocol_reject(struct pppcp_data *data,
					const struct pppcp_packet *packet)
{
	/*
	 * determine if the protocol reject is catastrophic or not.
	 * return RXJ_PLUS if this reject is acceptable, RXJ_MINUS if
	 * it is catastrophic.
	 *
	 * for now we always return RXJ_MINUS.  Any protocol
	 * reject will be catastrophic, since we only support the
	 * bare minimum number of protocols necessary to function.
	 */
	return RXJ_MINUS;
}

/*
 * For Echo-Request, Echo-Reply, and Discard-Request, we will not
 * bother checking the magic number of the packet, because we will
 * never send an echo or discard request.  We can't reliably detect
 * loop back anyway, since we don't negotiate a magic number.
 */
static guint8 pppcp_process_echo_request(struct pppcp_data *data,
					const struct pppcp_packet *packet)
{
	return RXR;
}

static guint8 pppcp_process_echo_reply(struct pppcp_data *data,
					const struct pppcp_packet *packet)
{
	return 0;
}

static guint8 pppcp_process_discard_request(struct pppcp_data *data,
					const struct pppcp_packet *packet)
{
	return 0;
}

static guint8 (*packet_ops[11])(struct pppcp_data *data,
					const struct pppcp_packet *packet) = {
	pppcp_process_configure_request,
	pppcp_process_configure_ack,
	pppcp_process_configure_nak,
	pppcp_process_configure_reject,
	pppcp_process_terminate_request,
	pppcp_process_terminate_ack,
	pppcp_process_code_reject,
	pppcp_process_protocol_reject,
	pppcp_process_echo_request,
	pppcp_process_echo_reply,
	pppcp_process_discard_request,
};

void pppcp_send_protocol_reject(struct pppcp_data *data,
				const guint8 *rejected_packet, gsize len)
{
	struct pppcp_packet *packet;

	pppcp_trace(data);

	/*
	 * Protocol-Reject can only be sent when we are in
	 * the OPENED state.  If in any other state, silently discard.
	 */
	if (data->state != OPENED)
		return;

	/*
	 * info should contain the old packet info, plus the 16bit
	 * protocol number we are rejecting.
	 */
	packet = pppcp_packet_new(data, PPPCP_CODE_TYPE_PROTOCOL_REJECT,
					len - 2);

	/*
	 * Identifier must be changed for each Protocol-Reject sent
	 */
	packet->identifier = new_identity(data, data->reject_identifier);

	/*
	 * rejected packet should be copied in, but it should be
	 * truncated if it needs to be to comply with mtu requirement
	 */
	memcpy(packet->data, rejected_packet + 2, len - 2);

	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);
}

/*
 * parse the packet and determine which event this packet caused
 */
void pppcp_process_packet(gpointer priv, const guint8 *new_packet, gsize len)
{
	struct pppcp_data *data = priv;
	const struct pppcp_packet *packet =
				(const struct pppcp_packet *) new_packet;
	guint8 event_type;
	guint data_len = 0;

	if (len < sizeof(struct pppcp_packet))
		return;

	/* check flags to see if we support this code */
	if (!(data->driver->supported_codes & (1 << packet->code)))
		event_type = RUC;
	else
		event_type = packet_ops[packet->code-1](data, packet);

	if (event_type) {
		data_len = ntohs(packet->length);
		pppcp_generate_event(data, event_type, new_packet, data_len);
	}
}

void pppcp_free(struct pppcp_data *pppcp)
{
	pppcp_stop_timer(&pppcp->config_timer_data);
	pppcp_stop_timer(&pppcp->terminate_timer_data);
	g_free(pppcp->peer_options);
	g_free(pppcp);
}

void pppcp_set_data(struct pppcp_data *pppcp, gpointer data)
{
	pppcp->priv = data;
}

gpointer pppcp_get_data(struct pppcp_data *pppcp)
{
	return pppcp->priv;
}

GAtPPP *pppcp_get_ppp(struct pppcp_data *pppcp)
{
	return pppcp->ppp;
}

void pppcp_set_local_options(struct pppcp_data *pppcp,
					const guint8 *options, guint16 len)
{
	pppcp->local_options = options;
	pppcp->local_options_len = len;
}

struct pppcp_data *pppcp_new(GAtPPP *ppp, const struct pppcp_proto *proto,
				gboolean dormant, guint max_failure)
{
	struct pppcp_data *data;

	data = g_try_malloc0(sizeof(struct pppcp_data));
	if (data == NULL)
		return NULL;

	if (dormant)
		data->state = STOPPED;
	else
		data->state = INITIAL;

	data->config_timer_data.restart_interval = INITIAL_RESTART_TIMEOUT;
	data->terminate_timer_data.restart_interval = INITIAL_RESTART_TIMEOUT;
	data->config_timer_data.max_counter = MAX_CONFIGURE;
	data->terminate_timer_data.max_counter = MAX_TERMINATE;
	data->config_timer_data.data = data;
	data->terminate_timer_data.data = data;

	if (max_failure)
		data->max_failure = max_failure;
	else
		data->max_failure = MAX_FAILURE;

	data->ppp = ppp;
	data->driver = proto;

	return data;
}
