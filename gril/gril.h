/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
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

#ifndef __GRIL_H
#define __GRIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "grilio.h"
#include "grilutil.h"
#include "parcel.h"
#include "ril_constants.h"
#include "drivers/rilmodem/vendor.h"

#define RIL_MAX_NUM_ACTIVE_DATA_CALLS 2

struct _GRil;

typedef struct _GRil GRil;

/*
 * This struct represents an entire RIL message read
 * from the command socket.  It can hold responses or
 * unsolicited requests from RILD.
 */
struct ril_msg {
	gchar *buf;
	gsize buf_len;
	gboolean unsolicited;
	int req;
	int serial_no;
	int error;
};

typedef void (*GRilResponseFunc)(struct ril_msg *message, gpointer user_data);

typedef void (*GRilNotifyFunc)(struct ril_msg *message, gpointer user_data);

typedef const char *(*GRilMsgIdToStrFunc)(int msg_id);

/**
 * TRACE:
 * @fmt: format string
 * @arg...: list of arguments
 *
 * Simple macro around ofono_debug() used for tracing RIL messages
 * name it is called in.
 */
#define G_RIL_TRACE(gril, fmt, arg...) do {	\
	if (gril && g_ril_get_trace(gril))	\
		ofono_debug(fmt, ## arg);	\
} while (0)

extern char print_buf[];

#define g_ril_print_request(gril, token, req)				\
	G_RIL_TRACE(gril, "[%d,%04d]> %s %s",				\
		g_ril_get_slot(gril), token,				\
		g_ril_request_id_to_string(gril, req), print_buf)
#define g_ril_print_request_no_args(gril, token, req)			\
	G_RIL_TRACE(gril, "[%d,%04d]> %s",				\
			g_ril_get_slot(gril), token,			\
			g_ril_request_id_to_string(gril, req))
#define g_ril_print_response(gril, message)				\
	G_RIL_TRACE(gril, "[%d,%04d]< %s %s",				\
			g_ril_get_slot(gril),				\
			message->serial_no,				\
			g_ril_request_id_to_string(gril, message->req), \
							print_buf)
#define g_ril_print_response_no_args(gril, message)			\
	G_RIL_TRACE(gril, "[%d,%04d]< %s",				\
		g_ril_get_slot(gril), message->serial_no,		\
			g_ril_request_id_to_string(gril, message->req))

#define g_ril_append_print_buf(gril, x...) do {	\
	if (gril && g_ril_get_trace(gril))	\
		sprintf(print_buf, x);		\
} while (0)

#define g_ril_print_unsol(gril, message)				\
	G_RIL_TRACE(gril, "[%d,UNSOL]< %s %s",				\
			g_ril_get_slot(gril),				\
			g_ril_unsol_request_to_string(gril,		\
							message->req),	\
			print_buf)
#define g_ril_print_unsol_no_args(gril, message)			\
	G_RIL_TRACE(gril, "[%d,UNSOL]< %s", g_ril_get_slot(gril),	\
			g_ril_unsol_request_to_string(gril, message->req))

void g_ril_init_parcel(const struct ril_msg *message, struct parcel *rilp);

GRil *g_ril_new(const char *sock_path, enum ofono_ril_vendor vendor);

GIOChannel *g_ril_get_channel(GRil *ril);
GRilIO *g_ril_get_io(GRil *ril);

GRil *g_ril_ref(GRil *ril);
void g_ril_unref(GRil *ril);

GRil *g_ril_clone(GRil *ril);

void g_ril_set_disconnect_function(GRil *ril, GRilDisconnectFunc disconnect,
					gpointer user_data);

gboolean g_ril_get_trace(GRil *ril);
gboolean g_ril_set_trace(GRil *ril, gboolean trace);

int g_ril_get_slot(GRil *ril);
gboolean g_ril_set_slot(GRil *ril, int slot);

/*!
 * If the function is not NULL, then on every read/write from the GIOChannel
 * provided to GRil the logging function will be called with the
 * input/output string and user data
 */
gboolean g_ril_set_debugf(GRil *ril, GRilDebugFunc func, gpointer user_data);

gboolean g_ril_set_vendor_print_msg_id_funcs(GRil *ril,
					GRilMsgIdToStrFunc req_to_string,
					GRilMsgIdToStrFunc unsol_to_string);


/*!
 * Queue an RIL request for execution.  The request contents are given
 * in data.  Once the command executes, the callback function given by
 * func is called with user provided data in user_data.
 *
 * Returns an id of the queued command which can be canceled using
 * g_ril_cancel.  If an error occurred, an id of 0 is returned.
 *
 */
gint g_ril_send(GRil *ril, const gint reqid, struct parcel *rilp,
		GRilResponseFunc func, gpointer user_data,
		GDestroyNotify notify);

guint g_ril_register(GRil *ril, const int req,
			GRilNotifyFunc func, gpointer user_data);

gboolean g_ril_unregister(GRil *ril, guint id);
gboolean g_ril_unregister_all(GRil *ril);

enum ofono_ril_vendor g_ril_vendor(GRil *ril);

const char *g_ril_request_id_to_string(GRil *ril, int req);
const char *g_ril_unsol_request_to_string(GRil *ril, int req);

#ifdef __cplusplus
}
#endif

#endif /* __GRIL_H */
