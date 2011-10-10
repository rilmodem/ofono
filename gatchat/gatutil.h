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

#ifndef __GATUTIL_H
#define __GATUTIL_H

#include "gat.h"

#ifdef __cplusplus
extern "C" {
#endif

void g_at_util_debug_chat(gboolean in, const char *str, gsize len,
				GAtDebugFunc debugf, gpointer user_data);

void g_at_util_debug_dump(gboolean in, const unsigned char *buf, gsize len,
				GAtDebugFunc debugf, gpointer user_data);

void g_at_util_debug_hexdump(gboolean in, const unsigned char *buf, gsize len,
				GAtDebugFunc debugf, gpointer user_data);

gboolean g_at_util_setup_io(GIOChannel *io, GIOFlags flags);

#ifdef __cplusplus
}
#endif

#endif /* __GATUTIL_H */
