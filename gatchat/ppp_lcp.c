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

enum lcp_options {
	RESERVED 		= 0,
	MRU			= 1,
	ACCM			= 2,
	AUTH_PROTO		= 3,
	QUAL_PROTO		= 4,
	MAGIC_NUMBER		= 5,
	DEPRECATED_QUAL_PROTO	= 6,
	PFC			= 7,
	ACFC			= 8,
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

/*
 * signal the Up event to the NCP
 */
static void lcp_up(struct pppcp_data *pppcp)
{
	ppp_generate_event(pppcp->ppp, PPP_OPENED);
}

/*
 * signal the Down event to the NCP
 */
static void lcp_down(struct pppcp_data *pppcp)
{
	ppp_generate_event(pppcp->ppp, PPP_DOWN);
}

/*
 * Indicate that the lower layer is now needed
 * Should trigger Up event
 */
static void lcp_started(struct pppcp_data *pppcp)
{
	ppp_generate_event(pppcp->ppp, PPP_UP);
}

/*
 * Indicate that the lower layer is not needed
 * Should trigger Down event
 */
static void lcp_finished(struct pppcp_data *pppcp)
{
	ppp_generate_event(pppcp->ppp, PPP_CLOSING);
}

/*
 * Scan the option to see if it is acceptable, unacceptable, or rejected
 *
 * We need to use a default case here because this option type value
 * could be anything.
 */
static guint lcp_option_scan(struct ppp_option *option, gpointer user)
{
	switch (option->type) {
	case ACCM:
	case AUTH_PROTO:
		/* XXX check to make sure it's a proto we recognize */
	case MAGIC_NUMBER:
	case PFC:
	case ACFC:
		return OPTION_ACCEPT;
		break;
	default:
		return OPTION_REJECT;
	}
}

/*
 * act on an acceptable option
 *
 * We need to use a default case here because this option type value
 * could be anything.
 */
static void lcp_option_process(gpointer data, gpointer user)
{
	struct ppp_option *option = data;
	struct pppcp_data *pppcp = user;
	GAtPPP *ppp = pppcp->ppp;
	guint32 magic;

	switch (option->type) {
	case ACCM:
		ppp_set_recv_accm(ppp, get_host_long(option->data));
		break;
	case AUTH_PROTO:
		ppp_set_auth(ppp, option->data);
		break;
	case MAGIC_NUMBER:
		/* XXX handle loopback */
		magic = get_host_long(option->data);
		if (magic != pppcp->magic_number)
			pppcp->magic_number = magic;
		else
			g_print("looped back? I should do something\n");
		break;
	case PFC:
		ppp_set_pfc(ppp, TRUE);
		break;
	case ACFC:
		ppp_set_acfc(ppp, TRUE);
		break;
	default:
		g_printerr("unhandled option %d\n", option->type);
	}
}

struct ppp_packet_handler lcp_packet_handler = {
	.proto = LCP_PROTOCOL,
	.handler = pppcp_process_packet,
};

struct pppcp_action lcp_action = {
	.this_layer_up =	lcp_up,
	.this_layer_down = 	lcp_down,
	.this_layer_started = 	lcp_started,
	.this_layer_finished =	lcp_finished,
	.option_scan = 		lcp_option_scan,
	.option_process = 	lcp_option_process,
};

void lcp_open(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* send an open event to the lcp layer */
	pppcp_generate_event(data, OPEN, NULL, 0);
}

void lcp_close(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* send a CLOSE  event to the lcp layer */
	pppcp_generate_event(data, CLOSE, NULL, 0);
}

void lcp_establish(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* send an UP event to the lcp layer */
	pppcp_generate_event(data, UP, NULL, 0);
}

void lcp_terminate(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* send a DOWN event to the lcp layer */
	pppcp_generate_event(data, DOWN, NULL, 0);
}

void lcp_free(struct pppcp_data *lcp)
{
	if (lcp == NULL)
		return;

	/* TBD unregister packet handler */

	pppcp_free(lcp);
}

struct pppcp_data * lcp_new(GAtPPP *ppp)
{
	struct pppcp_data *pppcp;
	struct ppp_option *option;
	guint16 codes = LCP_SUPPORTED_CODES;

	pppcp = pppcp_new(ppp, LCP_PROTOCOL, NULL);
	if (!pppcp) {
		g_print("Failed to allocate PPPCP struct\n");
		return NULL;
	}
	pppcp_set_valid_codes(pppcp, codes);
	pppcp->priv = pppcp;

	/* set the actions */
	pppcp->action = &lcp_action;

	/* add the default config options */
	option = g_try_malloc0(6);
	if (option == NULL) {
		pppcp_free(pppcp);
		return NULL;
	}
	option->type = ACCM;
	option->length = 6;
	pppcp_add_config_option(pppcp, option);

	/* register packet handler for LCP protocol */
	lcp_packet_handler.priv = pppcp;
	ppp_register_packet_handler(&lcp_packet_handler);
	return pppcp;
}
