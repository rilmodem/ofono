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

#include <glib.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "cssn.h"

struct cssn_data {
	GSList *mo_handler_list;
	GSList *mt_handler_list;
};

struct mo_handler {
	enum ss_cssi code1;
	mo_ss_notify_cb cb;
	void *cb_data;
};

struct mt_handler {
	enum ss_cssu code2;
	mt_ss_notify_cb cb;
	void *cb_data;
};

static gint ss_handler_compare(gconstpointer a, gconstpointer b)
{
	return memcmp(a, b, sizeof(struct mo_handler));
}

void ofono_mo_ss_register(struct ofono_modem *modem, enum ss_cssi code1,
		mo_ss_notify_cb cb, void *userdata)
{
	struct cssn_data *ss = modem->cssn;
	struct mo_handler *handler = g_try_new0(struct mo_handler, 1);

	handler->code1 = code1;
	handler->cb = cb;
	handler->cb_data = userdata;

	ss->mo_handler_list = g_slist_prepend(ss->mo_handler_list, handler);
}

void ofono_mo_ss_unregister(struct ofono_modem *modem, enum ss_cssi code1,
		mo_ss_notify_cb cb, void *userdata)
{
	struct cssn_data *ss = modem->cssn;
	struct mo_handler val = { code1, cb, userdata };
	GSList *l = g_slist_find_custom(ss->mo_handler_list, &val,
			ss_handler_compare);

	if (!l) {
		ofono_error("An unregistered handler passed to "
				"ofono_mo_ss_unregister");
		return;
	}

	g_free(l->data);
	ss->mo_handler_list = g_slist_delete_link(ss->mo_handler_list, l);
}

void ofono_mt_ss_register(struct ofono_modem *modem, enum ss_cssu code2,
		mt_ss_notify_cb cb, void *userdata)
{
	struct cssn_data *ss = modem->cssn;
	struct mt_handler *handler = g_try_new0(struct mt_handler, 1);

	handler->code2 = code2;
	handler->cb = cb;
	handler->cb_data = userdata;

	ss->mt_handler_list = g_slist_prepend(ss->mt_handler_list, handler);
}

void ofono_mt_ss_unregister(struct ofono_modem *modem, enum ss_cssu code2,
		mt_ss_notify_cb cb, void *userdata)
{
	struct cssn_data *ss = modem->cssn;
	struct mt_handler val = { code2, cb, userdata };
	GSList *l = g_slist_find_custom(ss->mt_handler_list, &val,
			ss_handler_compare);

	if (!l) {
		ofono_error("An unregistered handler passed to "
				"ofono_mt_ss_unregister");
		return;
	}

	g_free(l->data);
	ss->mt_handler_list = g_slist_delete_link(ss->mt_handler_list, l);
}

void ofono_cssn_init(struct ofono_modem *modem)
{
	struct cssn_data *ss = g_try_new0(struct cssn_data, 1);

	modem->cssn = ss;
}

static void cssn_free_handlers(GSList *l)
{
	GSList *iter;

	for (iter = l; iter; iter = iter->next)
		g_free(iter->data);
	g_slist_free(l);
}

void ofono_cssn_exit(struct ofono_modem *modem)
{
	if (!modem->cssn)
		return;

	cssn_free_handlers(modem->cssn->mo_handler_list);
	cssn_free_handlers(modem->cssn->mt_handler_list);
	g_free(modem->cssn);

	modem->cssn = NULL;
}

void ofono_cssi_notify(struct ofono_modem *modem, int code1, int index)
{
	struct cssn_data *ss = modem->cssn;
	struct mo_handler *h;
	GSList *l;

	for (l = ss->mo_handler_list; l; l = l->next) {
		h = l->data;
		if (h->code1 == (enum ss_cssi) code1)
			h->cb(index, h->cb_data);
	}
}

void ofono_cssu_notify(struct ofono_modem *modem, int code2, int index,
			const struct ofono_phone_number *ph)
{
	struct cssn_data *ss = modem->cssn;
	struct mt_handler *h;
	GSList *l;

	for (l = ss->mt_handler_list; l; l = l->next) {
		h = l->data;
		if (h->code2 == (enum ss_cssu) code2)
			h->cb(index, ph, h->cb_data);
	}
}
