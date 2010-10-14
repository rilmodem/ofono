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

#define PN_PIPE	0xd9

typedef struct {
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
} isi_pipe_create_req_t;

typedef struct {
	uint8_t cmd;
	uint8_t pipe_handle;
	uint8_t pad;
} isi_pipe_enable_req_t;

typedef struct {
	uint8_t cmd;
	uint8_t pipe_handle;
	uint8_t state_after;
} isi_pipe_reset_req_t;

typedef struct {
	uint8_t cmd;
	uint8_t pipe_handle;
} isi_pipe_remove_req_t;

typedef struct {
	uint8_t cmd;
	uint8_t pipe_handle;
	uint8_t error_code;

	uint8_t error1;
	uint8_t error2;
} isi_pipe_resp_t;

#define PN_PIPE_INVALID_HANDLE	0xff

enum {
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

enum {	/* error codes */
	PN_PIPE_NO_ERROR,
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

enum {	/* initial pipe state */
	PN_PIPE_DISABLE,
	PN_PIPE_ENABLE,
};

enum {
	PN_MSG_PRIORITY_LOW = 1,
	PN_MSG_PRIORITY_HIGH,
};

struct _GIsiPipe {
	GIsiClient *client;
	void (*handler)(GIsiPipe *);
	void (*error_handler)(GIsiPipe *);
	void *opaque;
	int error;
	uint8_t handle;
	gboolean enabled;
	gboolean enabling;
};

static int g_isi_pipe_error(uint8_t code)
{
	static const int codes[] = {
		[PN_PIPE_NO_ERROR] = 0,
		[PN_PIPE_ERR_INVALID_PARAM] = -EINVAL,
		[PN_PIPE_ERR_INVALID_HANDLE] = -EBADF,
		[PN_PIPE_ERR_INVALID_CTRL_ID] = -ENOTSUP,
		[PN_PIPE_ERR_NOT_ALLOWED] = -EPERM,
		[PN_PIPE_ERR_PEP_IN_USE] = -EBUSY,
		[PN_PIPE_ERR_OVERLOAD] = -ENOBUFS,
		[PN_PIPE_ERR_DEV_DISCONNECTED] = -ENETDOWN,
		[PN_PIPE_ERR_TIMEOUT] = -ETIMEDOUT,
		[PN_PIPE_ERR_ALL_PIPES_IN_USE] = -ENFILE,
		[PN_PIPE_ERR_GENERAL] = -EAGAIN,
		[PN_PIPE_ERR_NOT_SUPPORTED] = -ENOSYS,
	};

	if (code == PN_PIPE_NO_ERROR ||
	    ((code < sizeof(codes) / sizeof(codes[0])) && codes[code]))
		return codes[code];
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

static gboolean g_isi_pipe_created(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	GIsiPipe *pipe = opaque;
	const isi_pipe_resp_t *resp = data;

	if (len < 5 ||
	    resp->cmd != PNS_PIPE_CREATE_RESP)
		return FALSE;

	if (resp->pipe_handle != PN_PIPE_INVALID_HANDLE) {
		pipe->handle = resp->pipe_handle;
		if (pipe->enabling)
			g_isi_pipe_start(pipe);
		if (pipe->handler)
			pipe->handler(pipe);
	} else
		g_isi_pipe_handle_error(pipe, resp->error_code);
	return TRUE;
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
GIsiPipe *g_isi_pipe_create(GIsiModem *modem, void (*created)(GIsiPipe *),
				uint16_t obj1, uint16_t obj2,
				uint8_t type1, uint8_t type2)
{
	isi_pipe_create_req_t msg = {
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
	GIsiPipe *pipe = g_try_malloc(sizeof(GIsiPipe));

	if (pipe == NULL)
		return NULL;

	pipe->client = g_isi_client_create(modem, PN_PIPE);
	pipe->handler = created;
	pipe->error_handler = NULL;
	pipe->error = 0;
	pipe->enabling = FALSE;
	pipe->enabled = FALSE;
	pipe->handle = PN_PIPE_INVALID_HANDLE;

	if (pipe->client == NULL ||
	    g_isi_request_make(pipe->client, &msg, sizeof(msg), 3,
				g_isi_pipe_created, pipe) == NULL)
		goto error;

	return pipe;

error:
	if (pipe->client)
		g_isi_client_destroy(pipe->client);
	g_free(pipe);
	return NULL;
}

static const isi_pipe_resp_t *
g_isi_pipe_check_resp(const GIsiPipe *pipe, uint8_t cmd,
			const void *restrict data, size_t len)
{
	const isi_pipe_resp_t *resp = data;

	if ((len < 5) || (resp->cmd != cmd) ||
	    (resp->pipe_handle != pipe->handle))
		return NULL;
	return resp;
}

static gboolean g_isi_pipe_enabled(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	GIsiPipe *pipe = opaque;
	const isi_pipe_resp_t *resp;

	resp = g_isi_pipe_check_resp(pipe, PNS_PIPE_ENABLE_RESP, data, len);
	if (!resp)
		return FALSE;

	g_isi_pipe_handle_error(pipe, resp->error_code);
	pipe->enabling = FALSE;
	if (!pipe->error)
		pipe->enabled = TRUE;
	return TRUE;
}

static GIsiRequest *g_isi_pipe_enable(GIsiPipe *pipe)
{
	isi_pipe_enable_req_t msg = {
		.cmd = PNS_PIPE_ENABLE_REQ,
		.pipe_handle = pipe->handle,
	};
	const size_t len = 3;

	return g_isi_request_make(pipe->client, &msg, len, 5,
					g_isi_pipe_enabled, pipe);
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
static gboolean g_isi_pipe_removed(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	GIsiPipe *pipe = opaque;
	const isi_pipe_resp_t *resp;

	resp = g_isi_pipe_check_resp(pipe, PNS_PIPE_REMOVE_RESP, data, len);
	if (!resp)
		return FALSE;

	pipe->handle = PN_PIPE_INVALID_HANDLE;
	pipe->error = -EPIPE;
	return TRUE;
}


static GIsiRequest *g_isi_pipe_remove(GIsiPipe *pipe)
{
	isi_pipe_remove_req_t msg = {
		.cmd = PNS_PIPE_REMOVE_REQ,
		.pipe_handle = pipe->handle,
	};
	const size_t len = 3;

	return g_isi_request_make(pipe->client, &msg, len, 5,
					g_isi_pipe_removed, pipe);
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

void g_isi_pipe_set_error_handler(GIsiPipe *pipe, void (*cb)(GIsiPipe *))
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
