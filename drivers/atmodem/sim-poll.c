/*
 *
 *  oFono - Open Source Telephony
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>
#include <ofono/stk.h>

#include "gatchat.h"
#include "gatresult.h"
#include "ofono.h"

#include "atmodem.h"
#include "sim-poll.h"
#include "stk.h"

struct sim_poll_data {
	GAtChat *chat;
	struct ofono_modem *modem;
	struct ofono_sim *sim;
	struct ofono_stk *stk;
	unsigned int sim_watch;
	unsigned int stk_watch;
	unsigned int sim_state_watch;
	gboolean inserted;
	int idle_poll_interval;
	gint status_timeout;
	gint poll_timeout;
	guint status_cmd;
};

static const char *csim_prefix[] = { "+CSIM:", NULL };

static gboolean sim_status_poll(gpointer user_data);

static void sim_status_poll_schedule(struct sim_poll_data *spd)
{
	/* TODO: Decide on the interval based on whether any call is active */
	/* TODO: On idle, possibly only schedule if proactive commands enabled
	 * as indicated by EFphase + EFsst (51.011: 11.6.1) */
	int interval = spd->idle_poll_interval;

	/* When a SIM is inserted, the SIM might have requested a different
	 * interval.  */
	if (spd->inserted)
		interval = ofono_modem_get_integer(spd->modem,
				"status-poll-interval");

	spd->poll_timeout = g_timeout_add_seconds(interval,
			sim_status_poll, spd);
}

static gboolean sim_status_timeout(gpointer user_data)
{
	struct sim_poll_data *spd = user_data;

	spd->status_timeout = 0;

	g_at_chat_cancel(spd->chat, spd->status_cmd);
	spd->status_cmd = 0;

	if (spd->inserted == TRUE) {
		spd->inserted = FALSE;
		ofono_sim_inserted_notify(spd->sim, FALSE);
	}

	sim_status_poll_schedule(spd);

	return FALSE;
}

static void at_csim_status_cb(gboolean ok, GAtResult *result,
		gpointer user_data)
{
	struct sim_poll_data *spd = user_data;
	GAtResultIter iter;
	const guint8 *response;
	gint rlen, len;

	spd->status_cmd = 0;

	if (!spd->status_timeout)
		/* The STATUS already timed out */
		return;

	/* Card responded on time */

	g_source_remove(spd->status_timeout);
	spd->status_timeout = 0;

	if (spd->inserted != TRUE) {
		spd->inserted = TRUE;
		ofono_sim_inserted_notify(spd->sim, TRUE);
	}

	sim_status_poll_schedule(spd);

	/* Check if we have a proactive command */

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSIM:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &rlen))
		return;

	if (!g_at_result_iter_next_hexstring(&iter, &response, &len))
		return;

	if (rlen != len * 2 || len < 2)
		return;

	if (response[len - 2] != 0x91)
		return;

	/* We have a proactive command pending, FETCH it */
	at_sim_fetch_command(spd->stk, response[len - 1]);
}

static gboolean sim_status_poll(gpointer user_data)
{
	struct sim_poll_data *spd = user_data;

	spd->poll_timeout = 0;

	/* The SIM must respond in a given time frame which is of at
	 * least 5 seconds in TS 11.11.  */
	spd->status_timeout = g_timeout_add_seconds(5,
			sim_status_timeout, spd);

	/* Send STATUS */
	spd->status_cmd = g_at_chat_send(spd->chat, "AT+CSIM=8,A0F200C0",
			csim_prefix, at_csim_status_cb, spd, NULL);
	if (spd->status_cmd == 0)
		at_csim_status_cb(FALSE, NULL, spd);

	return FALSE;
}

static void sim_state_watch(enum ofono_sim_state new_state, void *user)
{
	struct sim_poll_data *spd = user;

	spd->inserted = new_state != OFONO_SIM_STATE_NOT_PRESENT;

	if (!spd->inserted)
		ofono_modem_set_integer(spd->modem,
				"status-poll-interval", 30);
}

static void sim_watch(struct ofono_atom *atom,
		enum ofono_atom_watch_condition cond, void *data)
{
	struct sim_poll_data *spd = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		spd->sim = __ofono_atom_get_data(atom);

		spd->sim_state_watch = ofono_sim_add_state_watch(spd->sim,
				sim_state_watch, spd, NULL);
		sim_state_watch(ofono_sim_get_state(spd->sim), spd);

		sim_status_poll(spd);

		return;
	}

	if (cond != OFONO_ATOM_WATCH_CONDITION_UNREGISTERED)
		return;

	spd->inserted = FALSE;

	spd->sim_state_watch = 0;

	if (spd->sim_watch) {
		__ofono_modem_remove_atom_watch(spd->modem, spd->sim_watch);
		spd->sim_watch = 0;
	}

	if (spd->stk_watch) {
		__ofono_modem_remove_atom_watch(spd->modem, spd->stk_watch);
		spd->stk_watch = 0;
	}

	if (spd->status_timeout) {
		g_source_remove(spd->status_timeout);
		spd->status_timeout = 0;
	}

	if (spd->poll_timeout) {
		g_source_remove(spd->poll_timeout);
		spd->poll_timeout = 0;
	}

	g_free(spd);
}

static void stk_watch(struct ofono_atom *atom,
		enum ofono_atom_watch_condition cond, void *data)
{
	struct sim_poll_data *spd = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED)
		spd->stk = __ofono_atom_get_data(atom);
	else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED)
		spd->stk = NULL;
}

void atmodem_poll_enable(struct ofono_modem *modem, GAtChat *chat)
{
	struct sim_poll_data *spd;

	if (__ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SIM) == NULL)
		return;

	spd = g_new0(struct sim_poll_data, 1);
	spd->chat = chat;
	spd->modem = modem;
	spd->idle_poll_interval = 30;

	spd->stk_watch = __ofono_modem_add_atom_watch(spd->modem,
			OFONO_ATOM_TYPE_STK, stk_watch, spd, NULL);

	spd->sim_watch = __ofono_modem_add_atom_watch(spd->modem,
			OFONO_ATOM_TYPE_SIM, sim_watch, spd, NULL);
}
