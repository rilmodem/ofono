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
	int slot_3g, numint;

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
	slot_3g = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel", __func__);
		goto error;
	}

	g_ril_append_print_buf(gril, "{%d}", slot_3g);
	g_ril_print_response(gril, message);

	/* Do it zero-based */
	return slot_3g - 1;

error:
	return -1;
}

int g_mtk_reply_parse_query_modem_type(GRil *gril,
					const struct ril_msg *message)
{
	struct parcel rilp;
	int numint, type;

	g_ril_init_parcel(message, &rilp);

	numint = parcel_r_int32(&rilp);
	if (numint != 1) {
		ofono_error("%s Wrong format", __func__);
		goto error;
	}

	type = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel", __func__);
		goto error;
	}

	g_ril_append_print_buf(gril, "{%d}", type);
	g_ril_print_response(gril, message);

	return type;

error:
	return -1;

}
