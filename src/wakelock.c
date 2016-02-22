/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2015 Jolla Ltd. All rights reserved.
 *  Contact: Hannu Mallat <hannu.mallat@jollamobile.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
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

#include <errno.h>
#include <string.h>
#include <glib.h>

#include "log.h"
#include "wakelock.h"
#include "ofono.h"

#define SYSTEM_WAKELOCK_NAME			"ofono-system"
#define SYSTEM_WAKELOCK_DURATION		30

static char *impl = NULL;
static struct wakelock_table table;

static struct wakelock *system_wakelock = NULL;
static guint system_wakelock_source = 0;

static gboolean system_wakelock_put(gpointer user_data)
{
	DBG("Releasing system wakelock");
	wakelock_release(system_wakelock);
	system_wakelock_source = 0;
	return FALSE;
}

void wakelock_system_lock(void)
{
	guint old_source;

	if (impl == NULL)
		return;

	DBG("Acquiring system wakelock");

	old_source = system_wakelock_source;

	system_wakelock_source = g_timeout_add_seconds(SYSTEM_WAKELOCK_DURATION,
							system_wakelock_put,
							NULL);

	if (system_wakelock_source)
		wakelock_acquire(system_wakelock);

	if (old_source) {
		g_source_remove(old_source);
		wakelock_release(system_wakelock);
	}
}

int wakelock_create(const char *name, struct wakelock **wakelock)
{
	if (impl == NULL) {
		*wakelock = NULL;
		return -EINVAL;
	}

	return (table.create)(name, wakelock);
}

int wakelock_free(struct wakelock *wakelock)
{
	if (impl == NULL)
		return -EINVAL;

	return table.free(wakelock);
}

int wakelock_acquire(struct wakelock *wakelock)
{
	if (impl == NULL)
		return -EINVAL;

	return table.acquire(wakelock);
}

int wakelock_release(struct wakelock *wakelock)
{
	if (impl == NULL)
		return -EINVAL;

	return table.release(wakelock);
}

ofono_bool_t wakelock_is_locked(struct wakelock *wakelock) {
	if (impl == NULL)
		return -EINVAL;

	return table.is_locked(wakelock);
}

int wakelock_plugin_register(const char *name, struct wakelock_table *fns)
{
	if (impl)
		return -EALREADY;

	impl = g_strdup(name);
	memcpy(&table, fns, sizeof(struct wakelock_table));
	return 0;
}

int wakelock_plugin_unregister(void)
{
	if (impl == NULL)
		return -ENOENT;

	memset(&table, 0, sizeof(struct wakelock_table));
	g_free(impl);
	impl = NULL;

	return 0;
}

int __ofono_wakelock_init(void)
{
	if (wakelock_create(SYSTEM_WAKELOCK_NAME, &system_wakelock) < 0)
		ofono_warn("Failed to create system keep alive wakelock");

	return 0;
}

void __ofono_wakelock_cleanup(void)
{
	if (system_wakelock)
		wakelock_free(system_wakelock);
}
