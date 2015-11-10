/*
 *
 *  RIL chat library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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

#ifndef __GRILIO_H
#define __GRILIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gfunc.h"

#define GRIL_BUFFER_SIZE 8192

struct _GRilIO;

typedef struct _GRilIO GRilIO;

struct ring_buffer;

typedef void (*GRilIOReadFunc)(struct ring_buffer *buffer, gpointer user_data);
typedef gboolean (*GRilIOWriteFunc)(gpointer user_data);

GRilIO *g_ril_io_new(GIOChannel *channel);
GRilIO *g_ril_io_new_blocking(GIOChannel *channel);

GIOChannel *g_ril_io_get_channel(GRilIO *io);

GRilIO *g_ril_io_ref(GRilIO *io);
void g_ril_io_unref(GRilIO *io);

gboolean g_ril_io_set_read_handler(GRilIO *io, GRilIOReadFunc read_handler,
					gpointer user_data);
gboolean g_ril_io_set_write_handler(GRilIO *io, GRilIOWriteFunc write_handler,
					gpointer user_data);
void g_ril_io_set_write_done(GRilIO *io, GRilDisconnectFunc func,
				gpointer user_data);

void g_ril_io_drain_ring_buffer(GRilIO *io, guint len);

gsize g_ril_io_write(GRilIO *io, const gchar *data, gsize count);

gboolean g_ril_io_set_disconnect_function(GRilIO *io,
			GRilDisconnectFunc disconnect, gpointer user_data);

gboolean g_ril_io_set_debug(GRilIO *io, GRilDebugFunc func, gpointer user_data);

#ifdef __cplusplus
}
#endif

#endif /* __GRILIO_H */
