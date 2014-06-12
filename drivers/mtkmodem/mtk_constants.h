/*
 *
 *  RIL constants for MTK modem
 *
 *  Copyright (C) 2014 Canonical Ltd.
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

#ifndef MTK_CONSTANTS_H
#define MTK_CONSTANTS_H

/* RIL Request Messages */
#define RIL_REQUEST_RADIO_POWEROFF 2011
#define RIL_REQUEST_DUAL_SIM_MODE_SWITCH 2012
#define RIL_REQUEST_SET_GPRS_CONNECT_TYPE 2016
#define RIL_REQUEST_SET_GPRS_TRANSFER_TYPE 2017
#define RIL_REQUEST_RADIO_POWERON 2033
#define RIL_REQUEST_SET_CALL_INDICATION 2036
#define RIL_REQUEST_SET_FD_MODE 2073

/* RIL Unsolicited Messages */
#define RIL_UNSOL_RADIO_TEMPORARILY_UNAVAILABLE 3008
#define RIL_UNSOL_CALL_PROGRESS_INFO 3004
#define RIL_UNSOL_INCOMING_CALL_INDICATION 3014
#define RIL_UNSOL_RESPONSE_PS_NETWORK_STATE_CHANGED 3022

#endif /* MTK_CONSTANTS_H */
