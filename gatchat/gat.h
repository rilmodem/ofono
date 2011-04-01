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

#ifndef __GAT_H
#define __GAT_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GAtDisconnectFunc)(gpointer user_data);
typedef void (*GAtReceiveFunc)(const unsigned char *data, gsize size,
							gpointer user_data);
typedef void (*GAtDebugFunc)(const char *str, gpointer user_data);
typedef void (*GAtSuspendFunc)(gpointer user_data);

#ifdef __cplusplus
}
#endif

#endif /* __GAT_H */
