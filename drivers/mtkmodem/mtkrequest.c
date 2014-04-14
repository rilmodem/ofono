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
