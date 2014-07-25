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

#include <glib.h>

#include <ofono/log.h>

#include "mtkreply.h"

int g_mtk_reply_parse_get_3g_capability(GRil *gril,
					const struct ril_msg *message)
{
	struct parcel rilp;
	int sim_3g, numint, slot;

	g_ril_init_parcel(message, &rilp);

	numint = parcel_r_int32(&rilp);
	if (numint < 1) {
		ofono_error("%s: wrong format", __func__);
		goto error;
	}

	/*
	 * Bitmap with 3g capability per slot. Reply is the same regardless of
	 * the socket used to sent the request.
	 */
	sim_3g = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel", __func__);
		goto error;
	}

	g_ril_append_print_buf(gril, "{%d}", sim_3g);
	g_ril_print_response(gril, message);

	slot = g_ril_get_slot(gril);

	return (sim_3g >> slot) & 0x01;

error:
	return -1;
}
