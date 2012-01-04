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

static GHashTable *device_hash;

struct dundee_device {
};

const char *__dundee_device_get_path(struct dundee_device *device)
{
	return "/";
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

static void destroy_device(gpointer user)
{
	struct dundee_device *device = user;

	g_free(device);
}

static void device_shutdown(gpointer key, gpointer value, gpointer user_data)
{
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
