/*
 *
 *  MTK driver for ofono/rilmodem
 *
 *  Copyright (C) 2014  Canonical Ltd.
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

#ifndef MTKREQUEST_H
#define MTKREQUEST_H

#include <ofono/types.h>
#include <ofono/sim.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTK_SWITCH_MODE_SIM_1_ACTIVE 1
#define MTK_SWITCH_MODE_SIM_2_ACTIVE 2
#define MTK_SWITCH_MODE_ALL_INACTIVE -1

#define MTK_CALL_INDIC_MODE_AVAIL 0
#define MTK_CALL_INDIC_MODE_NOT_AVAIL 1

/*
 * The meaning of mode seems to be:
 * -1 -> Both SIMs inactive
 * 1 -> SIM 1 active
 * 2 -> SIM 2 active
 * 3 -> Both SIMs active
 */
void g_mtk_request_dual_sim_mode_switch(GRil *gril,
					int mode,
					struct parcel *rilp);

/* 0:WHEN_NEEDED , 1: ALWAYS */
void g_mtk_request_set_gprs_connect_type(GRil *gril,
						int always,
						struct parcel *rilp);

/* 0:data prefer , 1: call prefer */
void g_mtk_request_set_gprs_transfer_type(GRil *gril,
						int callprefer,
						struct parcel *rilp);

/*
 * mode. MTK_CALL_INDIC_MODE_AVAIL: indicates that the specified call can be
 *       answered.
 *       MTK_CALL_INDIC_MODE_NOT_AVAIL: indicates we are busy, and that the
 *       specified call cannot be handled
 * "mode" seems useful for full dual SIM. An example would be that we have an
 * active call and an incoming call from the other SIM is signalled.
 *
 * call_id and seq_number are used to identify a specific call in the modem and
 * are taken from a previous RIL_UNSOL_INCOMING_CALL_INDICATION.
 */
void g_mtk_request_set_call_indication(GRil *gril, int mode, int call_id,
					int seq_number, struct parcel *rilp);

#ifdef __cplusplus
}
#endif

#endif /* MTKREQUEST_H */
