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

#include <stddef.h>

#include "mtkutil.h"
#include "mtk_constants.h"

const char *mtk_request_id_to_string(int req)
{
	switch (req) {
	case RIL_REQUEST_RADIO_POWEROFF:
		return "RIL_REQUEST_RADIO_POWEROFF";
	case RIL_REQUEST_DUAL_SIM_MODE_SWITCH:
		return "RIL_REQUEST_DUAL_SIM_MODE_SWITCH";
	case RIL_REQUEST_SET_GPRS_CONNECT_TYPE:
		return "RIL_REQUEST_SET_GPRS_CONNECT_TYPE";
	case RIL_REQUEST_SET_GPRS_TRANSFER_TYPE:
		return "RIL_REQUEST_SET_GPRS_TRANSFER_TYPE";
	case RIL_REQUEST_RADIO_POWERON:
		return "RIL_REQUEST_RADIO_POWERON";
	case RIL_REQUEST_SET_CALL_INDICATION:
		return "RIL_REQUEST_SET_CALL_INDICATION";
	case RIL_REQUEST_GET_3G_CAPABILITY:
		return "RIL_REQUEST_GET_3G_CAPABILITY";
	case RIL_REQUEST_SET_FD_MODE:
		return "RIL_REQUEST_SET_FD_MODE";
	default:
		return NULL;
	}
}

const char *mtk_unsol_request_to_string(int req)
{
	switch (req) {
	case RIL_UNSOL_RADIO_TEMPORARILY_UNAVAILABLE:
		return "RIL_UNSOL_RADIO_TEMPORARILY_UNAVAILABLE";
        case RIL_UNSOL_CALL_PROGRESS_INFO:
        	return "RIL_UNSOL_CALL_PROGRESS_INFO";
        case RIL_UNSOL_INCOMING_CALL_INDICATION:
        	return "RIL_UNSOL_INCOMING_CALL_INDICATION";
	case RIL_UNSOL_RESPONSE_PS_NETWORK_STATE_CHANGED:
		return "RIL_UNSOL_RESPONSE_PS_NETWORK_STATE_CHANGED";
	default:
		return NULL;
	}
}
