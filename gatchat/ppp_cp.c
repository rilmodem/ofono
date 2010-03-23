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

#ifdef DEBUG
static const char *pppcp_state_strings[] =
	{"INITIAL", "STARTING", "CLOSED", "STOPPED", "CLOSING", "STOPPING",
	"REQSENT", "ACKRCVD", "ACKSENT", "OPENED" };

#define pppcp_trace(p) do { \
	g_print("%s: current state %d:%s\n", __FUNCTION__, \
		p->state, pppcp_state_strings[p->state]); \
} while (0)
#else
#define pppcp_trace(p) do { } while (0)
#endif

#define pppcp_to_ppp_packet(p) \
	(p-PPP_HEADROOM)

struct pppcp_event {
	enum pppcp_event_type type;
	gint len;
	guint8 data[0];
};

#define INITIAL_RESTART_TIMEOUT	3000
#define MAX_TERMINATE		2
#define MAX_CONFIGURE		10
#define MAX_FAILURE		5
#define CP_HEADER_SZ		4

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
	struct pppcp_data *data = user_data;

	pppcp_trace(data);

	data->restart_timer = 0;

	if (data->restart_counter)
		pppcp_generate_event(data, TO_PLUS, NULL, 0);
	else
		pppcp_generate_event(data, TO_MINUS, NULL, 0);

	return FALSE;
}

static void pppcp_start_timer(struct pppcp_data *data)
{
	data->restart_timer = g_timeout_add(data->restart_interval,
				pppcp_timeout, data);
}

static void pppcp_stop_timer(struct pppcp_data *data)
{
	if (data->restart_timer) {
		g_source_remove(data->restart_timer);
		data->restart_timer = 0;
	}
}

static gboolean pppcp_timer_is_running(struct pppcp_data *data)
{
	/* determine if the restart timer is running */
	if (data->restart_timer)
		return TRUE;
	return FALSE;
}

static struct pppcp_event *pppcp_event_new(enum pppcp_event_type type,
					gpointer event_data, guint len)
{
	struct pppcp_event *event;
	guint8 *data = event_data;

	event = g_try_malloc0(sizeof(struct pppcp_event) + len);
	if (!event)
		return NULL;

	event->type = type;
	memcpy(event->data, data, len);
	event->len = len;
	return event;
}

static struct pppcp_event *pppcp_get_event(struct pppcp_data *data)
{
	return g_queue_pop_head(data->event_queue);
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

	if (action->this_layer_up)
		action->this_layer_down(data);
}

static void pppcp_this_layer_started(struct pppcp_data *data)
{
	struct pppcp_action *action = data->action;

	if (action->this_layer_up)
		action->this_layer_started(data);
}

static void pppcp_this_layer_finished(struct pppcp_data *data)
{
	struct pppcp_action *action = data->action;

	if (action->this_layer_up)
		action->this_layer_finished(data);
}

static void pppcp_free_option(gpointer data, gpointer user_data)
{
	struct ppp_option *option = data;
	g_free(option);
}

static void pppcp_clear_options(struct pppcp_data *data)
{
	g_list_foreach(data->acceptable_options, pppcp_free_option, NULL);
	g_list_foreach(data->unacceptable_options, pppcp_free_option, NULL);
	g_list_foreach(data->rejected_options, pppcp_free_option, NULL);
	g_list_free(data->acceptable_options);
	g_list_free(data->unacceptable_options);
	g_list_free(data->rejected_options);
	data->acceptable_options = NULL;
	data->unacceptable_options = NULL;
	data->rejected_options = NULL;
}

/*
 * set the restart counter to either max-terminate
 * or max-configure.  The counter is decremented for
 * each transmission, including the first.
 */
static void pppcp_initialize_restart_count(struct pppcp_data *data, guint value)
{
	pppcp_trace(data);
	pppcp_clear_options(data);
	data->restart_counter = value;
}

/*
 * set restart counter to zero
 */
static void pppcp_zero_restart_count(struct pppcp_data *data)
{
	data->restart_counter = 0;
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

	pppcp_trace(data);

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
	if (data->restart_counter == data->max_configure)
		data->config_identifier =
			new_identity(data, data->config_identifier);
	packet->identifier = data->config_identifier;

	ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
			ntohs(packet->length));

	/* XXX don't retransmit right now */
#if 0
	data->restart_counter--;
	pppcp_start_timer(data);
#endif
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

	/* subtract for header. */
	len = ntohs(pppcp_header->length) - sizeof(*packet);

	packet = pppcp_packet_new(data, CONFIGURE_ACK, len);

	/* copy the applied options in. */
	odata = packet->data;

	g_list_foreach(data->acceptable_options, copy_option, &odata);

	/* match identifier of the request */
	packet->identifier = pppcp_header->identifier;

	ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
			ntohs(packet->length));
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
	guint8 olength = 0;
	guint8 *odata;

	/* if we have any rejected options, send a config-reject */
	if (g_list_length(data->rejected_options)) {
		/* figure out how much space to allocate for options */
		g_list_foreach(data->rejected_options, get_option_length,
				&olength);

		packet = pppcp_packet_new(data, CONFIGURE_REJECT, olength);

		/* copy the rejected options in. */
		odata = packet->data;
		g_list_foreach(data->rejected_options, copy_option,
				&odata);

		packet->identifier = pppcp_header->identifier;
		ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
			ntohs(packet->length));

	}
	/* if we have any unacceptable options, send a config-nak */
	if (g_list_length(data->unacceptable_options)) {
		olength = 0;

		/* figure out how much space to allocate for options */
		g_list_foreach(data->unacceptable_options, get_option_length,
				&olength);

		packet = pppcp_packet_new(data, CONFIGURE_NAK, olength);

		/* copy the unacceptable options in. */
		odata = packet->data;
		g_list_foreach(data->unacceptable_options, copy_option,
				&odata);

		packet->identifier = pppcp_header->identifier;
		ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
				ntohs(packet->length));
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

	/*
	 * the data field can be used by the sender (us).
	 * leave this empty for now.
	 */
	packet = pppcp_packet_new(data, TERMINATE_REQUEST, 0);

	/*
	 * Is this a retransmission?  If so, do not change
	 * the identifier.  If not, we need a fresh identity.
	 */
	if (data->restart_counter == data->max_terminate)
		data->terminate_identifier =
			new_identity(data, data->terminate_identifier);
	packet->identifier = data->terminate_identifier;
	ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
			ntohs(packet->length));
	data->restart_counter--;
	pppcp_start_timer(data);
}

/*
 * transmit a Terminate-Ack packet
 */
static void pppcp_send_terminate_ack(struct pppcp_data *data,
					guint8 *request)
{
	struct pppcp_packet *packet;
	struct pppcp_packet *pppcp_header = (struct pppcp_packet *) request;

	packet = pppcp_packet_new(data, TERMINATE_ACK, 0);

	/* match identifier of the request */
	packet->identifier = pppcp_header->identifier;

	ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
			ntohs(pppcp_header->length));
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

	packet = pppcp_packet_new(data, CODE_REJECT,
			ntohs(((struct pppcp_packet *) rejected_packet)->length));

	/*
	 * Identifier must be changed for each Code-Reject sent
	 */
	packet->identifier = new_identity(data, data->reject_identifier);

	/*
	 * rejected packet should be copied in, but it should be
	 * truncated if it needs to be to comply with mtu requirement
	 */
	memcpy(packet->data, rejected_packet,
			ntohs(packet->length - CP_HEADER_SZ));

	ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
			ntohs(packet->length));
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
	ppp_transmit(data->ppp, pppcp_to_ppp_packet((guint8 *) packet),
			ntohs(packet->length));

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
		/* if timer is running, stop it */
		if (pppcp_timer_is_running(data))
			pppcp_stop_timer(data);
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

static void pppcp_up_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);
	switch (data->state) {
	case INITIAL:
		/* switch state to CLOSED */
		pppcp_transition_state(CLOSED, data);
		break;
	case STARTING:
		/* irc, scr/6 */
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case CLOSED:
	case STOPPED:
	case OPENED:
	case CLOSING:
	case STOPPING:
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		pppcp_illegal_event(data->state, UP);
	}
}

static void pppcp_down_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	switch (data->state) {
	case CLOSED:
		pppcp_transition_state(INITIAL, data);
		break;
	case STOPPED:
		/* tls/1 */
		pppcp_transition_state(STARTING, data);
		pppcp_this_layer_started(data);
		break;
	case CLOSING:
		pppcp_transition_state(INITIAL, data);
		break;
	case STOPPING:
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		pppcp_transition_state(STARTING, data);
		break;
	case OPENED:
		pppcp_transition_state(STARTING, data);
		pppcp_this_layer_down(data);
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, DOWN);
		/* illegal */
	}
}

static void pppcp_open_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);
	switch (data->state) {
	case INITIAL:
		/* tls/1 */
		pppcp_transition_state(STARTING, data);
		pppcp_this_layer_started(data);
		break;
	case STARTING:
		pppcp_transition_state(STARTING, data);
		break;
	case CLOSED:
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case STOPPED:
		/* 3r */
		pppcp_transition_state(STOPPED, data);
		break;
	case CLOSING:
	case STOPPING:
		/* 5r */
		pppcp_transition_state(STOPPING, data);
		break;
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		pppcp_transition_state(data->state, data);
		break;
	case OPENED:
		/* 9r */
		pppcp_transition_state(data->state, data);
		break;
	}
}

static void pppcp_close_event(struct pppcp_data *data, guint8* packet, guint len)
{
	switch (data->state) {
	case INITIAL:
		pppcp_transition_state(INITIAL, data);
		break;
	case STARTING:
		pppcp_this_layer_finished(data);
		pppcp_transition_state(INITIAL, data);
		break;
	case CLOSED:
	case STOPPED:
		pppcp_transition_state(CLOSED, data);
		break;
	case CLOSING:
	case STOPPING:
		pppcp_transition_state(CLOSING, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		pppcp_initialize_restart_count(data, data->max_terminate);
		pppcp_send_terminate_request(data);
		pppcp_transition_state(CLOSING, data);
		break;
	}
}

static void pppcp_to_plus_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSING:
		pppcp_send_terminate_request(data);
		pppcp_transition_state(CLOSING, data);
		break;
	case STOPPING:
		pppcp_send_terminate_request(data);
		pppcp_transition_state(STOPPING, data);
		break;
	case REQSENT:
	case ACKRCVD:
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case ACKSENT:
		pppcp_send_configure_request(data);
		pppcp_transition_state(ACKSENT, data);
		break;
	case INITIAL:
	case STARTING:
	case CLOSED:
	case STOPPED:
	case OPENED:
		pppcp_illegal_event(data->state, TO_PLUS);
	}
}

static void pppcp_to_minus_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);
	switch (data->state) {
	case CLOSING:
		pppcp_transition_state(CLOSED, data);
		pppcp_this_layer_finished(data);
		break;
	case STOPPING:
		pppcp_transition_state(STOPPED, data);
		pppcp_this_layer_finished(data);
		break;
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		/* tlf/3p */
		pppcp_transition_state(STOPPED, data);
		pppcp_this_layer_finished(data);
		break;
	case INITIAL:
	case STARTING:
	case CLOSED:
	case STOPPED:
	case OPENED:
		pppcp_illegal_event(data->state, TO_MINUS);
	}
}

static void pppcp_rcr_plus_event(struct pppcp_data *data,
				guint8 *packet, guint len)
{
	pppcp_trace(data);
	switch (data->state) {
	case CLOSED:
		pppcp_send_terminate_ack(data, packet);
		pppcp_transition_state(CLOSED, data);
		break;
	case STOPPED:
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_send_configure_request(data);
		pppcp_send_configure_ack(data, packet);
		pppcp_transition_state(ACKSENT, data);
		break;
	case CLOSING:
	case STOPPING:
		pppcp_transition_state(data->state, data);
		break;
	case REQSENT:
		pppcp_send_configure_ack(data, packet);
		pppcp_transition_state(ACKSENT, data);
		break;
	case ACKRCVD:
		pppcp_send_configure_ack(data, packet);
		pppcp_this_layer_up(data);
		pppcp_transition_state(OPENED, data);
		break;
	case ACKSENT:
		pppcp_send_configure_ack(data, packet);
		pppcp_transition_state(ACKSENT, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
		pppcp_send_configure_request(data);
		pppcp_send_configure_ack(data, packet);
		pppcp_transition_state(ACKSENT, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RCR_PLUS);
	}
}

static void pppcp_rcr_minus_event(struct pppcp_data *data,
				guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
		pppcp_send_terminate_ack(data, packet);
		pppcp_transition_state(CLOSED, data);
		break;
	case STOPPED:
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_send_configure_request(data);
		pppcp_send_configure_nak(data, packet);
		pppcp_transition_state(REQSENT, data);
		break;
	case CLOSING:
	case STOPPING:
		pppcp_transition_state(data->state, data);
		break;
	case REQSENT:
	case ACKRCVD:
		pppcp_send_configure_nak(data, packet);
		pppcp_transition_state(data->state, data);
		break;
	case ACKSENT:
		pppcp_send_configure_nak(data, packet);
		pppcp_transition_state(REQSENT, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
		pppcp_send_configure_request(data);
		pppcp_send_configure_nak(data, packet);
		pppcp_transition_state(REQSENT, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RCR_MINUS);
	}
}

static void pppcp_rca_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
		pppcp_send_terminate_ack(data, packet);
	case CLOSING:
	case STOPPING:
		pppcp_transition_state(data->state, data);
		break;
	case REQSENT:
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_transition_state(ACKRCVD, data);
		break;
	case ACKRCVD:
		/* scr/6x */
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
	case ACKSENT:
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_this_layer_up(data);
		pppcp_transition_state(OPENED, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RCA);
	}
}

static void pppcp_rcn_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
		pppcp_send_terminate_ack(data, packet);
	case CLOSING:
	case STOPPING:
		pppcp_transition_state(data->state, data);
	case REQSENT:
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case ACKRCVD:
		/* scr/6x */
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case ACKSENT:
		pppcp_initialize_restart_count(data, data->max_configure);
		pppcp_send_configure_request(data);
		pppcp_transition_state(ACKSENT, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RCN);
	}
}

static void pppcp_rtr_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
		pppcp_send_terminate_ack(data, packet);
	case CLOSING:
	case STOPPING:
		break;
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		pppcp_send_terminate_ack(data, packet);
		pppcp_transition_state(REQSENT, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
		pppcp_zero_restart_count(data);
		pppcp_send_terminate_ack(data, packet);
		pppcp_transition_state(STOPPING, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RTR);
	}
}

static void pppcp_rta_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
		pppcp_transition_state(data->state, data);
		break;
	case CLOSING:
		pppcp_this_layer_finished(data);
		pppcp_transition_state(CLOSED, data);
		break;
	case STOPPING:
		pppcp_this_layer_finished(data);
		pppcp_transition_state(STOPPED, data);
		break;
	case REQSENT:
	case ACKRCVD:
		pppcp_transition_state(REQSENT, data);
		break;
	case ACKSENT:
		pppcp_transition_state(ACKSENT, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
		pppcp_send_configure_request(data);
		pppcp_transition_state(REQSENT, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RTA);
	}
}

static void pppcp_ruc_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
	case CLOSING:
	case STOPPING:
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
	case OPENED:
		pppcp_send_code_reject(data, packet);
		pppcp_transition_state(data->state, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RUC);
	}
}

static void pppcp_rxj_plus_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
	case CLOSING:
	case STOPPING:
		pppcp_transition_state(data->state, data);
		break;
	case REQSENT:
	case ACKRCVD:
		pppcp_transition_state(REQSENT, data);
		break;
	case ACKSENT:
	case OPENED:
		pppcp_transition_state(data->state, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RXJ_PLUS);
	}
}

static void pppcp_rxj_minus_event(struct pppcp_data *data,
				guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
		pppcp_this_layer_finished(data);
		pppcp_transition_state(data->state, data);
		break;
	case CLOSING:
		pppcp_this_layer_finished(data);
		pppcp_transition_state(CLOSED, data);
		break;
	case STOPPING:
		pppcp_this_layer_finished(data);
		pppcp_transition_state(STOPPED, data);
		break;
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		pppcp_this_layer_finished(data);
		pppcp_transition_state(STOPPED, data);
		break;
	case OPENED:
		pppcp_this_layer_down(data);
		pppcp_initialize_restart_count(data, data->max_terminate);
		pppcp_send_terminate_request(data);
		pppcp_transition_state(STOPPING, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RXJ_MINUS);
	}
}

static void pppcp_rxr_event(struct pppcp_data *data, guint8 *packet, guint len)
{
	pppcp_trace(data);

	switch (data->state) {
	case CLOSED:
	case STOPPED:
	case CLOSING:
	case STOPPING:
	case REQSENT:
	case ACKRCVD:
	case ACKSENT:
		pppcp_transition_state(data->state, data);
		break;
	case OPENED:
		pppcp_send_echo_reply(data, packet);
		pppcp_transition_state(OPENED, data);
		break;
	case INITIAL:
	case STARTING:
		pppcp_illegal_event(data->state, RXR);
	}
}

static void pppcp_handle_event(gpointer user_data)
{
	struct pppcp_event *event;
	struct pppcp_data *data = user_data;

	while ((event = pppcp_get_event(data))) {
		if (event->type > RXR)
			pppcp_illegal_event(data->state, event->type);
		else
			data->event_ops[event->type](data, event->data,
							event->len);
		g_free(event);
	}
}

/*
 * send the event handler a new event to process
 */
void pppcp_generate_event(struct pppcp_data *data,
				enum pppcp_event_type event_type,
				gpointer event_data, guint data_len)
{
	struct pppcp_event *event;

	event = pppcp_event_new(event_type, event_data, data_len);
	if (event)
		g_queue_push_tail(data->event_queue, event);
	pppcp_handle_event(data);
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

	data->config_options = g_list_delete_link(data->config_options, list);
}

static guint8 pppcp_process_configure_request(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	gint len;
	int i = 0;
	struct ppp_option *option;
	enum option_rval rval = OPTION_ERR;
	struct pppcp_action *action = data->action;

	pppcp_trace(data);

	len = ntohs(packet->length) - CP_HEADER_SZ;

	/*
	 * check the options.
	 */
	while (i < len) {
		guint8 otype = packet->data[i];
		guint8 olen = packet->data[i+1];
		option = g_try_malloc0(olen);
		if (option == NULL)
			break;
		option->type = otype;
		option->length = olen;
		memcpy(option->data, &packet->data[i+2], olen-2);
		if (action->option_scan)
			rval = action->option_scan(option, data);
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
			g_printerr("unhandled option type %d\n", otype);
		}
		/* skip ahead to the next option */
		i += olen;
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
		g_list_foreach(data->acceptable_options,
				action->option_process, data->priv);
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
		guint8 otype = packet->data[i];
		guint8 olen = packet->data[i + 1];
		acked_option = g_try_malloc0(olen);
		if (acked_option == NULL)
			break;
		acked_option->type = otype;
		acked_option->length = olen;
		memcpy(acked_option->data, &packet->data[i + 2], olen - 2);
		list = g_list_find_custom(data->config_options,
				GUINT_TO_POINTER((guint) otype), is_option);
		if (list) {
			/*
			 * once we've applied the option, delete it from
			 * the config_options list.
			 */
			if (action->option_process)
				action->option_process(acked_option,
							data->priv);
			data->config_options =
				g_list_delete_link(data->config_options, list);
		} else
			g_printerr("oops -- found acked option %d we didn't request\n", acked_option->type);
		g_free(acked_option);

		/* skip ahead to the next option */
		i += olen;
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
	enum option_rval rval = OPTION_ERR;
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
		guint8 otype = packet->data[i];
		guint8 olen = packet->data[i+1];
		naked_option = g_try_malloc0(olen);
		if (naked_option == NULL)
			break;
		naked_option->type = otype;
		naked_option->length = olen;
		memcpy(naked_option->data, &packet->data[i + 2], olen - 2);
		if (action->option_scan)
			rval = action->option_scan(naked_option, data);
		if (rval == OPTION_ACCEPT) {
			/*
			 * check the current config options to see if they
			 * match.
			 */
			list = g_list_find_custom(data->config_options,
				GUINT_TO_POINTER((guint) otype), is_option);
			if (list) {
				/* modify current option value to match */
				config_option = list->data;

				/*
				 * option values should match, otherwise
				 * we need to reallocate
				 */
				if ((config_option->length ==
					naked_option->length) && (olen - 2)) {
						memcpy(config_option->data,
							naked_option->data,
							olen - 2);
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

		/* skip ahead to the next option */
		i += olen;
	}
	return RCN;
}

static guint8 pppcp_process_configure_reject(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	/*
	 * make sure identifier matches that of last sent configure
	 * request
	 */
	if (packet->identifier == data->config_identifier) {
		/*
		 * check to see which options were rejected
		 * Rejected options must be a subset of requested
		 * options.
		 *
		 * when a new configure-request is sent, we may
		 * not request any of these options be negotiated
		 */
		return RCN;
	}
	return 0;
}

static guint8 pppcp_process_terminate_request(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
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
	return RTA;
}

static guint8 pppcp_process_code_reject(struct pppcp_data *data,
					struct pppcp_packet *packet)
{
	/*
	 * determine if the code reject is catastrophic or not.
	 * return RXJ_PLUS if this reject is acceptable, RXJ_MINUS if
	 * it is catastrophic.
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
		event_type = data->packet_ops[packet->code-1](data, packet);

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

	/* free event queue */
	if (!g_queue_is_empty(data->event_queue))
		g_queue_foreach(data->event_queue, (GFunc) g_free, NULL);
	g_queue_free(data->event_queue);

	/* remove all config options */
	pppcp_clear_options(data);

	/* free self */
	g_free(data);
}

struct pppcp_data *pppcp_new(GAtPPP *ppp, guint16 proto,
				gpointer priv)
{
	struct pppcp_data *data;

	data = g_try_malloc0(sizeof(struct pppcp_data));
	if (!data)
		return NULL;

	data->state = INITIAL;
	data->restart_interval = INITIAL_RESTART_TIMEOUT;
	data->max_terminate = MAX_TERMINATE;
	data->max_configure = MAX_CONFIGURE;
	data->max_failure = MAX_FAILURE;
	data->event_queue = g_queue_new();
	data->identifier = 0;
	data->ppp = ppp;
	data->proto = proto;
	data->priv = priv;

	/* setup func ptrs for processing packet by pppcp code */
	data->packet_ops[CONFIGURE_REQUEST - 1] =
					pppcp_process_configure_request;
	data->packet_ops[CONFIGURE_ACK - 1] = pppcp_process_configure_ack;
	data->packet_ops[CONFIGURE_NAK - 1] = pppcp_process_configure_nak;
	data->packet_ops[CONFIGURE_REJECT - 1] = pppcp_process_configure_reject;
	data->packet_ops[TERMINATE_REQUEST - 1] =
					pppcp_process_terminate_request;
	data->packet_ops[TERMINATE_ACK - 1] = pppcp_process_terminate_ack;
	data->packet_ops[CODE_REJECT - 1] = pppcp_process_code_reject;
	data->packet_ops[PROTOCOL_REJECT - 1] = pppcp_process_protocol_reject;
	data->packet_ops[ECHO_REQUEST - 1] = pppcp_process_echo_request;
	data->packet_ops[ECHO_REPLY - 1] = pppcp_process_echo_reply;
	data->packet_ops[DISCARD_REQUEST - 1] = pppcp_process_discard_request;

	/* setup func ptrs for handling events by event type */
	data->event_ops[UP] = pppcp_up_event;
	data->event_ops[DOWN] = pppcp_down_event;
	data->event_ops[OPEN] = pppcp_open_event;
	data->event_ops[CLOSE] = pppcp_close_event;
	data->event_ops[TO_PLUS] = pppcp_to_plus_event;
	data->event_ops[TO_MINUS] = pppcp_to_minus_event;
	data->event_ops[RCR_PLUS] = pppcp_rcr_plus_event;
	data->event_ops[RCR_MINUS] = pppcp_rcr_minus_event;
	data->event_ops[RCA] = pppcp_rca_event;
	data->event_ops[RCN] = pppcp_rcn_event;
	data->event_ops[RTR] = pppcp_rtr_event;
	data->event_ops[RTA] = pppcp_rta_event;
	data->event_ops[RUC] = pppcp_ruc_event;
	data->event_ops[RXJ_PLUS] = pppcp_rxj_plus_event;
	data->event_ops[RXJ_MINUS] = pppcp_rxj_minus_event;
	data->event_ops[RXR] = pppcp_rxr_event;

	return data;
}
