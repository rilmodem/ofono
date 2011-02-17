/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <glib.h>

#include "ofono.h"
#include "gatserver.h"

struct ofono_emulator {
	struct ofono_atom *atom;
	enum ofono_emulator_type type;
	GAtServer *server;
};

static void emulator_debug(const char *str, void *data)
{
	ofono_info("%s: %s\n", (char *)data, str);
}

static void emulator_disconnect(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	DBG("%p", em);

	ofono_emulator_remove(em);
}

static void emulator_unregister(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	DBG("%p", em);

	g_at_server_unref(em->server);
	em->server = NULL;
}

void ofono_emulator_register(struct ofono_emulator *em, int fd)
{
	GIOChannel *io;

	DBG("%p, %d", em, fd);

	if (fd < 0)
		return;

	io = g_io_channel_unix_new(fd);

	em->server = g_at_server_new(io);
	if (em->server == NULL)
		return;

	g_io_channel_unref(io);

	g_at_server_set_debug(em->server, emulator_debug, "Server");
	g_at_server_set_disconnect_function(em->server,
						emulator_disconnect, em);

	__ofono_atom_register(em->atom, emulator_unregister);
}

static void emulator_remove(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	g_free(em);
}

struct ofono_emulator *ofono_emulator_create(struct ofono_modem *modem,
						enum ofono_emulator_type type)
{
	struct ofono_emulator *em;
	enum ofono_atom_type atom_t;

	DBG("modem: %p, type: %d", modem, type);

	if (type == OFONO_EMULATOR_TYPE_DUN)
		atom_t = OFONO_ATOM_TYPE_EMULATOR_DUN;
	else if (type == OFONO_EMULATOR_TYPE_HFP)
		atom_t = OFONO_ATOM_TYPE_EMULATOR_HFP;
	else
		return NULL;

	em = g_try_new0(struct ofono_emulator, 1);

	if (em == NULL)
		return NULL;

	em->type = type;

	em->atom = __ofono_modem_add_atom_offline(modem, atom_t,
							emulator_remove, em);

	return em;
}

void ofono_emulator_remove(struct ofono_emulator *em)
{
	__ofono_atom_free(em->atom);
}

void ofono_emulator_send_final(struct ofono_emulator *em,
				const struct ofono_error *final)
{
	char buf[256];

	/*
	 * TODO: Handle various CMEE modes and report error strings from
	 * common.c
	 */
	switch (final->type) {
	case OFONO_ERROR_TYPE_CMS:
		sprintf(buf, "+CMS ERROR: %d", final->error);
		g_at_server_send_ext_final(em->server, buf);
		break;

	case OFONO_ERROR_TYPE_CME:
		sprintf(buf, "+CME ERROR: %d", final->error);
		g_at_server_send_ext_final(em->server, buf);
		break;

	case OFONO_ERROR_TYPE_NO_ERROR:
		g_at_server_send_final(em->server, G_AT_SERVER_RESULT_OK);
		break;

	case OFONO_ERROR_TYPE_CEER:
	case OFONO_ERROR_TYPE_SIM:
	case OFONO_ERROR_TYPE_FAILURE:
		g_at_server_send_final(em->server, G_AT_SERVER_RESULT_ERROR);
		break;
	};
}

void ofono_emulator_send_unsolicited(struct ofono_emulator *em,
					const char *result)
{
	g_at_server_send_unsolicited(em->server, result);
}

void ofono_emulator_send_intermediate(struct ofono_emulator *em,
					const char *result)
{
	g_at_server_send_intermediate(em->server, result);
}

void ofono_emulator_send_info(struct ofono_emulator *em, const char *line,
				ofono_bool_t last)
{
	g_at_server_send_info(em->server, line, last);
}

struct handler {
	ofono_emulator_request_cb_t cb;
	void *data;
	ofono_destroy_func destroy;
	struct ofono_emulator *em;
};

struct ofono_emulator_request {
	GAtResultIter iter;
	enum ofono_emulator_request_type type;
};

static void handler_proxy(GAtServer *server, GAtServerRequestType type,
				GAtResult *result, gpointer userdata)
{
	struct handler *h = userdata;
	struct ofono_emulator_request req;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY;
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_SET;
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_QUERY;
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_SUPPORT;
	}

	g_at_result_iter_init(&req.iter, result);
	g_at_result_iter_next(&req.iter, "");

	h->cb(h->em, &req, h->data);
}

static void handler_destroy(gpointer userdata)
{
	struct handler *h = userdata;

	if (h->destroy)
		h->destroy(h->data);

	g_free(h);
}

ofono_bool_t ofono_emulator_add_handler(struct ofono_emulator *em,
					const char *prefix,
					ofono_emulator_request_cb_t cb,
					void *data, ofono_destroy_func destroy)
{
	struct handler *h;

	h = g_new0(struct handler, 1);
	h->cb = cb;
	h->data = data;
	h->destroy = destroy;
	h->em = em;

	if (g_at_server_register(em->server, prefix, handler_proxy, h,
					handler_destroy) == TRUE)
		return TRUE;

	g_free(h);

	return FALSE;
}

ofono_bool_t ofono_emulator_remove_handler(struct ofono_emulator *em,
						const char *prefix)
{
	return g_at_server_unregister(em->server, prefix);
}

ofono_bool_t ofono_emulator_request_next_string(
					struct ofono_emulator_request *req,
					const char **str)
{
	return g_at_result_iter_next_string(&req->iter, str);
}

ofono_bool_t ofono_emulator_request_next_number(
					struct ofono_emulator_request *req,
					int *number)
{
	return g_at_result_iter_next_number(&req->iter, number);
}

const char *ofono_emulator_request_get_raw(struct ofono_emulator_request *req)
{
	return g_at_result_iter_raw_line(&req->iter);
}

enum ofono_emulator_request_type ofono_emulator_request_get_type(
					struct ofono_emulator_request *req)
{
	return req->type;
}
