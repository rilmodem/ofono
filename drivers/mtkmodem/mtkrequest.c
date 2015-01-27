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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <ctype.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "mtkrequest.h"
#include "simutil.h"
#include "util.h"
#include "common.h"

void g_mtk_request_dual_sim_mode_switch(GRil *gril,
					int mode,
					struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, mode);

	g_ril_append_print_buf(gril, "(%d)", mode);
}

void g_mtk_request_set_gprs_connect_type(GRil *gril,
						int always,
						struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, always);

	g_ril_append_print_buf(gril, "(%d)", always);
};

void g_mtk_request_set_gprs_transfer_type(GRil *gril,
						int callprefer,
						struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, callprefer);

	g_ril_append_print_buf(gril, "(%d)", callprefer);
}

void g_mtk_request_set_call_indication(GRil *gril, int mode, int call_id,
					int seq_number, struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 3);
	/* What the parameters set is unknown */
	parcel_w_int32(rilp, mode);
	parcel_w_int32(rilp, call_id);
	parcel_w_int32(rilp, seq_number);

	g_ril_append_print_buf(gril, "(%d,%d,%d)", mode, call_id, seq_number);
}

void g_mtk_request_set_fd_mode(GRil *gril, int mode, int param1,
					int param2, struct parcel *rilp)
{
	int num_args;

	switch (mode) {
	case MTK_FD_MODE_DISABLE:
	case MTK_FD_MODE_ENABLE:
		num_args = 1;
		break;
	case MTK_FD_MODE_SET_TIMER:
		num_args = 3;
		break;
	case MTK_FD_MODE_SCREEN_STATUS:
		num_args = 2;
		break;
	default:
		ofono_error("%s: mode %d is wrong", __func__, mode);
		return;
	}

	parcel_init(rilp);
	parcel_w_int32(rilp, num_args);
	parcel_w_int32(rilp, mode);

	g_ril_append_print_buf(gril, "(%d,%d", num_args, mode);

	if (mode == MTK_FD_MODE_SCREEN_STATUS) {
		parcel_w_int32(rilp, param1);
		g_ril_append_print_buf(gril, "%s,%d)", print_buf, param1);
	} else if (mode == MTK_FD_MODE_SET_TIMER) {
		parcel_w_int32(rilp, param1);
		parcel_w_int32(rilp, param2);
		g_ril_append_print_buf(gril, "%s,%d,%d)", print_buf,
					param1, param2);
	} else {
		g_ril_append_print_buf(gril, "%s)", print_buf);
	}
}

void g_mtk_request_set_3g_capability(GRil *gril, struct parcel *rilp)
{
	int mode = g_ril_get_slot(gril) + 1;

	parcel_init(rilp);
	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, mode);

	g_ril_append_print_buf(gril, "(%d)", mode);
}

void g_mtk_request_store_modem_type(GRil *gril, int mode, struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, mode);

	g_ril_append_print_buf(gril, "(%d)", mode);
}

void g_mtk_request_set_trm(GRil *gril, int trm, struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, 1);
	parcel_w_int32(rilp, trm);

	g_ril_append_print_buf(gril, "(%d)", trm);
}
