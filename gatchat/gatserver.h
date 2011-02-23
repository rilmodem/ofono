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

#include "gatresult.h"
#include "gatutil.h"
#include "gatio.h"

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
	G_AT_SERVER_RESULT_EXT_ERROR = 256,
};

typedef enum _GAtServerResult GAtServerResult;

/* Types of AT command:
 * COMMAND_ONLY: command without any sub-parameters, e.g. ATA, AT+CLCC
 * QUERY: command followed by '?', e.g. AT+CPIN?
 * SUPPORT: command followed by '=?', e.g. AT+CSMS=?
 * SET: command followed by '=', e.g. AT+CLIP=1
 * 	or, basic command followed with sub-parameters, e.g. ATD12345;
 */
enum _GAtServerRequestType {
	G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY,
	G_AT_SERVER_REQUEST_TYPE_QUERY,
	G_AT_SERVER_REQUEST_TYPE_SUPPORT,
	G_AT_SERVER_REQUEST_TYPE_SET,
};

typedef enum _GAtServerRequestType GAtServerRequestType;

typedef void (*GAtServerNotifyFunc)(GAtServer *server,
					GAtServerRequestType type,
					GAtResult *result, gpointer user_data);

GAtServer *g_at_server_new(GIOChannel *io);
GIOChannel *g_at_server_get_channel(GAtServer *server);
GAtIO *g_at_server_get_io(GAtServer *server);

GAtServer *g_at_server_ref(GAtServer *server);
void g_at_server_suspend(GAtServer *server);
void g_at_server_resume(GAtServer *server);
void g_at_server_unref(GAtServer *server);

gboolean g_at_server_shutdown(GAtServer *server);

gboolean g_at_server_set_echo(GAtServer *server, gboolean echo);
gboolean g_at_server_set_disconnect_function(GAtServer *server,
					GAtDisconnectFunc disconnect,
					gpointer user_data);
gboolean g_at_server_set_debug(GAtServer *server,
					GAtDebugFunc func,
					gpointer user_data);

gboolean g_at_server_register(GAtServer *server, const char *prefix,
					GAtServerNotifyFunc notify,
					gpointer user_data,
					GDestroyNotify destroy_notify);
gboolean g_at_server_unregister(GAtServer *server, const char *prefix);

/* Send a final result code. E.g. G_AT_SERVER_RESULT_NO_DIALTONE */
void g_at_server_send_final(GAtServer *server, GAtServerResult result);

/* Send an extended final result code. E.g. +CME ERROR: SIM failure. */
void g_at_server_send_ext_final(GAtServer *server, const char *result);

/* Send an intermediate result code to report the progress. E.g. CONNECT */
void g_at_server_send_intermediate(GAtServer *server, const char *result);

/* Send an unsolicited result code. E.g. RING */
void g_at_server_send_unsolicited(GAtServer *server, const char *result);

/*
 * Send a single response line for the command.  The line should be no longer
 * than 2048 characters.  If the response contains multiple lines, use
 * FALSE for the 'last' parameter for lines 1 .. n -1, and 'TRUE' for the last
 * line.  This is required for formatting of 27.007 compliant multi-line
 * responses.
 */
void g_at_server_send_info(GAtServer *server, const char *line, gboolean last);

#ifdef __cplusplus
}
#endif

#endif /* __GATSERVER_H */
