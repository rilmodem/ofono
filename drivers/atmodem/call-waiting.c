/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <glib.h>

#include <ofono/log.h>
#include "driver.h"

#include "gatchat.h"
#include "gatresult.h"

#include "at.h"

static const char *none_prefix[] = { NULL };
static const char *ccwa_prefix[] = { "+CCWA:", NULL };

static void ccwa_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_waiting_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int num = 0;
	struct ofono_cw_condition *list = NULL;
	int i;

	dump_response("ccwa_query_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto out;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CCWA:"))
		num += 1;

	/* Specification is really unclear about this
	 * generate status=0 for all classes just in case
	 */
	if (num == 0) {
		list = g_new(struct ofono_cw_condition, 1);
		num = 1;

		list->status = 0;
		list->cls = GPOINTER_TO_INT(cbd->user);

		goto out;
	}

	list = g_new(struct ofono_cw_condition, num);

	g_at_result_iter_init(&iter, result);
	num = 0;

	while (g_at_result_iter_next(&iter, "+CCWA:")) {
		g_at_result_iter_next_number(&iter, &(list[num].status));
		g_at_result_iter_next_number(&iter, &(list[num].cls));

		num += 1;
	}

	for (i = 0; i < num; i++)
		ofono_debug("ccwa_cb: %d, %d", list[i].status, list[i].cls);

out:
	cb(&error, num, list, cbd->data);
	g_free(list);
}

static void at_ccwa_query(struct ofono_modem *modem, int cls,
				ofono_call_waiting_status_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	cbd->user = GINT_TO_POINTER(cls);

	if (cls == 7)
		sprintf(buf, "AT+CCWA=1,2");
	else
		sprintf(buf, "AT+CCWA=1,2,%d", cls);

	if (g_at_chat_send(at->parser, buf, ccwa_prefix,
				ccwa_query_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, 0, NULL, data);
	}
}

static void ccwa_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("ccwa_set_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_ccwa_set(struct ofono_modem *modem, int mode, int cls,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CCWA=1,%d,%d", mode, cls);

	if (g_at_chat_send(at->parser, buf, none_prefix,
				ccwa_set_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static struct ofono_call_waiting_ops ops = {
	.query 	= at_ccwa_query,
	.set 	= at_ccwa_set
};

void at_call_waiting_init(struct ofono_modem *modem)
{
	ofono_call_waiting_register(modem, &ops);
}

void at_call_waiting_exit(struct ofono_modem *modem)
{
	ofono_call_waiting_unregister(modem);
}
