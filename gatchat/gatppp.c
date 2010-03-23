/*
 *
 *  PPP library with GLib integration
 *
 *  Copyright (C) 2009-2010  Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <arpa/inet.h>

#include <glib.h>

#include "gatutil.h"
#include "gatppp.h"
#include "ppp.h"

/* Administrative Open */
void g_at_ppp_open(GAtPPP *ppp)
{
	/* send an OPEN event to the lcp layer */
	lcp_open(ppp->lcp);
}

void g_at_ppp_set_credentials(GAtPPP *ppp, const char *username,
				const char *passwd)
{
	auth_set_credentials(ppp->auth, username, passwd);
}

void g_at_ppp_set_connect_function(GAtPPP *ppp,
			       GAtPPPConnectFunc callback, gpointer user_data)
{
	ppp->connect_cb = callback;
	ppp->connect_data = user_data;
}

void g_at_ppp_set_disconnect_function(GAtPPP *ppp,
				  GAtPPPDisconnectFunc callback,
				  gpointer user_data)
{
	ppp->disconnect_cb = callback;
	ppp->disconnect_data = user_data;
}

void g_at_ppp_shutdown(GAtPPP *ppp)
{
	/* close the ppp */
	ppp_close(ppp);

	/* clean up all the queues */
	g_queue_free(ppp->event_queue);
	g_queue_free(ppp->recv_queue);

	/* cleanup modem channel */
	g_source_remove(ppp->modem_watch);
	g_io_channel_unref(ppp->modem);

	/* remove lcp */
	lcp_free(ppp->lcp);

	/* remove auth */
	auth_free(ppp->auth);
}

void g_at_ppp_ref(GAtPPP *ppp)
{
	g_atomic_int_inc(&ppp->ref_count);
}

void g_at_ppp_unref(GAtPPP *ppp)
{
	if (g_atomic_int_dec_and_test(&ppp->ref_count)) {
		g_at_ppp_shutdown(ppp);
		g_free(ppp);
	}
}

GAtPPP *g_at_ppp_new(GIOChannel *modem)
{
	GAtPPP *ppp;

	ppp = g_try_malloc0(sizeof(GAtPPP));
	if (!ppp)
		return NULL;

	ppp->modem = g_io_channel_ref(modem);
	if (!g_at_util_setup_io(ppp->modem, G_IO_FLAG_NONBLOCK)) {
		g_io_channel_unref(modem);
		g_free(ppp);
		return NULL;
	}
	g_io_channel_set_buffered(modem, FALSE);

	ppp->ref_count = 1;

	/* set options to defaults */
	ppp->mru = DEFAULT_MRU;
	ppp->recv_accm = DEFAULT_ACCM;
	ppp->xmit_accm[0] = DEFAULT_ACCM;
	ppp->xmit_accm[3] = 0x60000000; /* 0x7d, 0x7e */
	ppp->pfc = FALSE;
	ppp->acfc = FALSE;

	/* allocate the queues */
	ppp->event_queue = g_queue_new();
	ppp->recv_queue = g_queue_new();

	ppp->index = 0;

	/* initialize the lcp state */
	ppp->lcp = lcp_new(ppp);

	/* initialize the autentication state */
	ppp->auth = auth_new(ppp);

	/* intialize the network state */
	ppp->net = ppp_net_new(ppp);

	/* start listening for packets from the modem */
	ppp->modem_watch = g_io_add_watch(modem,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			ppp_cb, ppp);

	return ppp;
}
