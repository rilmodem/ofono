/*
 *
 *  AT chat library with GLib integration
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

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>

#include <glib.h>

#include "gsm0710.h"
#include "gatmux.h"

struct _GAtMux {
	gint ref_count;				/* Ref count */
	GIOChannel *channel;			/* channel */
	GAtDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	GAtDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
};

GAtMux *g_at_mux_new(GIOChannel *channel)
{
	GAtMux *mux;

	if (!channel)
		return NULL;

	mux = g_try_new0(GAtMux, 1);

	if (!mux)
		return mux;

	mux->ref_count = 1;

	mux->channel = channel;

	return mux;
}

static int open_device(const char *device)
{
	struct termios ti;
	int fd;

	fd = open(device, O_RDWR | O_NOCTTY);
	if (fd < 0)
		return -1;

	tcflush(fd, TCIOFLUSH);

	/* Switch TTY to raw mode */
	memset(&ti, 0, sizeof(ti));
	cfmakeraw(&ti);

	tcsetattr(fd, TCSANOW, &ti);

	return fd;
}

GAtMux *g_at_mux_new_from_tty(const char *device)
{
	GIOChannel *channel;
	int fd;

	fd = open_device(device);
	if (fd < 0)
		return NULL;

	channel = g_io_channel_unix_new(fd);
	if (!channel) {
		close(fd);
		return NULL;
	}

	return g_at_mux_new(channel);
}

GAtMux *g_at_mux_ref(GAtMux *mux)
{
	if (mux == NULL)
		return NULL;

	g_atomic_int_inc(&mux->ref_count);

	return mux;
}

void g_at_mux_unref(GAtMux *mux)
{
	if (mux == NULL)
		return;

	if (g_atomic_int_dec_and_test(&mux->ref_count)) {
		g_at_mux_shutdown(mux);

		g_free(mux);
	}
}

gboolean g_at_mux_shutdown(GAtMux *mux)
{
	if (mux->channel == NULL)
		return FALSE;

	return TRUE;
}

gboolean g_at_mux_set_disconnect_function(GAtMux *mux,
			GAtDisconnectFunc disconnect, gpointer user_data)
{
	if (mux == NULL)
		return FALSE;

	mux->user_disconnect = disconnect;
	mux->user_disconnect_data = user_data;

	return TRUE;
}

gboolean g_at_mux_set_debug(GAtMux *mux, GAtDebugFunc func, gpointer user)
{
	if (mux == NULL)
		return FALSE;

	mux->debugf = func;
	mux->debug_data = user;

	return TRUE;
}
