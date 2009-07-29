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

static GSList *history_drivers = NULL;

static struct ofono_history_context *history_context_create(
					struct ofono_modem *modem,
					struct ofono_history_driver *driver)
{
	struct ofono_history_context *context;

	if (driver->probe == NULL)
		return NULL;

	context = g_try_new0(struct ofono_history_context, 1);

	if (context == NULL)
		return NULL;

	context->driver = driver;
	context->modem = modem;

	if (driver->probe(context) < 0) {
		g_free(context);
		return NULL;
	}

	return context;
}

void __ofono_history_probe_drivers(struct ofono_modem *modem)
{
	GSList *l;
	struct ofono_history_context *context;
	struct ofono_history_driver *driver;

	for (l = history_drivers; l; l = l->next) {
		driver = l->data;

		context = history_context_create(modem, driver);

		if (!context)
			continue;

		modem->history_contexts =
			g_slist_prepend(modem->history_contexts, context);
	}
}

void __ofono_history_remove_drivers(struct ofono_modem *modem)
{
	GSList *l;
	struct ofono_history_context *context;

	for (l = modem->history_contexts; l; l = l->next) {
		context = l->data;

		if (context->driver->remove)
			context->driver->remove(context);

		g_free(context);
	}

	g_slist_free(modem->history_contexts);
	modem->history_contexts = NULL;
}

void __ofono_history_call_ended(struct ofono_modem *modem,
				const struct ofono_call *call,
				time_t start, time_t end)
{
	struct ofono_history_context *context;
	GSList *l;

	for (l = modem->history_contexts; l; l = l->next) {
		context = l->data;

		if (context->driver->call_ended)
			context->driver->call_ended(context, call, start, end);
	}
}

void __ofono_history_call_missed(struct ofono_modem *modem,
				const struct ofono_call *call, time_t when)
{
	struct ofono_history_context *context;
	GSList *l;

	for (l = modem->history_contexts; l; l = l->next) {
		context = l->data;

		if (context->driver->call_missed)
			context->driver->call_missed(context, call, when);
	}
}

int ofono_history_driver_register(const struct ofono_history_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	history_drivers = g_slist_prepend(history_drivers, (void *)driver);

	return 0;
}

void ofono_history_driver_unregister(const struct ofono_history_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	history_drivers = g_slist_remove(history_drivers, driver);
}
