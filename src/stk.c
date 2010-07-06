/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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
#include <stdio.h>
#include <stdint.h>

#include <glib.h>
#include <gdbus.h>
#include <errno.h>

#include "ofono.h"

#include "smsutil.h"
#include "stkutil.h"

static GSList *g_drivers = NULL;

struct ofono_stk {
	const struct ofono_stk_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	struct stk_command *pending_cmd;
	void (*cancel_cmd)(struct ofono_stk *stk);
	gboolean cancelled;
};

static int stk_respond(struct ofono_stk *stk, struct stk_response *rsp,
			ofono_stk_generic_cb_t cb)
{
	const guint8 *tlv;
	unsigned int tlv_len;

	if (stk->driver->terminal_response == NULL)
		return -ENOSYS;

	rsp->src = STK_DEVICE_IDENTITY_TYPE_TERMINAL;
	rsp->dst = STK_DEVICE_IDENTITY_TYPE_UICC;
	rsp->number = stk->pending_cmd->number;
	rsp->type = stk->pending_cmd->type;
	rsp->qualifier = stk->pending_cmd->qualifier;

	tlv = stk_pdu_from_response(rsp, &tlv_len);
	if (!tlv)
		return -EINVAL;

	stk_command_free(stk->pending_cmd);
	stk->pending_cmd = NULL;

	stk->driver->terminal_response(stk, tlv_len, tlv, cb, stk);

	return 0;
}

static int stk_send_envelope(struct ofono_stk *stk, struct stk_envelope *e,
				ofono_stk_envelope_cb_t cb)
{
	const guint8 *tlv;
	unsigned int tlv_len;

	if (stk->driver->envelope == NULL)
		return -ENOSYS;

	e->dst = STK_DEVICE_IDENTITY_TYPE_UICC;

	tlv = stk_pdu_from_envelope(e, &tlv_len);
	if (!tlv)
		return -EINVAL;

	stk->driver->envelope(stk, tlv_len, tlv, cb, stk);

	return 0;
}

static void stk_cbs_download_cb(const struct ofono_error *error,
				const unsigned char *data, int len, void *user)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("CellBroadcast download to UICC failed");
		/*
		 * "The ME may retry to deliver the same Cell Broadcast
		 * page."
		 */
		return;
	}

	if (len)
		ofono_error("CellBroadcast download returned %i bytes of data",
				len);

	DBG("CellBroadcast download to UICC reported no error");
}

void __ofono_cbs_sim_download(struct ofono_stk *stk, const struct cbs *msg)
{
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_envelope e;
	int err;

	memset(&e, 0, sizeof(e));

	e.type = STK_ENVELOPE_TYPE_CBS_PP_DOWNLOAD;
	e.src = STK_DEVICE_IDENTITY_TYPE_NETWORK;
	memcpy(&e.cbs_pp_download.page, msg, sizeof(msg));

	err = stk_send_envelope(stk, &e, stk_cbs_download_cb);
	if (err)
		stk_cbs_download_cb(&error, NULL, -1, stk);
}

static void stk_command_cb(const struct ofono_error *error, void *data)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("TERMINAL RESPONSE to a UICC command failed");
		/* "The ME may retry to deliver the same Cell Broadcast
		 * page." */
		return;
	}

	DBG("TERMINAL RESPONSE to a command reported no errors");
}

static void stk_proactive_command_cancel(struct ofono_stk *stk)
{
	if (!stk->pending_cmd)
		return;

	stk->cancelled = TRUE;

	stk->cancel_cmd(stk);

	if (stk->pending_cmd) {
		stk_command_free(stk->pending_cmd);
		stk->pending_cmd = NULL;
	}
}

void ofono_stk_proactive_session_end_notify(struct ofono_stk *stk)
{
	stk_proactive_command_cancel(stk);
}

void ofono_stk_proactive_command_notify(struct ofono_stk *stk,
					int length, const unsigned char *pdu)
{
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;
	char *buf;
	int i, err;
	gboolean respond = TRUE;

	/*
	 * Depending on the hardware we may have received a new
	 * command before we managed to send a TERMINAL RESPONSE to
	 * the previous one.  3GPP says in the current revision only
	 * one command can be executing at any time, so assume that
	 * the previous one is being cancelled and the card just
	 * expects a response to the new one.
	 */
	stk_proactive_command_cancel(stk);

	buf = g_try_malloc(length * 2 + 1);
	if (!buf)
		return;

	for (i = 0; i < length; i ++)
		sprintf(buf + i * 2, "%02hhx", pdu[i]);

	stk->cancelled = FALSE;

	stk->pending_cmd = stk_command_new_from_pdu(pdu, length);
	if (!stk->pending_cmd) {
		ofono_error("Can't parse proactive command: %s", buf);

		/*
		 * Nothing we can do, we'd need at least Command Details
		 * to be able to respond with an error.
		 */
		goto done;
	}

	ofono_debug("Proactive command PDU: %s", buf);

	memset(&rsp, 0, sizeof(rsp));

	switch (stk->pending_cmd->status) {
	case STK_PARSE_RESULT_OK:
		switch (stk->pending_cmd->type) {
		default:
			rsp.result.type =
				STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD;
			break;
		}

		if (respond)
			break;
		return;

	case STK_PARSE_RESULT_MISSING_VALUE:
		rsp.result.type = STK_RESULT_TYPE_MINIMUM_NOT_MET;
		break;

	case STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD:
		rsp.result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		break;

	case STK_PARSE_RESULT_TYPE_NOT_UNDERSTOOD:
	default:
		rsp.result.type = STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD;
		break;
	}

	err = stk_respond(stk, &rsp, stk_command_cb);
	if (err)
		stk_command_cb(&error, stk);

done:
	g_free(buf);
}

int ofono_stk_driver_register(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_stk_driver_unregister(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void stk_unregister(struct ofono_atom *atom)
{
	struct ofono_stk *stk = __ofono_atom_get_data(atom);

	if (stk->pending_cmd) {
		stk_command_free(stk->pending_cmd);
		stk->pending_cmd = NULL;
	}
}

static void stk_remove(struct ofono_atom *atom)
{
	struct ofono_stk *stk = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (stk == NULL)
		return;

	if (stk->driver && stk->driver->remove)
		stk->driver->remove(stk);

	g_free(stk);
}

struct ofono_stk *ofono_stk_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_stk *stk;
	GSList *l;

	if (driver == NULL)
		return NULL;

	stk = g_try_new0(struct ofono_stk, 1);

	if (stk == NULL)
		return NULL;

	stk->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_STK,
						stk_remove, stk);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_stk_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(stk, vendor, data) < 0)
			continue;

		stk->driver = drv;
		break;
	}

	return stk;
}

void ofono_stk_register(struct ofono_stk *stk)
{
	__ofono_atom_register(stk->atom, stk_unregister);
}

void ofono_stk_remove(struct ofono_stk *stk)
{
	__ofono_atom_free(stk->atom);
}

void ofono_stk_set_data(struct ofono_stk *stk, void *data)
{
	stk->driver_data = data;
}

void *ofono_stk_get_data(struct ofono_stk *stk)
{
	return stk->driver_data;
}
