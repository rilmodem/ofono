/*
 *
 *  AT chat library with GLib integration
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

#include <glib.h>

#include "gatrawip.h"

struct _GAtRawIP {
	gint ref_count;
	GAtIO *io;
	char *ifname;
	GAtDebugFunc debugf;
	gpointer debug_data;
};

GAtRawIP *g_at_rawip_new(GIOChannel *channel)
{
	GAtRawIP *rawip;
	GAtIO *io;

	io = g_at_io_new(channel);
	if (io == NULL)
		return NULL;

	rawip = g_at_rawip_new_from_io(io);

	g_at_io_unref(io);

	return rawip;
}

GAtRawIP *g_at_rawip_new_from_io(GAtIO *io)
{
	GAtRawIP *rawip;

	rawip = g_try_new0(GAtRawIP, 1);
	if (rawip == NULL)
		return NULL;

	rawip->ref_count = 1;

	rawip->io = g_at_io_ref(io);

	return rawip;
}

GAtRawIP *g_at_rawip_ref(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return NULL;

	g_atomic_int_inc(&rawip->ref_count);

	return rawip;
}

void g_at_rawip_unref(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return;

	if (g_atomic_int_dec_and_test(&rawip->ref_count) == FALSE)
		return;

	g_at_io_unref(rawip->io);
	rawip->io = NULL;

	g_free(rawip->ifname);
	g_free(rawip);
}

void g_at_rawip_open(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return;

	/* open TUN/TAP device */
}

void g_at_rawip_shutdown(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return;

	/* close TUN/TAP device */
}

const char *g_at_rawip_get_interface(GAtRawIP *rawip)
{
	if (rawip == NULL)
		return NULL;

	return rawip->ifname;
}

void g_at_rawip_set_debug(GAtRawIP *rawip, GAtDebugFunc func,
						gpointer user_data)
{
	if (rawip == NULL)
		return;

	rawip->debugf = func;
	rawip->debug_data = user_data;
}
