/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "mtk_constants.h"
#include "mtkunsol.h"
#include "mtkrequest.h"

#include "common.h"
#include "mtkmodem.h"
#include "drivers/rilmodem/voicecall.h"

/*
 * This is the voicecall atom implementation for mtkmodem. Most of the
 * implementation can be found in the rilmodem atom. The main reason for
 * creating a new atom is the need to handle specific MTK requests and
 * unsolicited events.
 */

static void mtk_set_indication_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(vd->ril, message);

		/* Request the call list again */
		ril_poll_clcc(vc);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
	}
}

static void mtk_incoming_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;
	struct unsol_call_indication *call_ind;

	call_ind = g_mtk_unsol_parse_incoming_call_indication(vd->ril, message);
	if (call_ind == NULL) {
		ofono_error("%s: error parsing event", __func__);
		return;
	}

	g_mtk_request_set_call_indication(vd->ril, MTK_CALL_INDIC_MODE_AVAIL,
						call_ind->call_id,
						call_ind->seq_number, &rilp);

	if (g_ril_send(vd->ril, RIL_REQUEST_SET_CALL_INDICATION,
			&rilp, mtk_set_indication_cb, vc, NULL) == 0)
		ofono_error("%s: cannot send indication", __func__);

	g_mtk_unsol_free_call_indication(call_ind);
}

static gboolean mtk_delayed_register(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);

	/* MTK generates this event instead of CALL_STATE_CHANGED */
	g_ril_register(vd->ril, RIL_UNSOL_CALL_PROGRESS_INFO,
			ril_call_state_notify, vc);

	/* Indicates incoming call, before telling the network our state */
	g_ril_register(vd->ril, RIL_UNSOL_INCOMING_CALL_INDICATION,
			mtk_incoming_notify, vc);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int mtk_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct ril_voicecall_data *vd;

	vd = g_try_new0(struct ril_voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	ril_voicecall_start(ril, vc, vendor, vd);

	/*
	 * Register events after ofono_voicecall_register() is called from
	 * ril_delayed_register().
	 */
	g_idle_add(mtk_delayed_register, vc);

	return 0;
}

static struct ofono_voicecall_driver driver = {
	.name			= MTKMODEM,
	.probe			= mtk_voicecall_probe,
	.remove			= ril_voicecall_remove,
	.dial			= ril_dial,
	.answer			= ril_answer,
	.hangup_all		= ril_hangup_all,
	.release_specific	= ril_hangup_specific,
	.send_tones		= ril_send_dtmf,
	.create_multiparty	= ril_create_multiparty,
	.private_chat		= ril_private_chat,
	.swap_without_accept	= ril_swap_without_accept,
	.hold_all_active	= ril_hold_all_active,
	.release_all_held	= ril_release_all_held,
	.set_udub		= ril_set_udub,
	.release_all_active	= ril_release_all_active,
};

void mtk_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void mtk_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
