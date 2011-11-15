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

struct	ppp_chap {
	guint8 method;
	GAtPPP *ppp;
};

enum chap_code {
	CHALLENGE = 1,
	RESPONSE,
	SUCCESS,
	FAILURE
};

static void chap_process_challenge(struct ppp_chap *chap, const guint8 *packet)
{
	const struct chap_header *header = (const struct chap_header *) packet;
	struct chap_header *response;
	GChecksum *checksum;
	const char *secret = g_at_ppp_get_password(chap->ppp);
	const char *username = g_at_ppp_get_username(chap->ppp);
	guint16 response_length;
	struct ppp_header *ppp_packet;
	gsize digest_len;

	/* create a checksum over id, secret, and challenge */
	checksum = g_checksum_new(chap->method);
	if (checksum == NULL)
		return;

	g_checksum_update(checksum, &header->identifier, 1);

	if (secret)
		g_checksum_update(checksum, (guchar *) secret, strlen(secret));

	g_checksum_update(checksum, &header->data[1], header->data[0]);

	/* transmit a response packet */
	/*
	 * allocate space for the header, the checksum, and the ppp header,
	 * and the value size byte
	 */
	digest_len = g_checksum_type_get_length(chap->method);
	response_length = digest_len + sizeof(*header) + 1;

	if (username != NULL)
		response_length += strlen(username);

	ppp_packet = ppp_packet_new(response_length, CHAP_PROTOCOL);
	if (ppp_packet == NULL)
		goto challenge_out;

	response = (struct chap_header *) &ppp_packet->info;
	if (response) {
		response->code = RESPONSE;
		response->identifier = header->identifier;
		response->length = htons(response_length);
		g_checksum_get_digest(checksum, response->data + 1,
							&digest_len);
		response->data[0] = digest_len;
		/* leave the name empty? */
	}

	if (username != NULL)
		memcpy(response->data + digest_len + 1, username,
				strlen(username));

	/* transmit the packet */
	ppp_transmit(chap->ppp, (guint8 *) ppp_packet, response_length);
	g_free(ppp_packet);

challenge_out:
	g_checksum_free(checksum);
}

/*
 * parse the packet
 */
void ppp_chap_process_packet(struct ppp_chap *chap, const guint8 *new_packet,
				gsize len)
{
	guint8 code;

	if (len < sizeof(struct chap_header))
		return;

	code = new_packet[0];

	switch (code) {
	case CHALLENGE:
		chap_process_challenge(chap, new_packet);
		break;
	case RESPONSE:
		break;
	case SUCCESS:
		ppp_auth_notify(chap->ppp, TRUE);
		break;
	case FAILURE:
		ppp_auth_notify(chap->ppp, FALSE);
		break;
	default:
		break;
	}
}

void ppp_chap_free(struct ppp_chap *chap)
{
	g_free(chap);
}

struct ppp_chap *ppp_chap_new(GAtPPP *ppp, guint8 method)
{
	struct ppp_chap *chap;

	if (method != MD5)
		return NULL;

	chap = g_try_new0(struct ppp_chap, 1);
	if (chap == NULL)
		return NULL;

	chap->ppp = ppp;
	chap->method = G_CHECKSUM_MD5;

	return chap;
}
