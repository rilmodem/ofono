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

#include <stdint.h>
#include <glib.h>

#include "client.h"

#define VERSION_TIMEOUT				5
#define VERSION_RETRIES				2

#define COMMON_MESSAGE				0xF0
#define COMM_ISI_VERSION_GET_REQ		0x12
#define COMM_ISI_VERSION_GET_RESP		0x13
#define COMM_ISA_ENTITY_NOT_REACHABLE_RESP	0x14

struct verify_data {
	GIsiVerifyFunc func;
	void *data;
	guint count;
	uint8_t resource;
};

static GIsiRequest *send_version_query(GIsiClient *client, GIsiResponseFunc cb,
					void *opaque)
{
	struct verify_data *vd = opaque;

	struct sockaddr_pn dst = {
		.spn_family = AF_PHONET,
		.spn_resource = vd->resource,
	};

	uint8_t msg[] = {
		COMMON_MESSAGE,
		COMM_ISI_VERSION_GET_REQ,
		0x00  /* Filler */
	};

	return g_isi_sendto(client, &dst, msg, sizeof(msg), VERSION_TIMEOUT,
			cb, opaque, NULL);
}

static gboolean verify_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const uint8_t *msg = data;
	struct verify_data *vd = opaque;
	GIsiVerifyFunc func = vd->func;

	gboolean alive = FALSE;

	if (!msg) {

		if (++vd->count < VERSION_RETRIES) {

			g_warning("Retry COMM_ISI_VERSION_GET_REQ");

			if (send_version_query(client, verify_cb, opaque))
				return TRUE;
		}

		g_warning("Timeout COMM_ISI_VERSION_GET_REQ");

		goto out;
	}

	if (len < 2 || msg[0] != COMMON_MESSAGE)
		goto out;

	if (msg[1] == COMM_ISI_VERSION_GET_RESP && len >= 4) {
		if (vd->resource == g_isi_client_resource(client)) {
			g_isi_version_set(client, msg[2], msg[3]);
			g_isi_server_object_set(client, object);
		}
		alive = TRUE;
		goto out;
	}

	if (msg[1] != COMM_ISA_ENTITY_NOT_REACHABLE_RESP)
		alive = TRUE;

out:
	if (func)
		func(client, alive, object, vd->data);

	g_free(vd);
	return TRUE;
}

/**
 * Verifies reachability of @a client with its resource. As a side
 * effect of this liveliness check, the ISI version of the interface
 * and the server object implementing the resource will be made
 * available via g_isi_client_version() and g_isi_server_object(),
 * respectively.
 * @param client client to verify
 * @param func callback to process outcome
 * @param opaque user data
 * @return NULL on error (see errno), GIsiRequest pointer on success.
 */
GIsiRequest *g_isi_verify(GIsiClient *client, GIsiVerifyFunc func,
				void *opaque)
{
	struct verify_data *data = g_try_new0(struct verify_data, 1);
	GIsiRequest *req = NULL;

	if (data == NULL)
		return NULL;

	data->func = func;
	data->data = opaque;
	data->resource = g_isi_client_resource(client);

	req = send_version_query(client, verify_cb, data);
	if (!req)
		g_free(data);

	return req;
}

/**
 * Verifies the reachability of an arbitrary resource.
 * @param client client to verify
 * @param func callback to process outcome
 * @param opaque user data
 * @return NULL on error (see errno), GIsiRequest pointer on success.
 */
GIsiRequest *g_isi_verify_resource(GIsiClient *client, uint8_t resource,
				GIsiVerifyFunc func, void *opaque)
{
	struct verify_data *data = g_try_new0(struct verify_data, 1);
	GIsiRequest *req = NULL;

	data->func = func;
	data->data = opaque;
	data->resource = resource;

	req = send_version_query(client, verify_cb, data);
	if (!req)
		g_free(data);

	return req;
}
