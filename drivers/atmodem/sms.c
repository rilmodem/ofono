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

static const char *csca_prefix[] = { "+CSCA", NULL };

static void at_csca_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	dump_response("csca_set_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_csca_set(struct ofono_modem *modem,
			const struct ofono_phone_number *sca,
			ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[64];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CSCA=\"%s\",%d", sca->number, sca->type);

	if (g_at_chat_send(at->parser, buf, csca_prefix,
				at_csca_set_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static void at_csca_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sca_query_cb_t cb = cbd->cb;
	struct ofono_error error;
	struct ofono_phone_number sca;
	const char *number;

	dump_response("at_csca_cb", ok, result);
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCA:"))
		goto err;

	if (!g_at_result_iter_next_string(&iter, &number))
		goto err;

	if (number[0] == '+') {
		number = number + 1;
		sca.type = 145;
	} else
		sca.type = 129;

	strncpy(sca.number, number, OFONO_MAX_PHONE_NUMBER_LENGTH);
	sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';

	g_at_result_iter_next_number(&iter, &sca.type);

	ofono_debug("csca_query_cb: %s, %d", sca.number, sca.type);

	cb(&error, &sca, cbd->data);

	return;

err:
	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, cbd->data);
	}
}

static void at_csca_query(struct ofono_modem *modem, ofono_sca_query_cb_t cb,
					void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);

	if (!cbd)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CSCA?", csca_prefix,
				at_csca_query_cb, cbd, g_free) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, NULL, data);
	}
}

static struct ofono_sms_ops ops = {
	.sca_query	= at_csca_query,
	.sca_set	= at_csca_set,
};

void at_sms_init(struct ofono_modem *modem)
{
	ofono_sms_manager_register(modem, &ops);
}

void at_sms_exit(struct ofono_modem *modem)
{
	ofono_sms_manager_unregister(modem);
}
