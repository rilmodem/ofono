/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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

struct gprs_context_data {
	GIsiClient *client;
	GIsiModem *idx;
	uint16_t gpds;	/* GPDS object handle */
	GSList *contexts;
};

struct context_data {
	unsigned cid;	/* oFono core context ID */
	struct ofono_gprs_context *driver;
	union {
		ofono_gprs_context_up_cb_t up_cb;
		ofono_gprs_context_cb_t down_cb;
	};
	void *data;

	GIsiPEP *pep;
	GIsiPipe *pipe;

	char apn[GPDS_MAX_APN_STRING_LENGTH + 1];
	char username[GPDS_MAX_USERNAME_LENGTH + 1];
	char password[GPDS_MAX_PASSWORD_LENGTH + 1];

	uint8_t handle;	/* GPDS context ID */
	uint8_t type;
};

static struct context_data *find_context_by_cid(GSList *contexts,
						unsigned int cid)
{
	GSList *m = NULL;

	for (m = contexts; m; m = m->next) {
		struct context_data *cd = m->data;

		if (cd->cid == cid)
			return cd;
	}
	return NULL;
}

static struct context_data *find_context_by_handle(GSList *contexts,
							uint8_t handle)
{
	GSList *m = NULL;

	for (m = contexts; m; m = m->next) {
		struct context_data *cd = m->data;

		if (cd->handle == handle)
			return cd;
	}
	return NULL;
}

static void destroy_context(struct context_data *cd)
{
	if (!cd)
		return;

	DBG("destroying %p (cid=%u)", cd, cd->cid);

	if (cd->pipe)
		g_isi_pipe_destroy(cd->pipe);

	if (cd->pep)
		g_isi_pep_destroy(cd->pep);

	g_free(cd);
}

static gboolean gprs_up_fail(struct context_data *cd)
{
	struct ofono_gprs_context *gc = cd->driver;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	CALLBACK_WITH_FAILURE(cd->up_cb, NULL, 0, NULL, NULL, NULL, NULL,
				cd->data);

	gcd->contexts = g_slist_remove(gcd->contexts, cd);
	destroy_context(cd);
	return TRUE;
}

static gboolean gprs_down_fail(struct context_data *cd)
{
	struct ofono_gprs_context *gc = cd->driver;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	CALLBACK_WITH_FAILURE(cd->down_cb, cd->data);

	gcd->contexts = g_slist_remove(gcd->contexts, cd);
	destroy_context(cd);
	return TRUE;
}

static gboolean check_resp(GIsiClient *client,
				const uint8_t *restrict msg, size_t len,
				uint_fast8_t cmd, struct context_data *cd)
{
	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return FALSE;
	}

	if (len < 3) {
		DBG("truncated message");
		return FALSE;
	}

	if (msg[0] != cmd) {
		DBG("unexpected message ID: %s (0x%02"PRIx8")",
			gpds_message_id_name(msg[0]), msg[0]);
		return FALSE;
	}

	if ((cd->handle != INVALID_ID && msg[1] != cd->handle)
		|| (msg[1] == INVALID_ID)) {
		DBG("invalid context ID: 0x%02"PRIx8, msg[1]);
		return FALSE;
	}

	if (msg[2] != GPDS_OK) {
		DBG("context creation error: %s (0x%02"PRIx8")",
			gpds_status_name(msg[2]), msg[2]);

		if (len > 3)
			DBG("  fail cause: %s (0x%02"PRIx8")",
				gpds_isi_cause_name(msg[3]), msg[3]);

		return FALSE;
	}
	return TRUE;
}

static void activate_ind_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	struct gprs_context_data *gcd = opaque;
	struct context_data *cd;

	const unsigned char *msg = data;
	GIsiSubBlockIter iter;

	char ifname[IF_NAMESIZE];
	char *ip = NULL;
	char *pdns = NULL;
	char *sdns = NULL;
	const char *dns[3];

	if (!msg || len < 3 || msg[0] != GPDS_CONTEXT_ACTIVATE_IND)
		return;

	cd = find_context_by_handle(gcd->contexts, msg[1]);
	if (!cd) {
		DBG("unknown context: 0x%02"PRIx8, msg[1]);
		return;
	}

	for (g_isi_sb_iter_init(&iter, msg, len, 3);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		uint8_t *addr_value = NULL;
		uint8_t addr_len = 0;

		switch (g_isi_sb_iter_get_id(&iter)) {

		/* TODO: IPv6 address support */

		case GPDS_PDP_ADDRESS_INFO:
			if (!g_isi_sb_iter_get_byte(&iter, &addr_len, 3)
				|| !g_isi_sb_iter_get_data(&iter,
						(void *)&addr_value, 4))
				goto error;

			ip = alloca(INET_ADDRSTRLEN);
			inet_ntop(AF_INET, (const void *)addr_value, ip,
					INET_ADDRSTRLEN);
			break;

		case GPDS_PDNS_ADDRESS_INFO:
			if (!g_isi_sb_iter_get_byte(&iter, &addr_len, 3)
				|| !g_isi_sb_iter_get_data(&iter,
						(void *)&addr_value, 4))
				break;

			pdns = alloca(INET_ADDRSTRLEN);
			inet_ntop(AF_INET, (const void *)addr_value, pdns,
					INET_ADDRSTRLEN);
			break;

		case GPDS_SDNS_ADDRESS_INFO:
			if (!g_isi_sb_iter_get_byte(&iter, &addr_len, 3)
				|| !g_isi_sb_iter_get_data(&iter,
						(void *)&addr_value, 4))
				break;

			sdns = alloca(INET_ADDRSTRLEN);
			inet_ntop(AF_INET, (const void *)addr_value, sdns,
					INET_ADDRSTRLEN);
			break;

		default:
			DBG("skipped sub-block: %s (%zu bytes)",
				gpds_subblock_name(g_isi_sb_iter_get_id(&iter)),
				g_isi_sb_iter_get_len(&iter));
		}
	}

	if (!g_isi_pep_get_ifname(cd->pep, ifname))
		goto error;

	dns[0] = pdns;
	dns[1] = sdns;
	dns[2] = 0;

	CALLBACK_WITH_SUCCESS(cd->up_cb, ifname, TRUE, (const char *)ip,
					STATIC_IP_NETMASK, NULL,
					dns, cd->data);
	return;

error:
	gprs_up_fail(cd);
}

static void activate_fail_ind_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct gprs_context_data *gcd = opaque;
	struct context_data *cd;

	if (!msg || len < 3 || msg[0] != GPDS_CONTEXT_ACTIVATE_FAIL_IND)
		return;

	cd = find_context_by_handle(gcd->contexts, msg[1]);
	if (cd == NULL) {
		DBG("unknown context: 0x%02"PRIx8, msg[1]);
		return;
	}

	gprs_up_fail(cd);
}

static gboolean context_activate_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	struct context_data *cd = opaque;

	if (!check_resp(client, data, len, GPDS_CONTEXT_ACTIVATE_RESP, cd))
		return gprs_up_fail(cd);

	/* TODO: Add timeout here in case indications never come */

	return TRUE;
}

static void send_context_activate(GIsiClient *client, void *opaque)
{
	struct context_data *cd = opaque;

	const unsigned char msg[] = {
		GPDS_CONTEXT_ACTIVATE_REQ,
		cd->handle,	/* context ID */
		0,		/* sub blocks */
	};

	if (g_isi_request_make(client, msg, sizeof(msg), GPDS_TIMEOUT,
				context_activate_cb, cd))
		g_isi_pipe_start(cd->pipe);
	else
		gprs_up_fail(cd);
}

static gboolean context_auth_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	struct context_data *cd = opaque;

	if (!check_resp(client, data, len, GPDS_CONTEXT_AUTH_RESP, cd))
		return gprs_up_fail(cd);

	send_context_activate(client, cd);
	return TRUE;
}

static void send_context_authenticate(GIsiClient *client, void *opaque)
{
	struct context_data *cd = opaque;
	size_t username_len = strlen(cd->username);
	size_t password_len = strlen(cd->password);

	const unsigned char top[] = {
		GPDS_CONTEXT_AUTH_REQ,
		cd->handle,
		2,	/* sub blocks */
		GPDS_USER_NAME_INFO,
		3 + username_len + 3,
		username_len,
		/* Username goes here */
	};

	const unsigned char bottom[] = {
		GPDS_PASSWORD_INFO,
		3 + password_len + 3,
		password_len,
		/* Password goes here */
	};

	const struct iovec iov[4] = {
		{ (uint8_t *)top, sizeof(top) },
		{ cd->username, username_len },
		{ (uint8_t *)bottom, sizeof(bottom) },
		{ cd->password, password_len },
	};

	if (!g_isi_request_vmake(client, iov, 4, GPDS_TIMEOUT,
					context_auth_cb, cd))
		gprs_up_fail(cd);
}

static gboolean context_conf_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	struct context_data *cd = opaque;

	if (!check_resp(client, data, len, GPDS_CONTEXT_CONFIGURE_RESP, cd))
		return gprs_up_fail(cd);

	if (cd->username[0] != '\0')
		send_context_authenticate(client, cd);
	else
		send_context_activate(client, cd);

	return TRUE;
}

static gboolean link_conf_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	struct context_data *cd = opaque;
	size_t apn_len = strlen(cd->apn);

	const unsigned char msg[] = {
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
		3 + apn_len + 3,
		apn_len,
	};

	const struct iovec iov[2] = {
		{ (uint8_t *)msg, sizeof(msg) },
		{ cd->apn, apn_len },
	};

	if (!check_resp(client, data, len, GPDS_LL_CONFIGURE_RESP, cd))
		return gprs_up_fail(cd);

	if (!g_isi_request_vmake(client, iov, 2, GPDS_TIMEOUT,
					context_conf_cb, cd))
		return gprs_up_fail(cd);

	return TRUE;
}

static gboolean create_context_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *resp = data;
	struct context_data *cd = opaque;

	unsigned char msg[] = {
		GPDS_LL_CONFIGURE_REQ,
		0x00,		/* GPDS context ID, added later */
		g_isi_pipe_get_handle(cd->pipe),
		GPDS_LL_PLAIN,	/* link type */
	};

	if (!check_resp(client, data, len, GPDS_CONTEXT_ID_CREATE_RESP, cd))
		return gprs_up_fail(cd);

	cd->handle = msg[1] = resp[1];

	if (!g_isi_request_make(client, msg, sizeof(msg), GPDS_TIMEOUT,
				link_conf_cb, cd))
		return gprs_up_fail(cd);

	/* TODO: send context configuration at the same time? */

	return TRUE;
}

static void create_pipe_cb(GIsiPipe *pipe)
{
	struct context_data *cd = g_isi_pipe_get_userdata(pipe);
	struct ofono_gprs_context *gc = cd->driver;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	const unsigned char msg[] = {
		GPDS_CONTEXT_ID_CREATE_REQ,
	};

	if (!g_isi_request_make(gcd->client, msg, sizeof(msg), GPDS_TIMEOUT,
				create_context_cb, cd))
		gprs_up_fail(cd);
}

static void deactivate_ind_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	struct gprs_context_data *gcd = opaque;
	struct context_data *cd;

	const unsigned char *msg = data;

	if (!msg || len < 3 || msg[0] != GPDS_CONTEXT_DEACTIVATE_IND)
		return;

	cd = find_context_by_handle(gcd->contexts, msg[1]);
	if (cd == NULL) {
		DBG("unknown context: 0x%02"PRIx8, msg[1]);
		return;
	}

	DBG("context deactivated: %s (0x%02"PRIx8")",
		gpds_isi_cause_name(msg[3]), msg[3]);

	ofono_gprs_context_deactivated(cd->driver, cd->cid);

	gcd->contexts = g_slist_remove(gcd->contexts, cd);
	destroy_context(cd);
}

static void isi_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_up_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct context_data *cd = g_try_new0(struct context_data, 1);
	struct context_data *old = NULL;

	if (!cd)
		return;

	cd->cid = ctx->cid;
	cd->driver = gc;
	cd->up_cb = cb;
	cd->data = data;
	cd->pep = NULL;
	cd->pipe = NULL;
	cd->handle = INVALID_ID;
	cd->type = GPDS_PDP_TYPE_IPV4;

	old = find_context_by_cid(gcd->contexts, ctx->cid);
	if (old) {
		DBG("duplicate context: %u", ctx->cid);
		goto error;
	}

	gcd->contexts = g_slist_append(gcd->contexts, cd);

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

	cd->pep = g_isi_pep_create(gcd->idx, NULL, NULL);
	if (cd->pep == NULL)
		goto error;

	cd->pipe = g_isi_pipe_create(gcd->idx, create_pipe_cb,
					g_isi_pep_get_object(cd->pep),
					gcd->gpds, PN_PEP_TYPE_GPRS,
					PN_PEP_TYPE_GPRS);
	if (cd->pipe == NULL)
		goto error;

	g_isi_pipe_set_userdata(cd->pipe, cd);
	return;

error:
	gprs_up_fail(cd);
}

static gboolean context_deactivate_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object,
					void *opaque)
{
	struct context_data *cd = opaque;
	struct ofono_gprs_context *gc = cd->driver;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	if (!check_resp(client, data, len, GPDS_CONTEXT_DEACTIVATE_RESP, cd))
		return gprs_down_fail(cd);

	gcd->contexts = g_slist_remove(gcd->contexts, cd);

	CALLBACK_WITH_SUCCESS(cd->down_cb, cd->data);
	destroy_context(cd);

	return TRUE;
}

static void isi_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct context_data *cd;

	unsigned char msg[] = {
		GPDS_CONTEXT_DEACTIVATE_REQ,
		0x00,	/* GPDS context ID, added later */
	};

	cd = find_context_by_cid(gcd->contexts, cid);
	if (!cd) {
		DBG("unknown context: %u", cid);
		return;
	}

	cd->down_cb = cb;
	cd->data = data;

	msg[1] = cd->handle;

	if (!g_isi_request_make(gcd->client, msg, sizeof(msg), GPDS_TIMEOUT,
				context_deactivate_cb, cd))
		gprs_down_fail(cd);
}

static void gpds_ctx_reachable_cb(GIsiClient *client, gboolean alive,
					uint16_t object,
					void *opaque)
{
	struct ofono_gprs_context *gc = opaque;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	const char *debug;

	if (!alive) {
		DBG("unable to bootstrap gprs context driver");
		return;
	}

	DBG("%s (v%03d.%03d) for PDP contexts",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	gcd->gpds = object;

	debug = getenv("OFONO_ISI_DEBUG");
	if (debug && (strcmp(debug, "all") == 0 || strcmp(debug, "gpds") == 0))
		g_isi_client_set_debug(gcd->client, gpds_debug, NULL);

	g_isi_subscribe(client, GPDS_CONTEXT_ACTIVATE_IND,
			activate_ind_cb, gcd);
	g_isi_subscribe(client, GPDS_CONTEXT_ACTIVATE_FAIL_IND,
			activate_fail_ind_cb, gcd);
	g_isi_subscribe(client, GPDS_CONTEXT_DEACTIVATE_IND,
			deactivate_ind_cb, gcd);
}

static int isi_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct gprs_context_data *gcd = g_try_new0(struct gprs_context_data, 1);

	if (!gcd)
		return -ENOMEM;

	gcd->client = g_isi_client_create(idx, PN_GPDS);
	if (!gcd->client) {
		g_free(gcd);
		return -ENOMEM;
	}

	ofono_gprs_context_set_data(gc, gcd);

	gcd->idx = idx;
	gcd->contexts = NULL;

	g_isi_verify(gcd->client, gpds_ctx_reachable_cb, gc);

	return 0;
}

static void isi_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GSList *m;

	ofono_gprs_context_set_data(gc, NULL);

	for (m = gcd->contexts; m; m = m->next)
		destroy_context(m->data);

	g_slist_free(gcd->contexts);

	if (gcd->client)
		g_isi_client_destroy(gcd->client);

	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "isimodem",
	.probe			= isi_gprs_context_probe,
	.remove			= isi_gprs_context_remove,
	.activate_primary	= isi_gprs_activate_primary,
	.deactivate_primary	= isi_gprs_deactivate_primary,
};

void isi_gprs_context_init()
{
	ofono_gprs_context_driver_register(&driver);
}

void isi_gprs_context_exit()
{
	ofono_gprs_context_driver_unregister(&driver);
}
