/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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
	unsigned int id;
	int code;
	void *notify;
	void *data;
	ofono_destroy_func destroy;
};

struct ofono_ssn {
	GSList *mo_handler_list;
	GSList *mt_handler_list;
	unsigned int next_id;
	const struct ofono_ssn_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static unsigned int add_ssn_handler(GSList **l, unsigned int *id,
					int code, void *notify, void *data,
					ofono_destroy_func destroy)
{
	struct ssn_handler *handler;

	if (notify == NULL)
		return 0;

	handler = g_new0(struct ssn_handler, 1);

	handler->code = code;
	handler->id = *id;
	*id = *id + 1;
	handler->notify = notify;
	handler->destroy = destroy;
	handler->data = data;

	*l = g_slist_prepend(*l, handler);

	return handler->id;
}

static gboolean remove_ssn_handler_by_id(GSList **l, unsigned int id)
{
	struct ssn_handler *handler;
	GSList *p;
	GSList *c;

	p = NULL;
	c = *l;

	while (c) {
		handler = c->data;

		if (handler->id != id) {
			p = c;
			c = c->next;
			continue;
		}

		if (p)
			p->next = c->next;
		else
			*l = c->next;

		if (handler->destroy)
			handler->destroy(handler->data);

		g_free(handler);
		g_slist_free_1(c);

		return TRUE;
	}

	return FALSE;
}

static void remove_all_handlers(GSList **l)
{
	struct ssn_handler *handler;
	GSList *c;

	for (c = *l; c; c = c->next) {
		handler = c->data;

		if (handler->destroy)
			handler->destroy(handler->data);

		g_free(handler);
	}

	g_slist_free(*l);
	*l = NULL;
}

unsigned int __ofono_ssn_mo_watch_add(struct ofono_ssn *ssn, int code1,
					ofono_ssn_mo_notify_cb cb, void *user,
					ofono_destroy_func destroy)
{
	if (ssn == NULL)
		return 0;

	DBG("%p, %d", ssn, code1);

	return add_ssn_handler(&ssn->mo_handler_list, &ssn->next_id,
				code1, cb, user, destroy);
}

gboolean __ofono_ssn_mo_watch_remove(struct ofono_ssn *ssn, int id)
{
	if (ssn == NULL)
		return FALSE;

	DBG("%p, %u", ssn, id);

	return remove_ssn_handler_by_id(&ssn->mo_handler_list, id);
}

unsigned int __ofono_ssn_mt_watch_add(struct ofono_ssn *ssn, int code2,
					ofono_ssn_mt_notify_cb cb, void *user,
					ofono_destroy_func destroy)
{
	if (ssn == NULL)
		return 0;

	DBG("%p, %d", ssn, code2);

	return add_ssn_handler(&ssn->mt_handler_list, &ssn->next_id,
				code2, cb, user, destroy);
}

gboolean __ofono_ssn_mt_watch_remove(struct ofono_ssn *ssn, int id)
{
	if (ssn == NULL)
		return FALSE;

	DBG("%p, %u", ssn, id);

	return remove_ssn_handler_by_id(&ssn->mt_handler_list, id);
}

void ofono_ssn_cssi_notify(struct ofono_ssn *ssn, int code1, int index)
{
	struct ssn_handler *h;
	GSList *l;
	ofono_ssn_mo_notify_cb notify;

	for (l = ssn->mo_handler_list; l; l = l->next) {
		h = l->data;
		notify = h->notify;

		if (h->code == code1)
			notify(index, h->data);
	}
}

void ofono_ssn_cssu_notify(struct ofono_ssn *ssn, int code2, int index,
				const struct ofono_phone_number *ph)
{
	struct ssn_handler *h;
	GSList *l;
	ofono_ssn_mt_notify_cb notify;

	for (l = ssn->mt_handler_list; l; l = l->next) {
		h = l->data;
		notify = h->notify;

		if (h->code == code2)
			notify(index, ph, h->data);
	}
}

int ofono_ssn_driver_register(const struct ofono_ssn_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_ssn_driver_unregister(const struct ofono_ssn_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void ssn_unregister(struct ofono_atom *atom)
{
	struct ofono_ssn *ssn = __ofono_atom_get_data(atom);

	remove_all_handlers(&ssn->mo_handler_list);
	remove_all_handlers(&ssn->mt_handler_list);
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
