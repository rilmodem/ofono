/*
 *
 *  AT chat library with GLib integration
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

#ifndef __GATIO_H
#define __GATIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gat.h"

struct _GAtIO;

typedef struct _GAtIO GAtIO;

struct ring_buffer;

typedef void (*GAtIOReadFunc)(struct ring_buffer *buffer, gpointer user_data);
typedef gboolean (*GAtIOWriteFunc)(gpointer user_data);

GAtIO *g_at_io_new(GIOChannel *channel);
GAtIO *g_at_io_new_blocking(GIOChannel *channel);

GIOChannel *g_at_io_get_channel(GAtIO *io);

GAtIO *g_at_io_ref(GAtIO *io);
void g_at_io_unref(GAtIO *io);

gboolean g_at_io_set_read_handler(GAtIO *io, GAtIOReadFunc read_handler,
					gpointer user_data);
gboolean g_at_io_set_write_handler(GAtIO *io, GAtIOWriteFunc write_handler,
					gpointer user_data);
void g_at_io_set_write_done(GAtIO *io, GAtDisconnectFunc func,
				gpointer user_data);

void g_at_io_drain_ring_buffer(GAtIO *io, guint len);

gsize g_at_io_write(GAtIO *io, const gchar *data, gsize count);

gboolean g_at_io_set_disconnect_function(GAtIO *io,
			GAtDisconnectFunc disconnect, gpointer user_data);

gboolean g_at_io_set_debug(GAtIO *io, GAtDebugFunc func, gpointer user_data);

#ifdef __cplusplus
}
#endif

#endif /* __GATIO_H */
