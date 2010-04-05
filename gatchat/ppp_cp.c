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
#include <glib.h>
#include <arpa/inet.h>

#include "gatppp.h"
#include "ppp.h"

#define pppcp_trace(p) do { \
	char *str = g_strdup_printf("%s: %s: current state %d:%s", \
				p->prefix, __FUNCTION__, \
				p->state, pppcp_state_strings[p->state]); \
	ppp_debug(p->ppp, str); \
	g_free(str); \
} while (0);

#define PPP_HEADROOM	2

#define pppcp_to_ppp_packet(p) \
	(((guint8 *) p) - PPP_HEADROOM)

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

static const char *pppcp_state_strings[] = {
	"INITIAL", "STARTING", "CLOSED", "STOPPED", "CLOSING", "STOPPING",
	"REQSENT", "ACKRCVD", "ACKSENT", "OPENED"
};

static const char *pppcp_event_strings[] = {
	"Up", "Down", "Open", "Close", "TO+", "TO-", "RCR+", "RCR-",
	"RCA", "RCN", "RTR", "RTA", "RUC", "RXJ+", "RXJ-", "RXR"
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

enum pppcp_code {
	CONFIGURE_REQUEST = 1,
	CONFIGURE_ACK,
	CONFIGURE_NAK,
	CONFIGURE_REJECT,
	TERMINATE_REQUEST,
	TERMINATE_ACK,
	CODE_REJECT,
	PROTOCOL_REJECT,
	ECHO_REQUEST,
	ECHO_REPLY,
	DISCARD_REQUEST
};

#define LCP_SUPPORTED_CODES	((1 << CONFIGURE_REQUEST) | \
				(1 << CONFIGURE_ACK) | \
				(1 << CONFIGURE_NAK) | \
				(1 << CONFIGURE_REJECT) | \
				(1 << TERMINATE_REQUEST) | \
				(1 << TERMINATE_ACK) | \
				(1 << CODE_REJECT) | \
				(1 << PROTOCOL_REJECT) | \
				(1 << ECHO_REQUEST) | \
				(1 << ECHO_REPLY) | \
				(1 << DISCARD_REQUEST))

#define IPCP_SUPPORTED_CODES	  ((1 << CONFIGURE_REQUEST) | \
				  (1 << CONFIGURE_ACK) | \
				  (1 << CONFIGURE_NAK) | \
				  (1 << CONFIGURE_REJECT) | \
				  (1 << TERMINATE_REQUEST) | \
				  (1 << TERMINATE_ACK) | \
				  (1 << CODE_REJECT))

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

static void pppcp_generate_event(struct pppcp_data *data,
				enum pppcp_event_type event_type,
				guint8 *packet, guint len);

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

	ppp_packet = g_try_malloc0(packet_length + 2);
	if (!ppp_packet)
		return NULL;

	/* add our protocol information */
	ppp_packet->proto = htons(data->proto);

	/* advance past protocol to add CP header information */
	packet = (struct pppcp_packet *) (ppp_packet->info);

	packet->length = htons(packet_length);
	packet->code = type;
	return packet;
}

static gboolean pppcp_timeout(gpointer user_data)
{
	struct pppcp_timer_data *timer_data = user_data;

	pppcp_trace(timer_data->data);

	timer_data->restart_timer = 0;

	if (timer_data->restart_counter)
		pppcp_generate_event(timer_data->data, TO_PLUS, NULL, 0);
	else
		pppcp_generate_event(timer_data->data, TO_MINUS, NULL, 0);

	return FALSE;
}

static void pppcp_start_timer(struct pppcp_timer_data *timer_data)
{
	if (timer_data->restart_timer)
		return;

	timer_data->restart_timer =
		g_timeout_add_seconds(timer_data->restart_interval,
				pppcp_timeout, timer_data);
}

static void pppcp_stop_timer(struct pppcp_timer_data *timer_data)
{
	if (timer_data->restart_timer) {
		g_source_remove(timer_data->restart_timer);
		timer_data->restart_timer = 0;
	}
}

static gboolean is_first_request(struct pppcp_timer_data *timer_data)
{
	return (timer_data->restart_counter == timer_data->max_counter);
}

/* actions */
/* log an illegal event, but otherwise do nothing */
static void pppcp_illegal_event(guint8 state, guint8 type)
{
	g_printerr("Illegal event %d while in state %d\n", type, state);
}

static void pppcp_this_layer_up(struct pppcp_data *data)
{
	struct pppcp_action *action = data->action;

	if (action->this_layer_up)
		action->this_layer_up(data);
}

static void pppcp_this_layer_down(struct pppcp_data *data)
{
	struct pppcp_action *action = data->action;

	if (action->this_layer_down)
		action->this_layer_down(data);
}

static void pppcp_this_layer_started(struct pppcp_data *data)
{
	struct pppcp_action *action = data->action;

	if (action->this_layer_started)
		action->this_layer_started(data);
}

static void pppcp_this_layer_finished(struct pppcp_data *data)
{
	struct pppcp_action *action = data->action;

	pppcp_trace(data);
	if (action->this_layer_finished)
		action->this_layer_finished(data);
}

static void pppcp_clear_options(struct pppcp_data *data)
{
	g_list_foreach(data->acceptable_options, (GFunc) g_free, NULL);
	g_list_foreach(data->unacceptable_options, (GFunc) g_free, NULL);
	g_list_foreach(data->rejected_options, (GFunc) g_free, NULL);
	g_list_free(data->acceptable_options);
	g_list_free(data->unacceptable_options);
	g_list_free(data->rejected_options);
	data->acceptable_options = NULL;
	data->unacceptable_options = NULL;
	data->rejected_options = NULL;
}

static void pppcp_free_options(struct pppcp_data *data)
{
	/* remove all config options */
	pppcp_clear_options(data);

	/* remove default option list */
	g_list_foreach(data->config_options, (GFunc) g_free, NULL);
	g_list_free(data->config_options);
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
	pppcp_clear_options(data);
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

static void get_option_length(gpointer data, gpointer user_data)
{
	struct ppp_option *option = data;
	guint8 *length = user_data;

	*length += option->length;
}

static void copy_option(gpointer data, gpointer user_data)
{
	struct ppp_option *option = data;
	guint8 **location = user_data;
	memcpy(*location, (guint8 *) option, option->length);
	*location += option->length;
}

static void reject_option(gpointer data, gpointer user_data)
{
	struct ppp_option *option = data;
	struct pppcp_data *pppcp = user_data;

	pppcp->rejected_options =
		g_list_append(pppcp->rejected_options, option);
}

static void print_option(gpointer data, gpointer user_data)
{
	struct ppp_option *option = data;
	struct pppcp_data *pppcp = user_data;

	g_print("%s: option %d len %d (%s)", pppcp->prefix, option->type,
			option->length, pppcp->option_strings[option->type]);
	if (option->length > 2) {
		int i;
		for (i = 0; i < option->length - 2; i++)
			g_print(" %02x", option->data[i]);
	}
	g_print("\n");
}

void pppcp_add_config_option(struct pppcp_data *data, struct ppp_option *option)
{
	data->config_options = g_list_append(data->config_options, option);
}

/*
 * transmit a Configure-Request packet
 * start the restart timer
 * decrement the restart counter
 */
static void pppcp_send_configure_request(struct pppcp_data *data)
{
	struct pppcp_packet *packet;
	guint8 olength = 0;
	guint8 *odata;
	struct pppcp_timer_data *timer_data = &data->config_timer_data;

	pppcp_trace(data);

	g_list_foreach(data->config_options, print_option, data);

	/* figure out how much space to allocate for options */
	g_list_foreach(data->config_options, get_option_length, &olength);

	packet = pppcp_packet_new(data, CONFIGURE_REQUEST, olength);

	/* copy config options into packet data */
	odata = packet->data;
	g_list_foreach(data->config_options, copy_option, &odata);

	/*
	 * if this is the first request, we need a new identifier.
	 * if this is a retransmission, leave the identifier alone.
	 */
	if (is_first_request(timer_data))
		data->config_identifier =
			new_identity(data, data->config_identifier);
	packet->identifier = data->config_identifier;

	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);

	/* start timer for retransmission */
	timer_data->restart_counter--;
	pppcp_start_timer(timer_data);
}

/*
 * transmit a Configure-Ack packet
 */
static void pppcp_send_configure_ack(struct pppcp_data *data,
					guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *pppcp_header = (struct pppcp_packet *) request;
	guint len;
	guint8 *odata;

	pppcp_trace(data);

	data->failure_counter = 0;

	g_list_foreach(data->acceptable_options, print_option, data);

	/* subtract for header. */
	len = ntohs(pppcp_header->length) - sizeof(*packet);

	packet = pppcp_packet_new(data, CONFIGURE_ACK, len);

	/* copy the applied options in. */
	odata = packet->data;

	g_list_foreach(data->acceptable_options, copy_option, &odata);

	/* match identifier of the request */
	packet->identifier = pppcp_header->identifier;

	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));
	pppcp_packet_free(packet);
}

/*
 * transmit a Configure-Nak or Configure-Reject packet
 */
static void pppcp_send_configure_nak(struct pppcp_data *data,
					guint8 *configure_packet)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *pppcp_header =
			(struct pppcp_packet *) configure_packet;
	guint8 olength;
	guint8 *odata;

	/*
	 * if we have exceeded our Max-Failure counter, we need
	 * to convert all packets to Configure-Reject
	 */
	if (data->failure_counter >= data->max_failure) {
		g_list_foreach(data->unacceptable_options, reject_option, data);
		g_list_free(data->unacceptable_options);
		data->unacceptable_options = NULL;
	}

	/* if we have any rejected options, send a config-reject */
	if (g_list_length(data->rejected_options)) {
		pppcp_trace(data);

		g_list_foreach(data->rejected_options, print_option, data);

		/* figure out how much space to allocate for options */
		olength = 0;
		g_list_foreach(data->rejected_options, get_option_length,
				&olength);

		packet = pppcp_packet_new(data, CONFIGURE_REJECT, olength);

		/* copy the rejected options in. */
		odata = packet->data;
		g_list_foreach(data->rejected_options, copy_option,
				&odata);

		packet->identifier = pppcp_header->identifier;
		ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

		pppcp_packet_free(packet);
	}

	/* if we have any unacceptable options, send a config-nak */
	if (g_list_length(data->unacceptable_options)) {
		pppcp_trace(data);

		g_list_foreach(data->unacceptable_options, print_option, data);

		/* figure out how much space to allocate for options */
		olength = 0;
		g_list_foreach(data->unacceptable_options, get_option_length,
				&olength);

		packet = pppcp_packet_new(data, CONFIGURE_NAK, olength);

		/* copy the unacceptable options in. */
		odata = packet->data;
		g_list_foreach(data->unacceptable_options, copy_option,
				&odata);

		packet->identifier = pppcp_header->identifier;
		ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
				ntohs(packet->length));

		pppcp_packet_free(packet);
		data->failure_counter++;
	}
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
	packet = pppcp_packet_new(data, TERMINATE_REQUEST, 0);

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
					guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *pppcp_header = (struct pppcp_packet *) request;

	pppcp_trace(data);

	packet = pppcp_packet_new(data, TERMINATE_ACK, 0);

	/* match identifier of the request */
	packet->identifier = pppcp_header->identifier;

	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(pppcp_header->length));

	pppcp_packet_free(packet);
}

/*
 * transmit a Code-Reject packet
 *
 * XXX this seg faults.
 */
static void pppcp_send_code_reject(struct pppcp_data *data,
					guint8 *rejected_packet)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *old_packet =
				(struct pppcp_packet *) rejected_packet;

	pppcp_trace(data);

	packet = pppcp_packet_new(data, CODE_REJECT, ntohs(old_packet->length));

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
				guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *header = (struct pppcp_packet *) request;

	/*
	 * 0 bytes for data, 4 bytes for magic number
	 */
	packet = pppcp_packet_new(data, ECHO_REPLY, 4);

	/*
	 * match identifier of request
	 */
	packet->identifier = header->identifier;

	/* magic number? */
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
				guint8 *packet, guint len)
{
	int actions;
	unsigned char new_state;

	if (event_type > RXR)
		goto error;

	pppcp_trace(data);

	actions = cp_transitions[event_type][data->state];
	new_state = actions & 0xf;

	g_print("event: %d (%s), action: %x, new_state: %d (%s)\n",
			event_type, pppcp_event_strings[event_type],
			actions, new_state, pppcp_state_strings[new_state]);

	if (actions & INV)
		goto error;

	if (actions & TLD)
		pppcp_this_layer_down(data);

	if (actions & TLF)
		pppcp_this_layer_finished(data);

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

	if (actions & TLU)
		pppcp_this_layer_up(data);

	pppcp_transition_state(new_state, data);

	/*
	 * The logic elsewhere generates the UP events when this is
	 * signaled.  So we must call this last
	 */
	if (actions & TLS)
		pppcp_this_layer_started(data);

	return;

error:
	pppcp_illegal_event(data->state, event_type);
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

static gint is_option(gconstpointer a, gconstpointer b)
{
	const struct ppp_option *o = a;
	guint8 otype = (guint8) GPOINTER_TO_UINT(b);

	if (o->type == otype)
		return 0;
	else
		return -1;
}

static void verify_config_option(gpointer elem, gpointer user_data)
{
	struct ppp_option *config = elem;
	struct pppcp_data *data = user_data;
	guint type = config->type;
	struct ppp_option *option;
	GList *list;

	/*
	 * determine whether this config option is in the
	 * acceptable options list
	 */
	list = g_list_find_custom(data->acceptable_options,
					GUINT_TO_POINTER(type), is_option);
	if (list)
		return;

	/*
	 * if the option did not exist, we need to store a copy
	 * of the option in the unacceptable_options list so it
	 * can be nak'ed.
	 */
	option = g_try_malloc0(config->length);
	if (option == NULL)
		return;

	option->type = config->type;
	option->length = config->length;
	data->unacceptable_options =
			g_list_append(data->unacceptable_options, option);
}

static void remove_config_option(gpointer elem, gpointer user_data)
{
	struct ppp_option *config = elem;
	struct pppcp_data *data = user_data;
	guint type = config->type;
	GList *list;

	/*
	 * determine whether this config option is in the
	 * applied options list
	 */
	list = g_list_find_custom(data->config_options,
					GUINT_TO_POINTER(type), is_option);
	if (!list)
		return;

	g_free(list->data);
	data->config_options = g_list_delete_link(data->config_options, list);
}

static struct ppp_option *extract_ppp_option(struct pppcp_data *data,
						guint8 *packet_data)
{
	struct ppp_option *option;
	guint8 otype = packet_data[0];
	guint8 olen = packet_data[1];

	option = g_try_malloc0(olen);
	if (option == NULL)
		return NULL;

	option->type = otype;
	option->length = olen;
	memcpy(option->data, &packet_data[2], olen-2);

	print_option(option, data);

	return option;
}

static guint8 pppcp_process_configure_request(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	gint len;
	int i = 0;
	struct ppp_option *option;
	enum option_rval rval;
	struct pppcp_action *action = data->action;

	pppcp_trace(data);

	len = ntohs(packet->length) - CP_HEADER_SZ;

	/*
	 * check the options.
	 */
	while (i < len) {
		option = extract_ppp_option(data, &packet->data[i]);
		if (option == NULL)
			break;

		/* skip ahead to the next option */
		i += option->length;

		if (action->option_scan)
			rval = action->option_scan(data, option);
		else
			rval = OPTION_ERR;

		switch (rval) {
		case OPTION_ACCEPT:
			data->acceptable_options =
				g_list_append(data->acceptable_options, option);
			break;
		case OPTION_REJECT:
			data->rejected_options =
				g_list_append(data->rejected_options, option);
			break;
		case OPTION_NAK:
			data->unacceptable_options =
				g_list_append(data->unacceptable_options,
						option);
			break;
		case OPTION_ERR:
			g_printerr("unhandled option type %d\n", option->type);
			g_free(option);
		}
	}

	/* make sure all required config options were included */
	g_list_foreach(data->config_options, verify_config_option, data);

	if (g_list_length(data->unacceptable_options) ||
			g_list_length(data->rejected_options))
		return RCR_MINUS;

	/*
	 * all options were acceptable, so we should apply them before
	 * sending a configure-ack
	 *
	 * Remove all applied options from the config_option list.  The
	 * protocol will have to re-add them if they want them renegotiated
	 * when the ppp goes down.
	 */
	if (action->option_process) {
		GList *l;

		for (l = data->acceptable_options; l; l = l->next)
			action->option_process(data, l->data);

		g_list_foreach(data->acceptable_options, remove_config_option,
				data);
	}

	return RCR_PLUS;
}

static guint8 pppcp_process_configure_ack(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	guint len;
	GList *list;
	struct ppp_option *acked_option;
	guint i = 0;
	struct pppcp_action *action = data->action;

	pppcp_trace(data);

	len = ntohs(packet->length) - CP_HEADER_SZ;

	/* if identifiers don't match, we should silently discard */
	if (packet->identifier != data->config_identifier) {
		g_printerr("received an ack id %d, but config id is %d\n",
			packet->identifier, data->config_identifier);
		return 0;
	}

	/*
	 * check each acked option.  If it is what we requested,
	 * then we can apply these option values.
	 *
	 * XXX what if it isn't?  Do this correctly -- for now
	 * we are just going to assume that all options matched
	 * and apply them.
	 */
	while (i < len) {
		acked_option = extract_ppp_option(data, &packet->data[i]);
		if (acked_option == NULL)
			break;

		list = g_list_find_custom(data->config_options,
				GUINT_TO_POINTER((guint) acked_option->type),
				is_option);
		if (list) {
			/*
			 * once we've applied the option, delete it from
			 * the config_options list.
			 */
			if (action->option_process)
				action->option_process(data, acked_option);

			g_free(list->data);
			data->config_options =
				g_list_delete_link(data->config_options, list);
		} else
			g_printerr("oops -- found acked option %d we didn't request\n", acked_option->type);

		/* skip ahead to the next option */
		i += acked_option->length;

		g_free(acked_option);
	}
	return RCA;
}

static guint8 pppcp_process_configure_nak(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	guint len;
	GList *list;
	struct ppp_option *naked_option;
	struct ppp_option *config_option;
	guint i = 0;
	enum option_rval rval;
	struct pppcp_action *action = data->action;

	pppcp_trace(data);

	len = ntohs(packet->length) - CP_HEADER_SZ;

	/* if identifiers don't match, we should silently discard */
	if (packet->identifier != data->config_identifier)
		return 0;

	/*
	 * check each unacceptable option.  If it is acceptable, then
	 * we can resend the configure request with this value. we need
	 * to check the current config options to see if we need to
	 * modify a value there, or add a new option.
	 */
	while (i < len) {
		naked_option = extract_ppp_option(data, &packet->data[i]);
		if (naked_option == NULL)
			break;

		/* skip ahead to the next option */
		i += naked_option->length;

		if (action->option_scan)
			rval = action->option_scan(data, naked_option);
		else
			rval = OPTION_ERR;

		if (rval == OPTION_ACCEPT) {
			/*
			 * check the current config options to see if they
			 * match.
			 */
			list = g_list_find_custom(data->config_options,
				GUINT_TO_POINTER((guint) naked_option->type),
				is_option);
			if (list) {
				/* modify current option value to match */
				config_option = list->data;

				/*
				 * option values should match, otherwise
				 * we need to reallocate
				 */
				if ((config_option->length ==
					naked_option->length) &&
							(naked_option - 2)) {
						memcpy(config_option->data,
						   naked_option->data,
						   naked_option->length - 2);
				} else {
					/* XXX implement this */
					g_printerr("uh oh, option value doesn't match\n");
				}
				g_free(naked_option);
			} else {
				/* add to list of config options */
				pppcp_add_config_option(data, naked_option);
			}
		} else {
			/* XXX handle this correctly */
			g_printerr("oops, option wasn't acceptable\n");
			g_free(naked_option);
		}
	}
	return RCN;
}

static guint8 pppcp_process_configure_reject(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	guint len;
	GList *list;
	struct ppp_option *rejected_option;
	guint i = 0;

	len = ntohs(packet->length) - CP_HEADER_SZ;

	/*
	 * make sure identifier matches that of last sent configure
	 * request
	 */
	if (packet->identifier != data->config_identifier)
		return 0;

	/*
	 * check to see which options were rejected
	 * Rejected options must be a subset of requested
	 * options.
	 *
	 * when a new configure-request is sent, we may
	 * not request any of these options be negotiated
	 */
	while (i < len) {
		rejected_option = extract_ppp_option(data, &packet->data[i]);
		if (rejected_option == NULL)
			break;

		/* skip ahead to the next option */
		i += rejected_option->length;

		/* find this option in our config options list */
		list = g_list_find_custom(data->config_options,
			GUINT_TO_POINTER((guint) rejected_option->type),
			is_option);
		if (list) {
			/* delete this config option */
			g_free(list->data);
			data->config_options =
				g_list_delete_link(data->config_options, list);
		}
		g_free(rejected_option);
	}
	return RCN;
}

static guint8 pppcp_process_terminate_request(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	pppcp_trace(data);

	return RTR;
}

static guint8 pppcp_process_terminate_ack(struct pppcp_data *data,
					struct pppcp_packet *packet)
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
					struct pppcp_packet *packet)
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
					struct pppcp_packet *packet)
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

static guint8 pppcp_process_echo_request(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	return RXR;
}

static guint8 pppcp_process_echo_reply(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	return 0;
}

static guint8 pppcp_process_discard_request(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	return 0;
}

void pppcp_send_protocol_reject(struct pppcp_data *data,
				guint8 *rejected_packet, gsize len)
{
	struct pppcp_packet *packet;
	struct ppp_header *ppp_packet = (struct ppp_header *) rejected_packet;

	pppcp_trace(data);

	/*
	 * Protocol-Reject can only be sent when we are in
	 * the OPENED state.  If in any other state, silently discard.
	 */
	if (data->state != OPENED) {
		g_free(ppp_packet);
		return;
	}

	/*
	 * info should contain the old packet info, plus the 16bit
	 * protocol number we are rejecting.
	 */
	packet = pppcp_packet_new(data, PROTOCOL_REJECT, len);

	/*
	 * Identifier must be changed for each Protocol-Reject sent
	 */
	packet->identifier = new_identity(data, data->reject_identifier);

	/*
	 * rejected packet should be copied in, but it should be
	 * truncated if it needs to be to comply with mtu requirement
	 */
	memcpy(packet->data, rejected_packet,
			(ntohs(packet->length) - CP_HEADER_SZ));

	ppp_transmit(data->ppp, pppcp_to_ppp_packet(packet),
			ntohs(packet->length));

	pppcp_packet_free(packet);
}

static guint8 (*packet_ops[11])(struct pppcp_data *data,
					struct pppcp_packet *packet) = {
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

/*
 * parse the packet and determine which event this packet caused
 */
void pppcp_process_packet(gpointer priv, guint8 *new_packet)
{
	struct pppcp_data *data = priv;
	struct pppcp_packet *packet = (struct pppcp_packet *) new_packet;
	guint8 event_type;
	gpointer event_data = NULL;
	guint data_len = 0;

	if (data == NULL)
		return;

	/* check flags to see if we support this code */
	if (!(data->valid_codes & (1 << packet->code)))
		event_type = RUC;
	else
		event_type = packet_ops[packet->code-1](data, packet);

	if (event_type) {
		data_len = ntohs(packet->length);
		event_data = packet;
		pppcp_generate_event(data, event_type, event_data, data_len);
	}
}

void pppcp_set_valid_codes(struct pppcp_data *data, guint16 codes)
{
	if (data == NULL)
		return;

	data->valid_codes = codes;
}

void pppcp_free(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* remove all config options */
	pppcp_free_options(data);

	/* free self */
	g_free(data);
}

void pppcp_set_data(struct pppcp_data *pppcp, gpointer data)
{
	pppcp->priv = data;
}

gpointer pppcp_get_data(struct pppcp_data *pppcp)
{
	return pppcp->priv;
}

struct pppcp_data *pppcp_new(GAtPPP *ppp, guint16 proto)
{
	struct pppcp_data *data;
	guint16 codes;

	data = g_try_malloc0(sizeof(struct pppcp_data));
	if (!data)
		return NULL;

	data->state = INITIAL;
	data->config_timer_data.restart_interval = INITIAL_RESTART_TIMEOUT;
	data->terminate_timer_data.restart_interval = INITIAL_RESTART_TIMEOUT;
	data->config_timer_data.max_counter = MAX_CONFIGURE;
	data->terminate_timer_data.max_counter = MAX_TERMINATE;
	data->config_timer_data.data = data;
	data->terminate_timer_data.data = data;
	data->max_failure = MAX_FAILURE;
	data->identifier = 0;

	data->ppp = ppp;
	data->proto = proto;

	switch (proto) {
	case LCP_PROTOCOL:
		codes = LCP_SUPPORTED_CODES;
		break;
	case IPCP_PROTO:
		codes = IPCP_SUPPORTED_CODES;
		break;
	default:
		codes = 0;
		break;
	}

	pppcp_set_valid_codes(data, codes);

	return data;
}
