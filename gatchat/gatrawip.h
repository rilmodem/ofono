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

#ifndef __G_AT_RAWIP_H
#define __G_AT_RAWIP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gat.h"
#include "gatio.h"

struct _GAtRawIP;

typedef struct _GAtRawIP GAtRawIP;

GAtRawIP *g_at_rawip_new(GIOChannel *channel);
GAtRawIP *g_at_rawip_new_from_io(GAtIO *io);

GAtRawIP *g_at_rawip_ref(GAtRawIP *rawip);
void g_at_rawip_unref(GAtRawIP *rawip);

void g_at_rawip_open(GAtRawIP *rawip);
void g_at_rawip_shutdown(GAtRawIP *rawip);

const char *g_at_rawip_get_interface(GAtRawIP *rawip);

void g_at_rawip_set_debug(GAtRawIP *rawip, GAtDebugFunc func,
						gpointer user_data);

#ifdef __cplusplus
}
#endif

#endif /* __G_AT_RAWIP_H */
