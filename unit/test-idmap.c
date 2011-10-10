/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <glib.h>

#include "idmap.h"

static void test_alloc(void)
{
	struct idmap *idmap;
	unsigned int bit;

	idmap = idmap_new(2);

	g_assert(idmap);

	bit = idmap_alloc(idmap);
	g_assert(bit == 1);

	bit = idmap_alloc(idmap);
	g_assert(bit == 2);

	bit = idmap_alloc(idmap);
	g_assert(bit == 3);

	idmap_put(idmap, 3);
	bit = idmap_alloc(idmap);
	g_assert(bit == 3);

	idmap_put(idmap, 0);
	bit = idmap_alloc(idmap);
	g_assert(bit == 3);

	idmap_put(idmap, 1);
	bit = idmap_alloc(idmap);
	g_assert(bit == 1);

	idmap_put(idmap, 1);
	idmap_put(idmap, 2);
	bit = idmap_alloc(idmap);
	g_assert(bit == 1);

	idmap_free(idmap);
}

static void test_alloc_next(void)
{
	struct idmap *idmap;
	unsigned int bit;

	idmap = idmap_new(256);

	g_assert(idmap);

	bit = idmap_alloc_next(idmap, 255);
	g_assert(bit == 256);

	bit = idmap_alloc_next(idmap, 255);
	g_assert(bit == 1);

	bit = idmap_alloc_next(idmap, 1);
	g_assert(bit == 2);

	idmap_free(idmap);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testidmap/alloc", test_alloc);
	g_test_add_func("/testidmap/alloc_next", test_alloc_next);

	return g_test_run();
}
