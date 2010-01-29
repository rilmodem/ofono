/*
 *
 *  AT Server library with GLib integration
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

#ifndef __GATSERVER_H
#define __GATSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gatutil.h"

struct _GAtServer;

typedef struct _GAtServer GAtServer;

/* V.250 Table 1/V.250 Result codes */
enum _GAtServerResult {
	G_AT_SERVER_RESULT_OK = 0,
	G_AT_SERVER_RESULT_CONNECT = 1,
	G_AT_SERVER_RESULT_RING = 2,
	G_AT_SERVER_RESULT_NO_CARRIER = 3,
	G_AT_SERVER_RESULT_ERROR = 4,
	G_AT_SERVER_RESULT_NO_DIALTONE = 6,
	G_AT_SERVER_RESULT_BUSY = 7,
	G_AT_SERVER_RESULT_NO_ANSWER = 8,
};

typedef enum _GAtServerResult GAtServerResult;

GAtServer *g_at_server_new(GIOChannel *io);

GAtServer *g_at_server_ref(GAtServer *server);
void g_at_server_unref(GAtServer *server);
gboolean g_at_server_shutdown(GAtServer *server);

gboolean g_at_server_set_disconnect_function(GAtServer *server,
					GAtDisconnectFunc disconnect,
					gpointer user_data);
gboolean g_at_server_set_debug(GAtServer *server,
					GAtDebugFunc func,
					gpointer user);

#ifdef __cplusplus
}
#endif

#endif /* __GATSERVER_H */
