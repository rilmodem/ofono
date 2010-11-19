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

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include "ofono.h"

#include "common.h"

static GSList *g_drivers = NULL;

struct ssn_handler {
	struct ofono_watchlist_item item;
	int code;
};

struct ofono_ssn {
	struct ofono_watchlist *mo_handler_list;
	struct ofono_watchlist *mt_handler_list;
	const struct ofono_ssn_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static unsigned int add_ssn_handler(struct ofono_watchlist *watchlist,
					int code, void *notify, void *data,
					ofono_destroy_func destroy)
{
	struct ssn_handler *handler;

	if (notify == NULL)
		return 0;

	handler = g_new0(struct ssn_handler, 1);

	handler->code = code;
	handler->item.notify = notify;
	handler->item.notify_data = data;
	handler->item.destroy = destroy;

	return __ofono_watchlist_add_item(watchlist,
				(struct ofono_watchlist_item *)handler);
}

unsigned int __ofono_ssn_mo_watch_add(struct ofono_ssn *ssn, int code1,
					ofono_ssn_mo_notify_cb cb, void *user,
					ofono_destroy_func destroy)
{
	if (ssn == NULL)
		return 0;

	DBG("%p, %d", ssn, code1);

	return add_ssn_handler(ssn->mo_handler_list, code1, cb, user, destroy);
}

gboolean __ofono_ssn_mo_watch_remove(struct ofono_ssn *ssn, unsigned int id)
{
	if (ssn == NULL)
		return FALSE;

	DBG("%p, %u", ssn, id);

	return __ofono_watchlist_remove_item(ssn->mo_handler_list, id);
}

unsigned int __ofono_ssn_mt_watch_add(struct ofono_ssn *ssn, int code2,
					ofono_ssn_mt_notify_cb cb, void *user,
					ofono_destroy_func destroy)
{
	if (ssn == NULL)
		return 0;

	DBG("%p, %d", ssn, code2);

	return add_ssn_handler(ssn->mt_handler_list, code2, cb, user, destroy);
}

gboolean __ofono_ssn_mt_watch_remove(struct ofono_ssn *ssn, unsigned int id)
{
	if (ssn == NULL)
		return FALSE;

	DBG("%p, %u", ssn, id);

	return __ofono_watchlist_remove_item(ssn->mt_handler_list, id);
}

void ofono_ssn_cssi_notify(struct ofono_ssn *ssn, int code1, int index)
{
	struct ssn_handler *h;
	GSList *l;
	ofono_ssn_mo_notify_cb notify;

	for (l = ssn->mo_handler_list->items; l; l = l->next) {
		h = l->data;
		notify = h->item.notify;

		if (h->code == code1)
			notify(index, h->item.notify_data);
	}
}

void ofono_ssn_cssu_notify(struct ofono_ssn *ssn, int code2, int index,
				const struct ofono_phone_number *ph)
{
	struct ssn_handler *h;
	GSList *l;
	ofono_ssn_mt_notify_cb notify;

	for (l = ssn->mt_handler_list->items; l; l = l->next) {
		h = l->data;
		notify = h->item.notify;

		if (h->code == code2)
			notify(index, ph, h->item.notify_data);
	}
}

int ofono_ssn_driver_register(const struct ofono_ssn_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_ssn_driver_unregister(const struct ofono_ssn_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void ssn_unregister(struct ofono_atom *atom)
{
	struct ofono_ssn *ssn = __ofono_atom_get_data(atom);

	__ofono_watchlist_free(ssn->mo_handler_list);
	ssn->mo_handler_list = NULL;

	__ofono_watchlist_free(ssn->mt_handler_list);
	ssn->mt_handler_list = NULL;
}

static void ssn_remove(struct ofono_atom *atom)
{
	struct ofono_ssn *ssn = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (ssn == NULL)
		return;

	if (ssn->driver && ssn->driver->remove)
		ssn->driver->remove(ssn);

	g_free(ssn);
}

struct ofono_ssn *ofono_ssn_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_ssn *ssn;
	GSList *l;

	if (driver == NULL)
		return NULL;

	ssn = g_try_new0(struct ofono_ssn, 1);

	if (ssn == NULL)
		return NULL;

	ssn->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_SSN,
						ssn_remove, ssn);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_ssn_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(ssn, vendor, data) < 0)
			continue;

		ssn->driver = drv;
		break;
	}

	return ssn;
}

void ofono_ssn_register(struct ofono_ssn *ssn)
{
	ssn->mo_handler_list = __ofono_watchlist_new(g_free);
	ssn->mt_handler_list = __ofono_watchlist_new(g_free);

	__ofono_atom_register(ssn->atom, ssn_unregister);
}

void ofono_ssn_remove(struct ofono_ssn *ssn)
{
	__ofono_atom_free(ssn->atom);
}

void ofono_ssn_set_data(struct ofono_ssn *ssn, void *data)
{
	ssn->driver_data = data;
}

void *ofono_ssn_get_data(struct ofono_ssn *ssn)
{
	return ssn->driver_data;
}
