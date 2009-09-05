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

#include <glib.h>
#include <glib/gprintf.h>

#include "gsm0710.h"

static void debug_message(struct gsm0710_context *ctx, const char *msg)
{
	g_print("debug: %s\n", msg);
}

static void test_setup(void)
{
	struct gsm0710_context ctx;

	gsm0710_initialize(&ctx);

	ctx.fd = -1;
	ctx.debug_message = debug_message;

	gsm0710_startup(&ctx, 0);

	gsm0710_shutdown(&ctx);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testmux/Setup", test_setup);

	return g_test_run();
}
