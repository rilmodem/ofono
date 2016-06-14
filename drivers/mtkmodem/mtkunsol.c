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

#include "mtkunsol.h"
#include "util.h"
#include "common.h"

void g_mtk_unsol_free_call_indication(struct unsol_call_indication *unsol)
{
	g_free(unsol);
}

struct unsol_call_indication *g_mtk_unsol_parse_incoming_call_indication(
					GRil *gril, struct ril_msg *message)
{
	struct parcel rilp;
	int numstr;
	struct unsol_call_indication *unsol;
	char *call_id, *phone, *address_type, *call_mode, *seq_num;
	char *fw_address = NULL;
	char *endp;

	unsol = g_try_malloc0(sizeof(*unsol));
	if (unsol == NULL) {
		ofono_error("%s: out of memory", __func__);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	numstr = parcel_r_int32(&rilp);
	if (numstr < 5) {
		ofono_error("%s: wrong array size (%d)", __func__, numstr);
		goto error;
	}

	call_id = parcel_r_string(&rilp);
	phone = parcel_r_string(&rilp);
	address_type = parcel_r_string(&rilp);
	call_mode = parcel_r_string(&rilp);
	seq_num = parcel_r_string(&rilp);
	if (numstr > 5)
		fw_address = parcel_r_string(&rilp);

	g_ril_append_print_buf(gril, "{%s,%s,%s,%s,%s,%s}",
				PRINTABLE_STR(call_id),
				PRINTABLE_STR(phone),
				PRINTABLE_STR(address_type),
				PRINTABLE_STR(call_mode),
				PRINTABLE_STR(seq_num),
				PRINTABLE_STR(fw_address));

	g_ril_print_unsol(gril, message);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel", __func__);
		goto err_conv;
	}

	if (call_id != NULL && *call_id != '\0') {
		unsol->call_id = (int) strtol(call_id, &endp, 10);
		if (*endp != '\0') {
			ofono_error("%s: cannot parse call id", __func__);
			goto err_conv;
		}
	} else {
		ofono_error("%s: no call id", __func__);
		goto err_conv;
	}

	if (call_mode != NULL && *call_mode != '\0') {
		unsol->call_mode = (int) strtol(call_mode, &endp, 10);
		if (*endp != '\0') {
			ofono_error("%s: cannot parse call mode", __func__);
			goto err_conv;
		}
	} else {
		ofono_error("%s: no call mode", __func__);
		goto err_conv;
	}

	if (seq_num != NULL && *seq_num != '\0') {
		unsol->seq_number = (int) strtol(seq_num, &endp, 10);
		if (*endp != '\0') {
			ofono_error("%s: cannot parse seq num", __func__);
			goto err_conv;
		}
	} else {
		ofono_error("%s: no seq num", __func__);
		goto err_conv;
	}

	g_free(call_id);
	g_free(phone);
	g_free(address_type);
	g_free(call_mode);
	g_free(seq_num);
	g_free(fw_address);

	return unsol;

err_conv:
	g_free(call_id);
	g_free(phone);
	g_free(address_type);
	g_free(call_mode);
	g_free(seq_num);
	g_free(fw_address);

error:
	g_free(unsol);

	return NULL;
}

int g_mtk_unsol_parse_registration_suspended(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	int numint, session_id;

	g_ril_init_parcel(message, &rilp);

	numint = parcel_r_int32(&rilp);
	if (numint != 1) {
		ofono_error("%s Wrong format", __func__);
		goto error;
	}

	session_id = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel", __func__);
		goto error;
	}

	g_ril_append_print_buf(gril, "{%d}", session_id);
	g_ril_print_unsol(gril, message);

	return session_id;

error:
	return -1;

}

struct parcel_str_array *g_mtk_unsol_parse_plmn_changed(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	struct parcel_str_array *str_arr;
	int i;

	g_ril_init_parcel(message, &rilp);

	str_arr = parcel_r_str_array(&rilp);
	if (str_arr == NULL || str_arr->num_str == 0) {
		ofono_error("%s: parse error for %s", __func__,
				ril_request_id_to_string(message->req));
		parcel_free_str_array(str_arr);
		str_arr = NULL;
		goto out;
	}

	g_ril_append_print_buf(gril, "{");

	for (i = 0; i < str_arr->num_str; ++i) {
		if (i + 1 == str_arr->num_str)
			g_ril_append_print_buf(gril, "%s%s}", print_buf,
						str_arr->str[i]);
		else
			g_ril_append_print_buf(gril, "%s%s, ", print_buf,
						str_arr->str[i]);
	}

	g_ril_print_unsol(gril, message);

out:
	return str_arr;
}
