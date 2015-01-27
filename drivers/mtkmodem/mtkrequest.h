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

#define MTK_GPRS_CONNECT_WHEN_NEEDED 0
#define MTK_GPRS_CONNECT_ALWAYS 1

#define MTK_GPRS_TRANSFER_DATA_PREFER 0
#define MTK_GPRS_TRANSFER_CALL_PREFER 1

#define MTK_CALL_INDIC_MODE_AVAIL 0
#define MTK_CALL_INDIC_MODE_NOT_AVAIL 1

#define MTK_FD_MODE_DISABLE 0
#define MTK_FD_MODE_ENABLE 1
#define MTK_FD_MODE_SET_TIMER 2
#define MTK_FD_MODE_SCREEN_STATUS 3

#define MTK_FD_PAR1_SCREEN_OFF 0
#define MTK_FD_PAR1_SCREEN_ON 1

#define MTK_MD_TYPE_INVALID 0
#define MTK_MD_TYPE_2G 1
#define MTK_MD_TYPE_3G 2
#define MTK_MD_TYPE_WG 3
#define MTK_MD_TYPE_TG 4
/* FDD CSFB modem */
#define MTK_MD_TYPE_LWG 5
/* TDD CSFB modem */
#define MTK_MD_TYPE_LTG 6
/* SGLTE modem */
#define MTK_MD_TYPE_LTNG 7

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

/*
 * mode . MTK_FD_MODE_DISABLE: Disable fast dormancy
 *        MTK_FD_MODE_ENABLE: Enable fast formancy
 *        MTK_FD_MODE_SET_TIMER: Set fast dormancy timer
 *        MTK_FD_MODE_SCREEN_STATUS: Inform modem of screen status, to select
 *                                   timers to use for FD.
 * param1 Id of timer to set or screen status
 * param2 Value of timer, in units of 0.1 seconds
 *
 * Setting timers and enabling FD is done by rild initialization, so we do not
 * need to do it again in ofono.
 */
void g_mtk_request_set_fd_mode(GRil *gril, int mode, int param1,
					int param2, struct parcel *rilp);

/*
 * Sets the slot to which the GRil object is connected as the one with 3G
 * capabilities. This makes the other slot just 2G.
 */
void g_mtk_request_set_3g_capability(GRil *gril, struct parcel *rilp);

/*
 * Set the MTK modem mode. See MTK_MD_TYPE_*.
 */
void g_mtk_request_store_modem_type(GRil *gril, int mode, struct parcel *rilp);

/*
 * Set TRM
 */
void g_mtk_request_set_trm(GRil *gril, int trm, struct parcel *rilp);

#ifdef __cplusplus
}
#endif

#endif /* MTKREQUEST_H */
