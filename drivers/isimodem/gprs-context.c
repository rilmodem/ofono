/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/uio.h>
#include <search.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include <gisi/client.h>
#include <gisi/iter.h>
#include <gisi/pep.h>
#include <gisi/pipe.h>

#include "isimodem.h"
#include "isiutil.h"
#include "gpds.h"
#include "debug.h"

#define STATIC_IP_NETMASK "255.255.255.255"

#define INVALID_ID (0xff)
# if (INVALID_ID < GPDS_MAX_CONTEXT_COUNT)
#   error Uho! This should not happen!
#endif

struct context_data {
	GIsiClient *client;
	GIsiModem *idx;
	uint16_t gpds;	/* GPDS object handle */
	unsigned cid;	/* oFono core context ID */
	struct ofono_gprs_context *context;
	ofono_gprs_context_cb_t cb;
	void *data;

	GIsiPEP *pep;
	GIsiPipe *pipe;
	guint reset;

	char apn[GPDS_MAX_APN_STRING_LENGTH + 1];
	char username[GPDS_MAX_USERNAME_LENGTH + 1];
	char password[GPDS_MAX_PASSWORD_LENGTH + 1];

	uint8_t handle;	/* GPDS context ID */
	uint8_t type;
};

static gboolean client_reset(gpointer data)
{
	struct context_data *cd = data;

	g_isi_client_reset(cd->client);
	cd->reset = 0;

	return FALSE;
}

static void reset_context(struct context_data *cd)
{
	if (cd == NULL)
		return;

	if (cd->pipe)
		g_isi_pipe_destroy(cd->pipe);

	if (cd->pep)
		g_isi_pep_destroy(cd->pep);

	cd->pep = NULL;
	cd->pipe = NULL;
	cd->handle = INVALID_ID;

	cd->reset = g_idle_add(client_reset, cd);
}

typedef void (*ContextFailFunc)(struct context_data *cd);

static void gprs_up_fail(struct context_data *cd)
{
	reset_context(cd);
	CALLBACK_WITH_FAILURE(cd->cb, cd->data);
}

static void gprs_down_fail(struct context_data *cd)
{
	reset_context(cd);
	CALLBACK_WITH_FAILURE(cd->cb, cd->data);
}

static gboolean check_resp(const GIsiMessage *msg, uint8_t id, size_t minlen,
				struct context_data *cd,
				ContextFailFunc fail_cb)
{
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0) {
		DBG("ISI message error: %d", g_isi_msg_error(msg));
		goto error;
	}

	if (g_isi_msg_id(msg) != id)
		return FALSE;

	if (g_isi_msg_data_len(msg) < minlen) {
		DBG("truncated message");
		goto error;
	}

	if (cd->handle != INVALID_ID && data[0] != cd->handle)
		return FALSE;

	if (data[1] != GPDS_OK) {
		DBG("context error: %s (0x%02"PRIx8")",
			gpds_status_name(data[1]), data[1]);

		if (minlen > 2)
			DBG("  fail cause: %s (0x%02"PRIx8")",
				gpds_isi_cause_name(data[2]), data[2]);

		goto error;
	}

	return TRUE;

error:
	if (fail_cb)
		fail_cb(cd);

	return FALSE;
}

static gboolean check_ind(const GIsiMessage *msg, size_t minlen,
				struct context_data *cd)

{
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0) {
		DBG("ISI message error: %d", g_isi_msg_error(msg));
		return FALSE;
	}

	if (g_isi_msg_data_len(msg) < minlen) {
		DBG("truncated message");
		return FALSE;
	}

	if (cd->handle != INVALID_ID && data[0] != cd->handle)
		return FALSE;

	return TRUE;
}

static void deactivate_ind_cb(const GIsiMessage *msg, void *opaque)
{
	struct context_data *cd = opaque;
	const uint8_t *data = g_isi_msg_data(msg);

	if (!check_ind(msg, 2, cd))
		return;

	DBG("context deactivated: %s (0x%02"PRIx8")",
		gpds_isi_cause_name(data[2]), data[2]);

	ofono_gprs_context_deactivated(cd->context, cd->cid);
	reset_context(cd);
}

static void activate_ind_cb(const GIsiMessage *msg, void *opaque)
{
	struct context_data *cd = opaque;
	GIsiSubBlockIter iter;
	const char *dns[5];
	int dns_count = 0;

	char ifname[IF_NAMESIZE];
	char *ip_addr = NULL;
	char *ipv6_addr = NULL;

	if (!check_ind(msg, 2, cd))
		return;

	for (g_isi_sb_iter_init(&iter, msg, 2);
			g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		uint8_t *addr_value = NULL;
		uint8_t addr_len = 0;

		switch (g_isi_sb_iter_get_id(&iter)) {

		case GPDS_PDP_ADDRESS_INFO:

			if (!g_isi_sb_iter_get_byte(&iter, &addr_len, 3))
				goto error;

			if (!g_isi_sb_iter_get_data(&iter, (void *)&addr_value,
							4))
				goto error;

			if (addr_len == 4) {
				ip_addr = alloca(INET_ADDRSTRLEN);
				inet_ntop(AF_INET, (const void *)addr_value,
						ip_addr, INET_ADDRSTRLEN);
			} else if (addr_len == 16) {
				ipv6_addr = alloca(INET6_ADDRSTRLEN);
				inet_ntop(AF_INET6, (const void *)addr_value,
						ipv6_addr, INET6_ADDRSTRLEN);
			}
			break;

		case GPDS_PDNS_ADDRESS_INFO:

			if (!g_isi_sb_iter_get_byte(&iter, &addr_len, 3))
				break;

			if (!g_isi_sb_iter_get_data(&iter, (void *)&addr_value,
							4))
				break;

			if (addr_len == 4) {
				char *addr = alloca(INET_ADDRSTRLEN);
				inet_ntop(AF_INET, (const void *)addr_value,
						addr, INET_ADDRSTRLEN);
				dns[dns_count++] = addr;
			} else if (addr_len == 16) {
				char *addr = alloca(INET6_ADDRSTRLEN);
				inet_ntop(AF_INET6, (const void *)addr_value,
						addr, INET6_ADDRSTRLEN);
				dns[dns_count++] = addr;
			}
			break;

		case GPDS_SDNS_ADDRESS_INFO:

			if (!g_isi_sb_iter_get_byte(&iter, &addr_len, 3))
				break;

			if (!g_isi_sb_iter_get_data(&iter, (void *)&addr_value,
							4))
				break;

			if (addr_len == 4) {
				char *addr = alloca(INET_ADDRSTRLEN);
				inet_ntop(AF_INET, (const void *)addr_value,
						addr, INET_ADDRSTRLEN);
				dns[dns_count++] = addr;
			} else if (addr_len == 16) {
				char *addr = alloca(INET6_ADDRSTRLEN);
				inet_ntop(AF_INET6, (const void *)addr_value,
						addr, INET6_ADDRSTRLEN);
				dns[dns_count++] = addr;
			}
			break;

		default:
			DBG("skipped sub-block: %s (%zu bytes)",
				gpds_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
		}
	}

	if (!g_isi_pep_get_ifname(cd->pep, ifname))
		goto error;

	dns[dns_count] = 0;

	ofono_gprs_context_set_interface(cd->context, ifname);

	if (ip_addr != NULL) {
		ofono_gprs_context_set_ipv4_address(cd->context, ip_addr, TRUE);
		ofono_gprs_context_set_ipv4_netmask(cd->context,
							STATIC_IP_NETMASK);
		ofono_gprs_context_set_ipv4_dns_servers(cd->context, dns);
	} else if (ipv6_addr != NULL) {
		ofono_gprs_context_set_ipv6_address(cd->context, ipv6_addr);
		ofono_gprs_context_set_ipv6_dns_servers(cd->context, dns);
	}

	CALLBACK_WITH_SUCCESS(cd->cb, cd->data);
	return;

error:
	gprs_up_fail(cd);
}

static void context_activate_cb(const GIsiMessage *msg, void *cd)
{
	check_resp(msg, GPDS_CONTEXT_ACTIVATE_RESP, 6, cd, gprs_up_fail);
}

static void send_context_activate(GIsiClient *client, void *opaque)
{
	struct context_data *cd = opaque;

	const unsigned char msg[] = {
		GPDS_CONTEXT_ACTIVATE_REQ,
		cd->handle,	/* context ID */
		0,		/* sub blocks */
	};

	g_isi_client_ind_subscribe(client, GPDS_CONTEXT_ACTIVATE_IND,
				activate_ind_cb, cd);
	g_isi_client_ind_subscribe(client, GPDS_CONTEXT_DEACTIVATE_IND,
				deactivate_ind_cb, cd);

	if (g_isi_client_send_with_timeout(client, msg, sizeof(msg),
				GPDS_CTX_ACTIVATE_TIMEOUT,
				context_activate_cb, cd, NULL))
		g_isi_pipe_start(cd->pipe);
	else
		gprs_up_fail(cd);
}

static void context_auth_cb(const GIsiMessage *msg, void *opaque)
{
	struct context_data *cd = opaque;

	if (!check_resp(msg, GPDS_CONTEXT_AUTH_RESP, 2, cd, gprs_up_fail))
		return;

	send_context_activate(cd->client, cd);
}

static void send_context_authenticate(GIsiClient *client, void *opaque)
{
	struct context_data *cd = opaque;
	size_t username_len = strlen(cd->username);
	size_t password_len = strlen(cd->password);

	/* Pad the fields to the next 32bit boundary */
	size_t sb_userinfo_len = ALIGN4(3 + username_len);
	size_t userinfo_pad_len = sb_userinfo_len - (3 + username_len);

	size_t sb_password_info_len = ALIGN4(3 + password_len);
	size_t password_pad_len = sb_password_info_len - (3 + password_len);

	const uint8_t padding[4] = { 0 };

	const uint8_t top[] = {
		GPDS_CONTEXT_AUTH_REQ,
		cd->handle,
		2,	/* sub blocks */
		GPDS_USER_NAME_INFO,
		sb_userinfo_len,
		username_len,
		/* Username goes here */
		/* Possible padding goes here */
	};

	const uint8_t bottom[] = {
		GPDS_PASSWORD_INFO,
		sb_password_info_len,
		password_len,
		/* Password goes here */
		/* Possible padding goes here */
	};

	const struct iovec iov[6] = {
		{ (uint8_t *) top, sizeof(top) },
		{ cd->username, username_len },
		{ (uint8_t *) padding, userinfo_pad_len },
		{ (uint8_t *) bottom, sizeof(bottom) },
		{ cd->password, password_len },
		{ (uint8_t *) padding, password_pad_len },
	};

	if (!g_isi_client_vsend(client, iov, 6, context_auth_cb, cd, NULL))
		gprs_up_fail(cd);
}

static void context_conf_cb(const GIsiMessage *msg, void *opaque)
{
	struct context_data *cd = opaque;

	if (!check_resp(msg, GPDS_CONTEXT_CONFIGURE_RESP, 2, cd, gprs_up_fail))
		return;

	if (cd->username[0] != '\0')
		send_context_authenticate(cd->client, cd);
	else
		send_context_activate(cd->client, cd);
}

static void link_conf_cb(const GIsiMessage *msg, void *opaque)
{
	struct context_data *cd = opaque;
	size_t apn_len = strlen(cd->apn);
	size_t sb_apn_info_len = ALIGN4(3 + apn_len);
	size_t apn_pad_len = sb_apn_info_len - (3 + apn_len);

	const uint8_t padding[4] = { 0 };

	const uint8_t req[] = {
		GPDS_CONTEXT_CONFIGURE_REQ,
		cd->handle,	/* context ID */
		cd->type,	/* PDP type */
		GPDS_CONT_TYPE_NORMAL,
		cd->handle,	/* primary context ID */
		0x00,		/* filler */
		2,		/* sub blocks */
		GPDS_DNS_ADDRESS_REQ_INFO,
		4,		/* subblock length */
		0, 0,		/* padding */
		GPDS_APN_INFO,
		sb_apn_info_len,
		apn_len,
		/* Possible padding goes here */
	};

	const struct iovec iov[3] = {
		{ (uint8_t *) req, sizeof(req) },
		{ cd->apn, apn_len },
		{ (uint8_t *) padding, apn_pad_len },
	};

	if (!check_resp(msg, GPDS_LL_CONFIGURE_RESP, 2, cd, gprs_up_fail))
		return;

	if (!g_isi_client_vsend(cd->client, iov, 3, context_conf_cb, cd, NULL))
		gprs_up_fail(cd);
}

static void create_context_cb(const GIsiMessage *msg, void *opaque)
{
	struct context_data *cd = opaque;
	const uint8_t *data = g_isi_msg_data(msg);

	uint8_t req[] = {
		GPDS_LL_CONFIGURE_REQ,
		0x00,		/* GPDS context ID, added later */
		g_isi_pipe_get_handle(cd->pipe),
		GPDS_LL_PLAIN,	/* link type */
	};

	if (!check_resp(msg, GPDS_CONTEXT_ID_CREATE_RESP, 2, cd, gprs_up_fail))
		return;

	cd->handle = req[1] = data[0];

	if (!g_isi_client_send(cd->client, req, sizeof(req), link_conf_cb,
				cd, NULL))
		gprs_up_fail(cd);
}

static void create_pipe_cb(GIsiPipe *pipe)
{
	struct context_data *cd = g_isi_pipe_get_userdata(pipe);

	const uint8_t msg[] = {
		GPDS_CONTEXT_ID_CREATE_REQ,
	};

	if (!g_isi_client_send(cd->client, msg, sizeof(msg), create_context_cb,
				cd, NULL))
		gprs_up_fail(cd);
}

static void isi_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct context_data *cd = ofono_gprs_context_get_data(gc);

	DBG("activate: gpds = 0x%04x", cd->gpds);

	if (cd == NULL || !cd->gpds) {
		/* GPDS is not reachable */
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	if (cd->reset) {
		g_isi_client_reset(cd->client);
		g_source_remove(cd->reset);
		cd->reset = 0;
	}

	cd->cid = ctx->cid;
	cd->cb = cb;
	cd->data = data;
	cd->pep = NULL;
	cd->pipe = NULL;
	cd->handle = INVALID_ID;

	switch (ctx->proto) {
	case OFONO_GPRS_PROTO_IP:
		cd->type = GPDS_PDP_TYPE_IPV4;
		break;

	case OFONO_GPRS_PROTO_IPV6:
		cd->type = GPDS_PDP_TYPE_IPV6;
		break;

	case OFONO_GPRS_PROTO_IPV4V6:
		/* Not supported by modem */
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	if (strlen(ctx->apn) >= GPDS_MAX_APN_STRING_LENGTH
			|| strlen(ctx->username) >= GPDS_MAX_USERNAME_LENGTH
			|| strlen(ctx->password) >= GPDS_MAX_PASSWORD_LENGTH)
		goto error;

	strncpy(cd->apn, ctx->apn, GPDS_MAX_APN_STRING_LENGTH);
	cd->apn[GPDS_MAX_APN_STRING_LENGTH] = '\0';

	strncpy(cd->username, ctx->username, GPDS_MAX_USERNAME_LENGTH);
	cd->username[GPDS_MAX_USERNAME_LENGTH] = '\0';

	strncpy(cd->password, ctx->password, GPDS_MAX_PASSWORD_LENGTH);
	cd->username[GPDS_MAX_PASSWORD_LENGTH] = '\0';

	cd->pep = g_isi_pep_create(cd->idx, NULL, NULL);
	if (cd->pep == NULL)
		goto error;

	cd->pipe = g_isi_pipe_create(cd->idx, create_pipe_cb,
					g_isi_pep_get_object(cd->pep),
					cd->gpds, PN_PEP_TYPE_GPRS,
					PN_PEP_TYPE_GPRS);
	if (cd->pipe == NULL)
		goto error;

	g_isi_pipe_set_userdata(cd->pipe, cd);
	return;

error:
	gprs_up_fail(cd);
}

static void context_deactivate_cb(const GIsiMessage *msg, void *opaque)
{
	struct context_data *cd = opaque;

	if (!check_resp(msg, GPDS_CONTEXT_DEACTIVATE_RESP, 2, cd,
			gprs_down_fail))
		return;

	CALLBACK_WITH_SUCCESS(cd->cb, cd->data);
	reset_context(cd);
}

static void isi_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct context_data *cd = ofono_gprs_context_get_data(gc);

	unsigned char msg[] = {
		GPDS_CONTEXT_DEACTIVATE_REQ,
		0x00,	/* GPDS context ID, added later */
	};

	if (cd == NULL)
		return;

	cd->cb = cb;
	cd->data = data;

	msg[1] = cd->handle;

	if (!g_isi_client_send_with_timeout(cd->client, msg, sizeof(msg),
				GPDS_CTX_DEACTIVATE_TIMEOUT,
				context_deactivate_cb, cd, NULL))
		gprs_down_fail(cd);
}

static void gpds_ctx_reachable_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_gprs_context *gc = opaque;
	struct context_data *cd = ofono_gprs_context_get_data(gc);

	if (g_isi_msg_error(msg) < 0) {
		DBG("unable to bootstrap gprs context driver");
		ofono_gprs_context_remove(gc);
		return;
	}

	cd->gpds = g_isi_msg_object(msg);
}

static int isi_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct context_data *cd = g_try_new0(struct context_data, 1);

	if (cd == NULL)
		return -ENOMEM;

	cd->client = g_isi_client_create(idx, PN_GPDS);
	if (cd->client == NULL) {
		g_free(cd);
		return -ENOMEM;
	}

	cd->idx = idx;
	cd->context = gc;
	ofono_gprs_context_set_data(gc, cd);

	g_isi_client_verify(cd->client, gpds_ctx_reachable_cb, gc, NULL);

	return 0;
}

static void isi_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct context_data *cd = ofono_gprs_context_get_data(gc);

	ofono_gprs_context_set_data(gc, NULL);

	if (cd == NULL)
		return;

	if (cd->reset)
		g_source_remove(cd->reset);

	if (cd->pipe != NULL)
		g_isi_pipe_destroy(cd->pipe);

	if (cd->pep != NULL)
		g_isi_pep_destroy(cd->pep);

	g_isi_client_destroy(cd->client);
	g_free(cd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "isimodem",
	.probe			= isi_gprs_context_probe,
	.remove			= isi_gprs_context_remove,
	.activate_primary	= isi_gprs_activate_primary,
	.deactivate_primary	= isi_gprs_deactivate_primary,
};

void isi_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void isi_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
