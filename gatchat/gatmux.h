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

#ifndef __GATMUX_H
#define __GATMUX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gatchat.h"

struct _GAtMux;

typedef struct _GAtMux GAtMux;

GAtMux *g_at_mux_new(GIOChannel *channel);
GAtMux *g_at_mux_new_from_tty(const char *device);

GAtMux *g_at_mux_ref(GAtMux *mux);
void g_at_mux_unref(GAtMux *mux);

gboolean g_at_mux_start(GAtMux *mux);
gboolean g_at_mux_shutdown(GAtMux *mux);

gboolean g_at_mux_set_disconnect_function(GAtMux *mux,
			GAtDisconnectFunc disconnect, gpointer user_data);

gboolean g_at_mux_set_debug(GAtMux *mux, GAtDebugFunc func, gpointer user);

GIOChannel *g_at_mux_create_channel(GAtMux *mux);
GAtChat *g_at_mux_create_chat(GAtMux *mux, GAtSyntax *syntax);

#ifdef __cplusplus
}
#endif

#endif /* __GATMUX_H */
