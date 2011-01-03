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
#include <errno.h>
#include <glib.h>

#include "client.h"
#include "pipe.h"

#define PN_PIPE			0xD9
#define PN_PIPE_INVALID_HANDLE	0xFF

struct isi_pipe_create_req {
	uint8_t cmd;
	uint8_t state_after;
	uint8_t priority;

	uint8_t device1;
	uint8_t object1;
	uint8_t type1;
	uint8_t pad;

	uint8_t device2;
	uint8_t object2;
	uint8_t type2;
	uint8_t n_sb;
};

struct isi_pipe_enable_req {
	uint8_t cmd;
	uint8_t pipe_handle;
	uint8_t pad;
};

struct isi_pipe_reset_req {
	uint8_t cmd;
	uint8_t pipe_handle;
	uint8_t state_after;
};

struct isi_pipe_remove_req {
	uint8_t cmd;
	uint8_t pipe_handle;
};

struct isi_pipe_resp {
	uint8_t pipe_handle;
	uint8_t error_code;
	uint8_t error1;
	uint8_t error2;
};

enum isi_pipe_message_id {
	PNS_PIPE_CREATE_REQ,
	PNS_PIPE_CREATE_RESP,
	PNS_PIPE_REMOVE_REQ,
	PNS_PIPE_REMOVE_RESP,
	PNS_PIPE_RESET_REQ,
	PNS_PIPE_RESET_RESP,
	PNS_PIPE_ENABLE_REQ,
	PNS_PIPE_ENABLE_RESP,
	PNS_PIPE_REDIRECT_REQ,
	PNS_PIPE_REDIRECT_RESP,
	PNS_PIPE_DISABLE_REQ,
	PNS_PIPE_DISABLE_RESP,
};

enum pn_pipe_error {	/* error codes */
	PN_PIPE_ERR_NO_ERROR,
	PN_PIPE_ERR_INVALID_PARAM,
	PN_PIPE_ERR_INVALID_HANDLE,
	PN_PIPE_ERR_INVALID_CTRL_ID,
	PN_PIPE_ERR_NOT_ALLOWED,
	PN_PIPE_ERR_PEP_IN_USE,
	PN_PIPE_ERR_OVERLOAD,
	PN_PIPE_ERR_DEV_DISCONNECTED,
	PN_PIPE_ERR_TIMEOUT,
	PN_PIPE_ERR_ALL_PIPES_IN_USE,
	PN_PIPE_ERR_GENERAL,
	PN_PIPE_ERR_NOT_SUPPORTED,
};

enum pn_pipe_state {	/* initial pipe state */
	PN_PIPE_DISABLE,
	PN_PIPE_ENABLE,
};

enum pn_msg_priority {
	PN_MSG_PRIORITY_LOW = 1,
	PN_MSG_PRIORITY_HIGH,
};

struct _GIsiPipe {
	GIsiClient *client;
	GIsiPipeHandler handler;
	GIsiPipeErrorHandler error_handler;
	void *opaque;
	int error;
	uint8_t handle;
	gboolean enabled;
	gboolean enabling;
};

static int g_isi_pipe_error(enum pn_pipe_error code)
{
	switch (code) {
	case PN_PIPE_ERR_NO_ERROR:
		return 0;
	case PN_PIPE_ERR_INVALID_PARAM:
		return -EINVAL;
	case PN_PIPE_ERR_INVALID_HANDLE:
		return -EBADF;
	case PN_PIPE_ERR_INVALID_CTRL_ID:
		return -ENOTSUP;
	case PN_PIPE_ERR_NOT_ALLOWED:
		return -EPERM;
	case PN_PIPE_ERR_PEP_IN_USE:
		return -EBUSY;
	case PN_PIPE_ERR_OVERLOAD:
		return -ENOBUFS;
	case PN_PIPE_ERR_DEV_DISCONNECTED:
		return -ENETDOWN;
	case PN_PIPE_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case PN_PIPE_ERR_ALL_PIPES_IN_USE:
		return -ENFILE;
	case PN_PIPE_ERR_GENERAL:
		return -EAGAIN;
	case PN_PIPE_ERR_NOT_SUPPORTED:
		return -ENOSYS;
	}
	return -EBADMSG;
}

static void g_isi_pipe_handle_error(GIsiPipe *pipe, uint8_t code)
{
	int err = g_isi_pipe_error(code);

	if (err == 0)
		return;

	pipe->error = err;

	if (pipe->error_handler)
		pipe->error_handler(pipe);
}

static void g_isi_pipe_created(const GIsiMessage *msg, void *data)
{
	struct isi_pipe_resp *resp;
	size_t len = sizeof(struct isi_pipe_resp);
	GIsiPipe *pipe = data;

	if (g_isi_msg_error(msg) < 0) {
		g_isi_pipe_handle_error(pipe, PN_PIPE_ERR_TIMEOUT);
		return;
	}

	if (g_isi_msg_id(msg) != PNS_PIPE_CREATE_RESP)
		return;

	if (!g_isi_msg_data_get_struct(msg, 0, (const void **) &resp, len))
		return;

	if (resp->pipe_handle == PN_PIPE_INVALID_HANDLE) {
		g_isi_pipe_handle_error(pipe, resp->error_code);
		return;
	}

	pipe->handle = resp->pipe_handle;

	if (pipe->enabling)
		g_isi_pipe_start(pipe);

	if (pipe->handler)
		pipe->handler(pipe);
}

/**
 * Create a Phonet pipe in disabled state and with low priority.
 * @param modem ISI modem to create a pipe with
 * @param created optional callback for created event
 * @param obj1 Object handle of the first end point
 * @param obj2 Object handle of the second end point
 * @param type1 Type of the first end point
 * @param type2 Type of the second end point
 * @return a pipe object on success, NULL on error.
 */
GIsiPipe *g_isi_pipe_create(GIsiModem *modem, GIsiPipeHandler cb, uint16_t obj1,
				uint16_t obj2, uint8_t type1, uint8_t type2)
{
	struct isi_pipe_create_req msg = {
		.cmd = PNS_PIPE_CREATE_REQ,
		.state_after = PN_PIPE_DISABLE,
		.priority = PN_MSG_PRIORITY_LOW,
		.device1 = obj1 >> 8,
		.object1 = obj1 & 0xff,
		.type1 = type1,
		.device2 = obj2 >> 8,
		.object2 = obj2 & 0xff,
		.type2 = type2,
		.n_sb = 0,
	};
	size_t len = sizeof(msg);
	GIsiPipe *pipe;

	pipe = g_try_new0(GIsiPipe, 1);
	if (pipe == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	pipe->client = g_isi_client_create(modem, PN_PIPE);
	if (pipe->client == NULL) {
		errno = ENOMEM;
		g_free(pipe);
		return NULL;
	}

	pipe->handler = cb;
	pipe->error_handler = NULL;
	pipe->error = 0;
	pipe->enabling = FALSE;
	pipe->enabled = FALSE;
	pipe->handle = PN_PIPE_INVALID_HANDLE;

	if (g_isi_client_send(pipe->client, &msg, len,
					g_isi_pipe_created, pipe, NULL))
		return pipe;

	g_isi_client_destroy(pipe->client);
	g_free(pipe);

	return NULL;
}

static void g_isi_pipe_enabled(const GIsiMessage *msg, void *data)
{
	GIsiPipe *pipe = data;
	const struct isi_pipe_resp *resp;
	size_t len = sizeof(struct isi_pipe_resp);

	if (g_isi_msg_error(msg) < 0) {
		g_isi_pipe_handle_error(pipe, PN_PIPE_ERR_TIMEOUT);
		return;
	}

	if (g_isi_msg_id(msg) != PNS_PIPE_ENABLE_RESP)
		return;

	if (!g_isi_msg_data_get_struct(msg, 0, (const void **) &resp, len))
		return;

	if (pipe->handle != resp->pipe_handle)
		return;

	g_isi_pipe_handle_error(pipe, resp->error_code);

	pipe->enabling = FALSE;

	if (!pipe->error)
		pipe->enabled = TRUE;
}

static void g_isi_pipe_enable(GIsiPipe *pipe)
{
	struct isi_pipe_enable_req msg = {
		.cmd = PNS_PIPE_ENABLE_REQ,
		.pipe_handle = pipe->handle,
	};
	size_t len = sizeof(msg);

	g_isi_client_send(pipe->client, &msg, len,
				g_isi_pipe_enabled, pipe, NULL);
}

/**
 * Enable a pipe, i.e. turn on data transfer between the two end points.
 * @param pipe pipe as returned from g_isi_pipe_create()
 * @return 0 on success or an error code
 */
int g_isi_pipe_start(GIsiPipe *pipe)
{
	if (pipe->error)
		return pipe->error;

	if (pipe->enabling || pipe->enabled)
		return 0;

	if (pipe->handle != PN_PIPE_INVALID_HANDLE)
		g_isi_pipe_enable(pipe);
	else
		pipe->enabling = TRUE;

	return 0;
}

/* Not very useful, it will never have time to trigger */
static void g_isi_pipe_removed(const GIsiMessage *msg, void *data)
{
	GIsiPipe *pipe = data;
	struct isi_pipe_resp *resp;
	size_t len = sizeof(struct isi_pipe_resp);

	if (g_isi_msg_error(msg) < 0) {
		g_isi_pipe_handle_error(pipe, PN_PIPE_ERR_TIMEOUT);
		return;
	}

	if (g_isi_msg_id(msg) != PNS_PIPE_REMOVE_RESP)
		return;

	if (!g_isi_msg_data_get_struct(msg, 0, (const void **) &resp, len))
		return;

	if (pipe->handle != resp->pipe_handle)
		return;

	pipe->handle = PN_PIPE_INVALID_HANDLE;
	pipe->error = -EPIPE;
}


static void g_isi_pipe_remove(GIsiPipe *pipe)
{
	struct isi_pipe_remove_req msg = {
		.cmd = PNS_PIPE_REMOVE_REQ,
		.pipe_handle = pipe->handle,
	};
	size_t len = sizeof(msg);

	g_isi_client_send(pipe->client, &msg, len,
				g_isi_pipe_removed, pipe, NULL);
}

/**
 * Destroy a pipe. If it was connected, it is removed.
 * @param pipe pipe as returned from g_isi_pipe_create()
 */
void g_isi_pipe_destroy(GIsiPipe *pipe)
{
	if (!pipe->error)
		g_isi_pipe_remove(pipe);

	g_isi_client_destroy(pipe->client);
	g_free(pipe);
}

void g_isi_pipe_set_error_handler(GIsiPipe *pipe, GIsiPipeErrorHandler cb)
{
	pipe->error_handler = cb;
}

int g_isi_pipe_get_error(const GIsiPipe *pipe)
{
	return pipe->error;
}

void *g_isi_pipe_set_userdata(GIsiPipe *pipe, void *opaque)
{
	void *old = pipe->opaque;

	pipe->opaque = opaque;
	return old;
}

void *g_isi_pipe_get_userdata(GIsiPipe *pipe)
{
	return pipe->opaque;
}

/**
 * Return a pipe handle.
 * @param pipe a ready-made pipe with handler data present. Available
 * after the pipe creation callback is called.
 * @return uint8_t handle.
 */

uint8_t g_isi_pipe_get_handle(GIsiPipe *pipe)
{
	return pipe->handle;
}
