/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
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

#include <string.h>
#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "gatchat.h"
#include "gatresult.h"

#include "cdmamodem.h"

static const char *gcap_prefix[] = { "+GCAP:", NULL };

static void attr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
         struct cb_data *cbd = user_data;
         ofono_devinfo_query_cb_t cb = cbd->cb;
         const char *prefix = cbd->user;
         struct ofono_error error;
         const char *attr;

         decode_at_error(&error, g_at_result_final_response(result));

         if (!ok) {
                 cb(&error, NULL, cbd->data);
                 return;
         }

         if (at_util_parse_attr(result, prefix, &attr) == FALSE) {
                 CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
                 return;
         }

         cb(&error, attr, cbd->data);
}

static void cdma_query_manufacturer(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	GAtChat *chat = ofono_devinfo_get_data(info);

	cbd->user = "+GMI:";

	if (g_at_chat_send(chat, "AT+GMI", NULL, attr_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void cdma_query_model(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	GAtChat *chat = ofono_devinfo_get_data(info);

	cbd->user = "+GMM:";

	if (g_at_chat_send(chat, "AT+GMM", NULL, attr_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void cdma_query_revision(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	GAtChat *chat = ofono_devinfo_get_data(info);

	cbd->user = "+GMR:";

	if (g_at_chat_send(chat, "AT+GMR", NULL, attr_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void cdma_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	GAtChat *chat = ofono_devinfo_get_data(info);

	cbd->user = "+GSN:";

	if (g_at_chat_send(chat, "AT+GSN", NULL, attr_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void capability_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_devinfo *info = user_data;

	ofono_devinfo_register(info);
}

static int cdma_devinfo_probe(struct ofono_devinfo *info,
				unsigned int vendor, void *data)
{
	GAtChat *chat = data;

	ofono_devinfo_set_data(info, g_at_chat_clone(chat));

	g_at_chat_send(chat, "AT+GCAP", gcap_prefix,
				capability_cb, info, NULL);

	return 0;
}

static void cdma_devinfo_remove(struct ofono_devinfo *info)
{
	GAtChat *chat = ofono_devinfo_get_data(info);

	g_at_chat_unref(chat);
	ofono_devinfo_set_data(info, NULL);
}

static struct ofono_devinfo_driver driver = {
	.name			= "cdmamodem",
	.probe			= cdma_devinfo_probe,
	.remove			= cdma_devinfo_remove,
	.query_manufacturer	= cdma_query_manufacturer,
	.query_model		= cdma_query_model,
	.query_revision		= cdma_query_revision,
	.query_serial		= cdma_query_serial
};

void cdma_devinfo_init(void)
{
	ofono_devinfo_driver_register(&driver);
}

void cdma_devinfo_exit(void)
{
	ofono_devinfo_driver_unregister(&driver);
}
