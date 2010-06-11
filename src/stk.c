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
};

static void stk_cbs_download_cb(const struct ofono_error *error,
				const unsigned char *data, int len, void *user)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("CellBroadcast download to UICC failed");
		/* "The ME may retry to deliver the same Cell Broadcast
		 * page." */
		return;
	}

	DBG("CellBroadcast download to UICC reported no error");
}

void __ofono_cbs_sim_download(struct ofono_stk *stk, const struct cbs *msg)
{
	const guint8 *tlv;
	unsigned int tlv_len;
	struct stk_envelope e;

	if (stk->driver->envelope == NULL)
		return;

	e.type = STK_ENVELOPE_TYPE_CBS_PP_DOWNLOAD;
	e.src = STK_DEVICE_IDENTITY_TYPE_NETWORK;
	e.dst = STK_DEVICE_IDENTITY_TYPE_UICC;
	memcpy(&e.cbs_pp_download.page, msg, sizeof(msg));

	tlv = stk_pdu_from_envelope(&e, &tlv_len);
	if (!tlv)
		return;

	stk->driver->envelope(stk, tlv_len, tlv, stk_cbs_download_cb, stk);
}

void ofono_stk_proactive_command_notify(struct ofono_stk *stk,
					int length, const unsigned char *pdu)
{
	struct stk_command *cmd;
	char *buf;
	int i;

	buf = g_try_new(char, length * 2 + 1);
	for (i = 0; i < length; i ++)
		sprintf(buf + i * 2, "%02hhx", pdu[i]);
	DBG("Proactive command PDU: %s", buf);

	cmd = stk_command_new_from_pdu(pdu, length);
	if (!cmd) {
		ofono_error("Can't parse proactive command: %s", buf);
		g_free(buf);

		/* TODO: return TERMINAL RESPONSE with permanent error */
		return;
	}

	/* TODO: execute */

	g_free(buf);
	stk_command_free(cmd);
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
