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

struct pap_header {
	guint8 code;
	guint8 identifier;
	guint16 length;
	guint8 data[0];
} __attribute__((packed));

struct ppp_pap {
	GAtPPP *ppp;
	struct ppp_header *authreq;
	guint16 authreq_len;
	guint retry_timer;
	guint retries;
};

enum pap_code {
	PAP_REQUEST = 1,
	PAP_ACK,
	PAP_NAK
};

/*
 * RFC 1334 2.1.1:
 *   The Authenticate-Request packet MUST be repeated until a valid
 *   reply packet is received, or an optional retry counter expires.
 *
 * If we don't get a reply after this many attempts, we can safely
 * assume we're never going to get one.
 */
#define PAP_MAX_RETRY	3  /* attempts */
#define PAP_TIMEOUT	10 /* seconds */

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

void ppp_pap_process_packet(struct ppp_pap *pap, const guint8 *new_packet,
				gsize len)
{
	guint8 code;

	if (len < sizeof(struct pap_header))
		return;

	code = new_packet[0];

	switch (code) {
	case PAP_ACK:
		g_source_remove(pap->retry_timer);
		pap->retry_timer = 0;
		ppp_auth_notify(pap->ppp, TRUE);
		break;
	case PAP_NAK:
		g_source_remove(pap->retry_timer);
		pap->retry_timer = 0;
		ppp_auth_notify(pap->ppp, FALSE);
		break;
	default:
		break;
	}
}

static gboolean ppp_pap_timeout(gpointer user_data)
{
	struct ppp_pap *pap = (struct ppp_pap *)user_data;
	struct pap_header *authreq;

	if (++pap->retries >= PAP_MAX_RETRY) {
		pap->retry_timer = 0;
		ppp_auth_notify(pap->ppp, FALSE);
		return FALSE;
	}

	/*
	 * RFC 1334 2.2.1:
	 * The Identifier field MUST be changed each time an
	 * Authenticate-Request packet is issued.
	 */
	authreq = (struct pap_header *)&pap->authreq->info;
	authreq->identifier++;

	ppp_transmit(pap->ppp, (guint8 *)pap->authreq, pap->authreq_len);

	return TRUE;
}

gboolean ppp_pap_start(struct ppp_pap *pap)
{
	struct pap_header *authreq;
	struct ppp_header *packet;
	const char *username = g_at_ppp_get_username(pap->ppp);
	const char *password = g_at_ppp_get_password(pap->ppp);
	guint16 length;

	length = sizeof(*authreq) + strlen(username) + strlen(password) + 2;

	packet = ppp_packet_new(length, PAP_PROTOCOL);
	if (packet == NULL)
		return FALSE;

	pap->authreq = packet;
	pap->authreq_len = length;

	authreq = (struct pap_header *)&packet->info;
	authreq->code = PAP_REQUEST;
	authreq->identifier = 1;
	authreq->length = htons(length);

	authreq->data[0] = (unsigned char) strlen(username);
	memcpy(authreq->data + 1, username, strlen(username));
	authreq->data[strlen(username) + 1] = (unsigned char)strlen(password);
	memcpy(authreq->data + 1 + strlen(username) + 1, password,
					strlen(password));

	/* Transmit the packet and schedule a retry. */
	ppp_transmit(pap->ppp, (guint8 *)packet, length);
	pap->retries = 0;
	pap->retry_timer = g_timeout_add_seconds(PAP_TIMEOUT,
							ppp_pap_timeout, pap);

	return TRUE;
}

void ppp_pap_free(struct ppp_pap *pap)
{
	if (pap->retry_timer != 0)
		g_source_remove(pap->retry_timer);

	if (pap->authreq != NULL)
		g_free(pap->authreq);

	g_free(pap);
}

struct ppp_pap *ppp_pap_new(GAtPPP *ppp)
{
	struct ppp_pap *pap;

	pap = g_try_new0(struct ppp_pap, 1);
	if (pap == NULL)
		return NULL;

	pap->ppp = ppp;

	return pap;
}
