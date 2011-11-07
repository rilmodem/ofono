/*
 *
 *  oFono - Open Source Telephony
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

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <errno.h>
#include <string.h>

#include <glib.h>

#include "gatppp.h"
#include "ppp.h"

#define IPV6CP_SUPPORTED_CODES	((1 << PPPCP_CODE_TYPE_CONFIGURE_REQUEST) | \
				(1 << PPPCP_CODE_TYPE_CONFIGURE_ACK) | \
				(1 << PPPCP_CODE_TYPE_CONFIGURE_NAK) | \
				(1 << PPPCP_CODE_TYPE_CONFIGURE_REJECT) | \
				(1 << PPPCP_CODE_TYPE_TERMINATE_REQUEST) | \
				(1 << PPPCP_CODE_TYPE_TERMINATE_ACK) | \
				(1 << PPPCP_CODE_TYPE_CODE_REJECT))

#define OPTION_COPY(_options, _len, _req, _type, _var, _opt_len)	\
	if (_req) {							\
		_options[_len] = _type;					\
		_options[_len + 1] = _opt_len + 2;			\
		memcpy(_options + _len + 2, _var, _opt_len);		\
		_len += _opt_len + 2;					\
	}

/* We request only IPv6 Interface Id */
#define IPV6CP_MAX_CONFIG_OPTION_SIZE	10
#define IPV6CP_MAX_FAILURE		3
#define IPV6CP_ERROR ipv6cp_error_quark()

enum ipv6cp_option_types {
	IPV6CP_INTERFACE_ID = 1,
};

struct ipv6cp_data {
	guint8 options[IPV6CP_MAX_CONFIG_OPTION_SIZE];
	guint16 options_len;
	guint8 req_options;
	guint64 local_addr;
	guint64 peer_addr;
	gboolean is_server;
};

static GQuark ipv6cp_error_quark(void)
{
	return g_quark_from_static_string("ipv6cp");
}

static void ipv6cp_generate_config_options(struct ipv6cp_data *ipv6cp)
{
	guint16 len = 0;

	OPTION_COPY(ipv6cp->options, len,
			ipv6cp->req_options & IPV6CP_INTERFACE_ID,
			IPV6CP_INTERFACE_ID, &ipv6cp->local_addr,
			sizeof(ipv6cp->local_addr));

	ipv6cp->options_len = len;
}

static void ipv6cp_reset_config_options(struct ipv6cp_data *ipv6cp)
{
	ipv6cp->req_options = IPV6CP_INTERFACE_ID;

	ipv6cp_generate_config_options(ipv6cp);
}

static void ipv6cp_up(struct pppcp_data *pppcp)
{

}

static void ipv6cp_down(struct pppcp_data *pppcp)
{
	struct ipv6cp_data *ipv6cp = pppcp_get_data(pppcp);

	ipv6cp_reset_config_options(ipv6cp);

	pppcp_set_local_options(pppcp, ipv6cp->options, ipv6cp->options_len);
}

static void ipv6cp_finished(struct pppcp_data *pppcp)
{

}

static enum rcr_result ipv6cp_server_rcr(struct ipv6cp_data *ipv6cp,
					const struct pppcp_packet *packet,
					guint8 **new_options, guint16 *new_len)
{
	struct ppp_option_iter iter;
	guint8 nak_options[IPV6CP_MAX_CONFIG_OPTION_SIZE];
	guint16 len = 0;
	guint8 *rej_options = NULL;
	guint16 rej_len = 0;
	guint64 addr;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		guint8 type = ppp_option_iter_get_type(&iter);
		const void *data = ppp_option_iter_get_data(&iter);

		switch (type) {
		case IPV6CP_INTERFACE_ID:
			memcpy(&addr, data, sizeof(addr));

			OPTION_COPY(nak_options, len,
					addr != ipv6cp->peer_addr || addr == 0,
					type, &ipv6cp->peer_addr,
					ppp_option_iter_get_length(&iter));
			break;
		default:
			if (rej_options == NULL) {
				guint16 max_len = ntohs(packet->length) - 4;
				rej_options = g_new0(guint8, max_len);
			}

			OPTION_COPY(rej_options, rej_len, rej_options != NULL,
					type, data,
					ppp_option_iter_get_length(&iter));
			break;
		}
	}

	if (rej_len > 0) {
		*new_len = rej_len;
		*new_options = rej_options;

		return RCR_REJECT;
	}

	if (len > 0) {
		*new_len = len;
		*new_options = g_memdup(nak_options, len);

		return RCR_NAK;
	}

	return RCR_ACCEPT;
}

static enum rcr_result ipv6cp_client_rcr(struct ipv6cp_data *ipv6cp,
					const struct pppcp_packet *packet,
					guint8 **new_options, guint16 *new_len)
{
	struct ppp_option_iter iter;
	guint8 *options = NULL;
	guint8 len = 0;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		guint8 type = ppp_option_iter_get_type(&iter);
		const void *data = ppp_option_iter_get_data(&iter);

		switch (type) {
		case IPV6CP_INTERFACE_ID:
			memcpy(&ipv6cp->peer_addr, data,
					sizeof(ipv6cp->peer_addr));

			if (ipv6cp->peer_addr != 0)
				break;
			/*
			 * Fall through, reject zero Interface ID
			 */
		default:
			if (options == NULL) {
				guint16 max_len = ntohs(packet->length) - 4;
				options = g_new0(guint8, max_len);
			}

			OPTION_COPY(options, len, options != NULL,
					type, data,
					ppp_option_iter_get_length(&iter));
			break;
		}
	}

	if (len > 0) {
		*new_len = len;
		*new_options = options;

		return RCR_REJECT;
	}

	return RCR_ACCEPT;
}

static enum rcr_result ipv6cp_rcr(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet,
					guint8 **new_options, guint16 *new_len)
{
	struct ipv6cp_data *ipv6cp = pppcp_get_data(pppcp);

	if (ipv6cp->is_server)
		return ipv6cp_server_rcr(ipv6cp, packet, new_options, new_len);
	else
		return ipv6cp_client_rcr(ipv6cp, packet, new_options, new_len);
}

static void ipv6cp_rca(struct pppcp_data *pppcp,
					const struct pppcp_packet *packet)
{
	struct ipv6cp_data *ipv6cp = pppcp_get_data(pppcp);
	struct ppp_option_iter iter;

	if (ipv6cp->is_server)
		return;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);

		switch (ppp_option_iter_get_type(&iter)) {
		case IPV6CP_INTERFACE_ID:
			memcpy(&ipv6cp->local_addr, data,
					sizeof(ipv6cp->local_addr));
			break;
		default:
			break;
		}
	}
}

static void ipv6cp_rcn_nak(struct pppcp_data *pppcp,
				const struct pppcp_packet *packet)
{
	struct ipv6cp_data *ipv6cp = pppcp_get_data(pppcp);
	struct ppp_option_iter iter;

	if (ipv6cp->is_server)
		return;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		const guint8 *data = ppp_option_iter_get_data(&iter);

		switch (ppp_option_iter_get_type(&iter)) {
		case IPV6CP_INTERFACE_ID:
			ipv6cp->req_options |= IPV6CP_INTERFACE_ID;
			memcpy(&ipv6cp->local_addr, data,
				sizeof(ipv6cp->local_addr));
			break;
		default:
			break;
		}
	}

	ipv6cp_generate_config_options(ipv6cp);
	pppcp_set_local_options(pppcp, ipv6cp->options, ipv6cp->options_len);
}

static void ipv6cp_rcn_rej(struct pppcp_data *pppcp,
				const struct pppcp_packet *packet)
{
	struct ipv6cp_data *ipv6cp = pppcp_get_data(pppcp);
	struct ppp_option_iter iter;

	ppp_option_iter_init(&iter, packet);

	while (ppp_option_iter_next(&iter) == TRUE) {
		switch (ppp_option_iter_get_type(&iter)) {
		case IPV6CP_INTERFACE_ID:
			ipv6cp->req_options &= ~IPV6CP_INTERFACE_ID;
			break;
		default:
			break;
		}
	}

	ipv6cp_generate_config_options(ipv6cp);
	pppcp_set_local_options(pppcp, ipv6cp->options, ipv6cp->options_len);
}

struct pppcp_proto ipv6cp_proto = {
	.proto			= IPV6CP_PROTO,
	.name			= "ipv6cp",
	.supported_codes	= IPV6CP_SUPPORTED_CODES,
	.this_layer_up		= ipv6cp_up,
	.this_layer_down	= ipv6cp_down,
	.this_layer_finished	= ipv6cp_finished,
	.rca			= ipv6cp_rca,
	.rcn_nak		= ipv6cp_rcn_nak,
	.rcn_rej		= ipv6cp_rcn_rej,
	.rcr			= ipv6cp_rcr,
};

struct pppcp_data *ipv6cp_new(GAtPPP *ppp, gboolean is_server,
					const char *local, const char *peer,
					GError **error)
{
	struct ipv6cp_data *ipv6cp;
	struct pppcp_data *pppcp;
	struct in6_addr local_addr;
	struct in6_addr peer_addr;

	if (local == NULL)
		memset(&local_addr, 0, sizeof(local_addr));
	else if (inet_pton(AF_INET6, local, &local_addr) != 1) {
		g_set_error(error, IPV6CP_ERROR, errno,
				"Unable to set local Interface ID: %s",
				strerror(errno));
		return NULL;
	}

	if (peer == NULL)
		memset(&peer_addr, 0, sizeof(peer_addr));
	else if (inet_pton(AF_INET6, peer, &peer_addr) != 1) {
		g_set_error(error, IPV6CP_ERROR, errno,
				"Unable to set peer Interface ID: %s",
				g_strerror(errno));
		return NULL;
	}

	ipv6cp = g_try_new0(struct ipv6cp_data, 1);
	if (ipv6cp == NULL)
		return NULL;

	pppcp = pppcp_new(ppp, &ipv6cp_proto, FALSE, IPV6CP_MAX_FAILURE);
	if (pppcp == NULL) {
		g_free(ipv6cp);
		return NULL;
	}

	memcpy(&ipv6cp->local_addr, &local_addr.s6_addr[8],
						sizeof(ipv6cp->local_addr));
	memcpy(&ipv6cp->peer_addr, &peer_addr.s6_addr[8],
						sizeof(ipv6cp->peer_addr));
	ipv6cp->is_server = is_server;

	pppcp_set_data(pppcp, ipv6cp);

	ipv6cp_reset_config_options(ipv6cp);

	pppcp_set_local_options(pppcp, ipv6cp->options, ipv6cp->options_len);

	return pppcp;
}

void ipv6cp_free(struct pppcp_data *data)
{
	struct ipv6cp_data *ipv6cp = pppcp_get_data(data);

	g_free(ipv6cp);
	pppcp_free(data);
}
