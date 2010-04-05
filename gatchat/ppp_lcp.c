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

#define LCP_SUPPORTED_CODES	((1 << PPPCP_CODE_TYPE_CONFIGURE_REQUEST) | \
				(1 << PPPCP_CODE_TYPE_CONFIGURE_ACK) | \
				(1 << PPPCP_CODE_TYPE_CONFIGURE_NAK) | \
				(1 << PPPCP_CODE_TYPE_CONFIGURE_REJECT) | \
				(1 << PPPCP_CODE_TYPE_TERMINATE_REQUEST) | \
				(1 << PPPCP_CODE_TYPE_TERMINATE_ACK) | \
				(1 << PPPCP_CODE_TYPE_CODE_REJECT) | \
				(1 << PPPCP_CODE_TYPE_PROTOCOL_REJECT) | \
				(1 << PPPCP_CODE_TYPE_ECHO_REQUEST) | \
				(1 << PPPCP_CODE_TYPE_ECHO_REPLY) | \
				(1 << PPPCP_CODE_TYPE_DISCARD_REQUEST))

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
	/* XXX should implement a way to signal NCP */
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
	ppp_generate_event(pppcp->ppp, PPP_DOWN);
}

/*
 * Scan the option to see if it is acceptable, unacceptable, or rejected
 *
 * We need to use a default case here because this option type value
 * could be anything.
 */
static guint lcp_option_scan(struct pppcp_data *pppcp,
						struct ppp_option *option)
{
	switch (option->type) {
	case ACCM:
	case AUTH_PROTO:
		/* XXX check to make sure it's a proto we recognize */
	case MAGIC_NUMBER:
	case PFC:
	case ACFC:
		return OPTION_ACCEPT;
	}

	return OPTION_REJECT;
}

/*
 * act on an acceptable option
 *
 * We need to use a default case here because this option type value
 * could be anything.
 */
static void lcp_option_process(struct pppcp_data *pppcp,
						struct ppp_option *option)
{
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
		break;
	}
}

static const char *lcp_option_strings[256] = {
	[0]	= "Vendor Specific",
	[1]	= "Maximum-Receive-Unit",
	[2]	= "Async-Control-Character-Map",
	[3]	= "Authentication-Protocol",
	[4]	= "Quality-Protocol",
	[5]	= "Magic-Number",
	[6]	= "Quality-Protocol (deprecated)",
	[7]	= "Protocol-Field-Compression",
	[8]	= "Address-and-Control-Field-Compression",
	[9]	= "FCS-Alternatives",
	[10]	= "Self-Describing-Pad",
	[11]	= "Numbered-Mode",
	[12]	= "Multi-Link-Procedure (deprecated)",
	[13]	= "Callback",
};

struct pppcp_action lcp_action = {
	.this_layer_up		= lcp_up,
	.this_layer_down	= lcp_down,
	.this_layer_started	= lcp_started,
	.this_layer_finished	= lcp_finished,
	.option_scan		= lcp_option_scan,
	.option_process		= lcp_option_process,
};

void lcp_open(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* send an open event to the lcp layer */
	pppcp_signal_open(data);
}

void lcp_establish(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* send an UP event to the lcp layer */
	pppcp_signal_up(data);
}

void lcp_terminate(struct pppcp_data *data)
{
	if (data == NULL)
		return;

	/* send a CLOSE event to the lcp layer */
	pppcp_signal_close(data);
}

void lcp_free(struct pppcp_data *lcp)
{
	if (lcp == NULL)
		return;

	pppcp_free(lcp);
}

void lcp_protocol_reject(struct pppcp_data *lcp, guint8 *packet, gsize len)
{
	pppcp_send_protocol_reject(lcp, packet, len);
}

struct pppcp_data *lcp_new(GAtPPP *ppp)
{
	struct pppcp_data *pppcp;
	struct ppp_option *option;

	pppcp = pppcp_new(ppp, LCP_PROTOCOL);
	if (!pppcp) {
		g_print("Failed to allocate PPPCP struct\n");
		return NULL;
	}

	pppcp->option_strings = lcp_option_strings;
	pppcp->prefix = "lcp";

	pppcp_set_valid_codes(pppcp, LCP_SUPPORTED_CODES);

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

	return pppcp;
}
