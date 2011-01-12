/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gatchat.h"
#include "gatresult.h"

#include "nwmodem.h"

static const char *none_prefix[] = { NULL };
static const char *nwrat_prefix[] = { "$NWRAT:", NULL };

struct radio_settings_data {
	GAtChat *chat;
};

static void nwrat_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	enum ofono_radio_access_mode mode;
	struct ofono_error error;
	GAtResultIter iter;
	int value;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "$NWRAT:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &value) == FALSE)
		goto error;

	switch (value) {
	case 0:
		mode = OFONO_RADIO_ACCESS_MODE_ANY;
		break;
	case 1:
		mode = OFONO_RADIO_ACCESS_MODE_GSM;
		break;
	case 2:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	cb(&error, mode, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void nw_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(rsd->chat, "AT$NWRAT?", nwrat_prefix,
					nwrat_query_cb, cbd, g_free) == 0) {
		CALLBACK_WITH_FAILURE(cb, -1, data);
		g_free(cbd);
	}
}

static void nwrat_modify_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void nw_set_rat_mode(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[20];
	int value = 0;

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		value = 0;
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		value = 1;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		value = 2;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		goto error;
	}

	snprintf(buf, sizeof(buf), "AT$NWRAT=%u,2", value);

	if (g_at_chat_send(rsd->chat, buf, none_prefix,
					nwrat_modify_cb, cbd, g_free) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void nwrat_support_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;

	if (!ok)
		return;

	ofono_radio_settings_register(rs);
}

static int nw_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct radio_settings_data *rsd;

	rsd = g_try_new0(struct radio_settings_data, 1);
	if (rsd == NULL)
		return -ENOMEM;

	rsd->chat = g_at_chat_clone(chat);

	ofono_radio_settings_set_data(rs, rsd);

	g_at_chat_send(rsd->chat, "AT$NWRAT=?", nwrat_prefix,
					nwrat_support_cb, rs, NULL);

	return 0;
}

static void nw_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);

	ofono_radio_settings_set_data(rs, NULL);

	g_at_chat_unref(rsd->chat);
	g_free(rsd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= "nwmodem",
	.probe			= nw_radio_settings_probe,
	.remove			= nw_radio_settings_remove,
	.query_rat_mode		= nw_query_rat_mode,
	.set_rat_mode		= nw_set_rat_mode
};

void nw_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void nw_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
