/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  BMW Car IT GmbH. All rights reserved.
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

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <netinet/ether.h>

#include <glib.h>
#include <gdbus.h>

#include "dundee.h"

static int next_device_id = 0;
static GHashTable *device_hash;

struct dundee_device {
	char *path;
	struct dundee_device_driver *driver;
	gboolean registered;

};

const char *__dundee_device_get_path(struct dundee_device *device)
{
	return device->path;
}

void __dundee_device_append_properties(struct dundee_device *device,
					DBusMessageIter *dict)
{
}

void __dundee_device_foreach(dundee_device_foreach_func func, void *userdata)
{
	GHashTableIter iter;
	gpointer key, value;

	DBG("");

	g_hash_table_iter_init(&iter, device_hash);

	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		struct dundee_device *device = value;

		func(device, userdata);
	}
}

static int register_device(struct dundee_device *device)
{
	return 0;
}

static int unregister_device(struct dundee_device *device)
{
	return 0;
}

static void destroy_device(gpointer user)
{
	struct dundee_device *device = user;

	g_free(device->path);

	g_free(device);
}

struct dundee_device *dundee_device_create(struct dundee_device_driver *d)
{
	struct dundee_device *device;

	device = g_try_new0(struct dundee_device, 1);
	if (device == NULL)
		return NULL;

	device->driver = d;

	device->path = g_strdup_printf("/device%d", next_device_id);
	if (device->path == NULL) {
		g_free(device);
		return NULL;
	}

	next_device_id += 1;

	return device;
}

int dundee_device_register(struct dundee_device *device)
{
	int err;

	err = register_device(device);
	if (err < 0)
		return err;

	device->registered = TRUE;

	g_hash_table_insert(device_hash, g_strdup(device->path), device);

	return 0;
}

void dundee_device_unregister(struct dundee_device *device)
{
	DBG("%p", device);

	unregister_device(device);

	device->registered = FALSE;

	g_hash_table_remove(device_hash, device->path);
}

static void device_shutdown(gpointer key, gpointer value, gpointer user_data)
{
	struct dundee_device *device = value;

	unregister_device(device);
}

void __dundee_device_shutdown(void)
{
	g_hash_table_foreach(device_hash, device_shutdown, NULL);

	__dundee_exit();
}

int __dundee_device_init(void)
{
	DBG("");

	device_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, destroy_device);

	return 0;
}

void __dundee_device_cleanup(void)
{
	DBG("");

	g_hash_table_destroy(device_hash);
}
