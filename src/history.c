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

#include <glib.h>

#include "ofono.h"

static GSList *history_drivers = NULL;

struct history_call_foreach_data {
	const struct ofono_call *call;
	union {
		struct {
			time_t start;
			time_t end;
		};

		time_t when;
	};
};

struct history_sms_foreach_data {
	const struct ofono_uuid *uuid;
	const char *address;
	const char *text;
	union {
		struct {
			const struct tm *remote;
			const struct tm *local;
		};
		struct {
			time_t when;
			enum ofono_history_sms_status status;
		};
	};
};

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

static void context_remove(struct ofono_atom *atom)
{
	struct ofono_history_context *context = __ofono_atom_get_data(atom);

	if (context->driver->remove)
		context->driver->remove(context);

	g_free(context);
}

void __ofono_history_probe_drivers(struct ofono_modem *modem)
{
	struct ofono_history_driver *driver;
	struct ofono_history_context *context;
	GSList *l;

	for (l = history_drivers; l; l = l->next) {
		driver = l->data;

		context = history_context_create(modem, driver);
		if (context == NULL)
			continue;

		__ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_HISTORY,
						context_remove, context);
	}
}

static void history_call_ended(struct ofono_atom *atom, void *data)
{
	struct ofono_history_context *context = __ofono_atom_get_data(atom);
	struct history_call_foreach_data *hfd = data;

	if (context->driver->call_ended == NULL)
		return;

	context->driver->call_ended(context, hfd->call, hfd->start, hfd->end);
}

void __ofono_history_call_ended(struct ofono_modem *modem,
				const struct ofono_call *call,
				time_t start, time_t end)
{
	struct history_call_foreach_data hfd;

	hfd.call = call;
	hfd.start = start;
	hfd.end = end;

	__ofono_modem_foreach_atom(modem, OFONO_ATOM_TYPE_HISTORY,
					history_call_ended, &hfd);
}

static void history_call_missed(struct ofono_atom *atom, void *data)
{
	struct ofono_history_context *context = __ofono_atom_get_data(atom);
	struct history_call_foreach_data *hfd = data;

	if (context->driver->call_missed == NULL)
		return;

	context->driver->call_missed(context, hfd->call, hfd->when);
}

void __ofono_history_call_missed(struct ofono_modem *modem,
				const struct ofono_call *call, time_t when)
{
	struct history_call_foreach_data hfd;

	hfd.call = call;
	hfd.when = when;

	__ofono_modem_foreach_atom(modem, OFONO_ATOM_TYPE_HISTORY,
					history_call_missed, &hfd);
}

static void history_sms_received(struct ofono_atom *atom, void *data)
{
	struct ofono_history_context *context = __ofono_atom_get_data(atom);
	struct history_sms_foreach_data *hfd = data;

	if (context->driver->sms_received == NULL)
		return;

	context->driver->sms_received(context, hfd->uuid, hfd->address,
					hfd->remote, hfd->local, hfd->text);
}

void __ofono_history_sms_received(struct ofono_modem *modem,
					const struct ofono_uuid *uuid,
					const char *from,
					const struct tm *remote,
					const struct tm *local,
					const char *text)
{
	struct history_sms_foreach_data hfd;

	hfd.uuid = uuid;
	hfd.address = from;
	hfd.remote = remote;
	hfd.local = local;
	hfd.text = text;

	__ofono_modem_foreach_atom(modem, OFONO_ATOM_TYPE_HISTORY,
					history_sms_received, &hfd);
}

static void history_sms_send_pending(struct ofono_atom *atom, void *data)
{
	struct ofono_history_context *context = __ofono_atom_get_data(atom);
	struct history_sms_foreach_data *hfd = data;

	if (context->driver->sms_send_pending == NULL)
		return;

	context->driver->sms_send_pending(context, hfd->uuid, hfd->address,
						hfd->when, hfd->text);
}

void __ofono_history_sms_send_pending(struct ofono_modem *modem,
					const struct ofono_uuid *uuid,
					const char *to,
					time_t when, const char *text)
{
	struct history_sms_foreach_data hfd;

	hfd.uuid = uuid;
	hfd.address = to;
	hfd.text = text;
	hfd.when = when;
	hfd.status = OFONO_HISTORY_SMS_STATUS_PENDING;

	__ofono_modem_foreach_atom(modem, OFONO_ATOM_TYPE_HISTORY,
					history_sms_send_pending, &hfd);
}

static void history_sms_send_status(struct ofono_atom *atom, void *data)
{
	struct ofono_history_context *context = __ofono_atom_get_data(atom);
	struct history_sms_foreach_data *hfd = data;

	if (context->driver->sms_send_status == NULL)
		return;

	context->driver->sms_send_status(context, hfd->uuid,
						hfd->when, hfd->status);
}

void __ofono_history_sms_send_status(struct ofono_modem *modem,
					const struct ofono_uuid *uuid,
					time_t when,
					enum ofono_history_sms_status status)
{
	struct history_sms_foreach_data hfd;

	hfd.uuid = uuid;
	hfd.address = NULL;
	hfd.text = NULL;
	hfd.when = when;
	hfd.status = status;

	__ofono_modem_foreach_atom(modem, OFONO_ATOM_TYPE_HISTORY,
					history_sms_send_status, &hfd);
}

int ofono_history_driver_register(const struct ofono_history_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	history_drivers = g_slist_prepend(history_drivers, (void *) driver);

	return 0;
}

void ofono_history_driver_unregister(const struct ofono_history_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	history_drivers = g_slist_remove(history_drivers, driver);
}
