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
#include <arpa/inet.h>

#include <glib.h>

#include "gatppp.h"
#include "ppp.h"

struct chap_header {
	guint8 code;
	guint8 identifier;
	guint16 length;
	guint8 data[0];
} __attribute__((packed));

struct chap_data {
	guint8 method;
	struct auth_data *auth;
};

enum chap_code {
	CHALLENGE=1,
	RESPONSE,
	SUCCESS,
	FAILURE
};

void auth_set_credentials(struct auth_data *data, const char *username,
				const char *password)
{
	if (data == NULL)
		return;

	g_free(data->username);
	data->username = g_strdup(username);

	g_free(data->password);
	data->password = g_strdup(password);
}

static void chap_process_challenge(struct auth_data *auth, guint8 *packet)
{
	struct chap_header *header = (struct chap_header *) packet;
	struct chap_header *response;
	struct chap_data *data = auth->proto_data;
	GChecksum *checksum;
	gchar *secret = data->auth->password;
	guint16 response_length;
	struct ppp_header *ppp_packet;
	gsize digest_len;

	/* create a checksum over id, secret, and challenge */
	checksum = g_checksum_new(data->method);
	if (!checksum)
		return;
	g_checksum_update(checksum, &header->identifier, 1);
	g_checksum_update(checksum, (guchar *) secret, strlen(secret));
	g_checksum_update(checksum, &header->data[1], header->data[0]);

	/* transmit a response packet */
	/*
	 * allocate space for the header, the checksum, and the ppp header,
	 * and the value size byte
	 */
	digest_len = g_checksum_type_get_length(data->method);
	response_length = digest_len + sizeof(*header) + 1;
	ppp_packet = g_try_malloc0(response_length + 2);
	if (!ppp_packet)
		goto challenge_out;

	/* add our protocol information */
	ppp_packet->proto = htons(CHAP_PROTOCOL);
	response = (struct chap_header *) &ppp_packet->info;
	if (response) {
		response->code = RESPONSE;
		response->identifier = header->identifier;
		response->length = htons(response_length);
		response->data[0] = digest_len;
		g_checksum_get_digest(checksum, &response->data[1],
					(gsize *) &response->data[0]);
		/* leave the name empty? */
	}

	/* transmit the packet */
	ppp_transmit(auth->ppp, (guint8 *) ppp_packet, response_length);

challenge_out:
	g_checksum_free(checksum);
}

static void chap_process_success(struct auth_data *data, guint8 *packet)
{
	ppp_generate_event(data->ppp, PPP_SUCCESS);
}

static void chap_process_failure(struct auth_data *data, guint8 *packet)
{
	struct chap_header *header = (struct chap_header *) packet;

	g_print("Failed to authenticate, message %s\n", header->data);
}

/*
 * parse the packet
 */
static void chap_process_packet(gpointer priv, guint8 *new_packet)
{
	struct auth_data *data = priv;
	guint8 code = new_packet[0];

	switch (code) {
	case CHALLENGE:
		chap_process_challenge(data, new_packet);
		break;
	case RESPONSE:
		g_print("Oops, received RESPONSE, but I've not implemented\n");
		break;
	case SUCCESS:
		chap_process_success(data, new_packet);
		break;
	case FAILURE:
		chap_process_failure(data, new_packet);
		break;
	default:
		g_print("Unknown auth code\n");
		break;
	}
}

struct ppp_packet_handler chap_packet_handler = {
	.proto = CHAP_PROTOCOL,
	.handler = chap_process_packet,
};

static void chap_free(struct auth_data *auth)
{
	/* TBD unregister protocol handler */

	g_free(auth->proto_data);
}

static struct chap_data *chap_new(struct auth_data *auth, guint8 method)
{
	struct chap_data *data;

	data = g_try_malloc0(sizeof(*data));
	if (!data)
		return NULL;

	data->auth = auth;
	switch (method) {
	case MD5:
		data->method = G_CHECKSUM_MD5;
		break;
	default:
		g_print("Unknown method\n");
	}

	/* register packet handler for CHAP protocol */
	chap_packet_handler.priv = auth;
	ppp_register_packet_handler(&chap_packet_handler);
	return data;
}

void auth_set_proto(struct auth_data *data, guint16 proto, guint8 method)
{
	if (data == NULL)
		return;

	switch (proto) {
	case CHAP_PROTOCOL:
		data->proto_data = (gpointer) chap_new(data, method);
		break;
	default:
		g_print("Unknown auth protocol 0x%x\n", proto);
	}
}

void auth_free(struct auth_data *data)
{
	if (data == NULL)
		return;

	chap_free(data);
	g_free(data);
}

struct auth_data *auth_new(GAtPPP *ppp)
{
	struct auth_data *data;

	data = g_try_malloc0(sizeof(*data));
	if (!data)
		return NULL;

	data->ppp = ppp;
	return data;
}
