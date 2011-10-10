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

#ifndef __G_AT_HDLC_H
#define __G_AT_HDLC_H

#include "gat.h"
#include "gatio.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _GAtHDLC;

typedef struct _GAtHDLC GAtHDLC;

GAtHDLC *g_at_hdlc_new(GIOChannel *channel);
GAtHDLC *g_at_hdlc_new_from_io(GAtIO *io);

GAtHDLC *g_at_hdlc_ref(GAtHDLC *hdlc);
void g_at_hdlc_unref(GAtHDLC *hdlc);

void g_at_hdlc_set_debug(GAtHDLC *hdlc, GAtDebugFunc func, gpointer user_data);

void g_at_hdlc_set_xmit_accm(GAtHDLC *hdlc, guint32 accm);
guint32 g_at_hdlc_get_xmit_accm(GAtHDLC *hdlc);

void g_at_hdlc_set_recv_accm(GAtHDLC *hdlc, guint32 accm);
guint32 g_at_hdlc_get_recv_accm(GAtHDLC *hdlc);

void g_at_hdlc_set_receive(GAtHDLC *hdlc, GAtReceiveFunc func,
							gpointer user_data);
gboolean g_at_hdlc_send(GAtHDLC *hdlc, const unsigned char *data, gsize size);

void g_at_hdlc_set_recording(GAtHDLC *hdlc, const char *filename);

GAtIO *g_at_hdlc_get_io(GAtHDLC *hdlc);

void g_at_hdlc_set_start_frame_marker(GAtHDLC *hdlc, gboolean marker);
void g_at_hdlc_set_no_carrier_detect(GAtHDLC *hdlc, gboolean detect);

void g_at_hdlc_set_suspend_function(GAtHDLC *hdlc, GAtSuspendFunc func,
							gpointer user_data);

void g_at_hdlc_suspend(GAtHDLC *hdlc);
void g_at_hdlc_resume(GAtHDLC *hdlc);

#ifdef __cplusplus
}
#endif

#endif /* __G_AT_HDLC_H */
