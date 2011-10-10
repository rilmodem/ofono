/*
 *
 *  AT chat library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009  Trolltech ASA.
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
typedef struct _GAtMuxDriver GAtMuxDriver;
typedef enum _GAtMuxChannelStatus GAtMuxChannelStatus;
typedef void (*GAtMuxSetupFunc)(GAtMux *mux, gpointer user_data);

enum _GAtMuxDlcStatus {
	G_AT_MUX_DLC_STATUS_RTC = 0x02,
	G_AT_MUX_DLC_STATUS_RTR = 0x04,
	G_AT_MUX_DLC_STATUS_IC = 0x08,
	G_AT_MUX_DLC_STATUS_DV = 0x80,
};

struct _GAtMuxDriver {
	void (*remove)(GAtMux *mux);
	gboolean (*startup)(GAtMux *mux);
	gboolean (*shutdown)(GAtMux *mux);
	gboolean (*open_dlc)(GAtMux *mux, guint8 dlc);
	gboolean (*close_dlc)(GAtMux *mux, guint8 dlc);
	void (*set_status)(GAtMux *mux, guint8 dlc, guint8 status);
	void (*write)(GAtMux *mux, guint8 dlc, const void *data, int towrite);
	int (*feed_data)(GAtMux *mux, void *data, int len);
};

GAtMux *g_at_mux_new(GIOChannel *channel, const GAtMuxDriver *driver);
GAtMux *g_at_mux_new_gsm0710_basic(GIOChannel *channel, int framesize);
GAtMux *g_at_mux_new_gsm0710_advanced(GIOChannel *channel, int framesize);

GAtMux *g_at_mux_ref(GAtMux *mux);
void g_at_mux_unref(GAtMux *mux);

gboolean g_at_mux_start(GAtMux *mux);
gboolean g_at_mux_shutdown(GAtMux *mux);

gboolean g_at_mux_set_disconnect_function(GAtMux *mux,
			GAtDisconnectFunc disconnect, gpointer user_data);

gboolean g_at_mux_set_debug(GAtMux *mux, GAtDebugFunc func, gpointer user_data);

GIOChannel *g_at_mux_create_channel(GAtMux *mux);

/*!
 * Multiplexer driver integration functions
 */
void g_at_mux_set_dlc_status(GAtMux *mux, guint8 dlc, int status);
void g_at_mux_feed_dlc_data(GAtMux *mux, guint8 dlc,
				const void *data, int tofeed);

int g_at_mux_raw_write(GAtMux *mux, const void *data, int towrite);

void g_at_mux_set_data(GAtMux *mux, void *data);
void *g_at_mux_get_data(GAtMux *mux);

/*!
 * Uses the passed in GAtChat to setup a GSM 07.10 style multiplexer on the
 * channel used by GAtChat.  This function queries the multiplexer capability,
 * preferring advanced mode over basic.  If supported, the best available
 * multiplexer mode is entered.  If this is successful, the chat is
 * shutdown and unrefed.  The chat's channel will be transferred to the
 * resulting multiplexer object.
 */
gboolean g_at_mux_setup_gsm0710(GAtChat *chat,
				GAtMuxSetupFunc notify, gpointer user_data,
				GDestroyNotify destroy);

#ifdef __cplusplus
}
#endif

#endif /* __GATMUX_H */
