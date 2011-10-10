/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  ST-Ericsson AB.
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
#include <ofono/gnss.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"
#include "vendor.h"

struct gnss_data {
	GAtChat *chat;
	unsigned int vendor;
};

static const char *none_prefix[] = { NULL };
static const char *cpos_prefix[] = { "+CPOS:", NULL };
static const char *cposr_prefix[] = { "+CPOSR:", NULL };

static void gnss_pr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gnss_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_gnss_position_reporting(struct ofono_gnss *gnss,
					ofono_bool_t enable,
					ofono_gnss_cb_t cb,
					void *data)
{
	struct gnss_data *ad = ofono_gnss_get_data(gnss);
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("");

	if (enable) {
		g_at_chat_send(ad->chat, "AT+CPOSR=1",
				cposr_prefix, gnss_pr_cb, cbd, g_free);

		if (ad->vendor == OFONO_VENDOR_STE)
			g_at_chat_send(ad->chat, "AT*EPOSADRR=1",
					NULL, NULL, NULL, NULL);
	} else {
		g_at_chat_send(ad->chat, "AT+CPOSR=0",
				cposr_prefix, gnss_pr_cb, cbd, g_free);

		if (ad->vendor == OFONO_VENDOR_STE)
			g_at_chat_send(ad->chat, "AT*EPOSADRR=0",
					NULL, NULL, NULL, NULL);
	}
}

static void gnss_se_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gnss_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_gnss_send_element(struct ofono_gnss *gnss,
				const char *xml,
				ofono_gnss_cb_t cb, void *data)
{
	struct gnss_data *ad = ofono_gnss_get_data(gnss);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, strlen(xml) + 10);
	int len;

	DBG("");

	if (buf == NULL)
		goto error;

	len = sprintf(buf, "AT+CPOS\r");
	len += sprintf(buf + len, "%s", xml);

	if (g_at_chat_send_and_expect_short_prompt(ad->chat, buf, cpos_prefix,
							gnss_se_cb, cbd,
							g_free) > 0) {
		g_free(buf);
		return;
	}

error:
	g_free(buf);
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean gnss_parse_report(GAtResult *result, const char *prefix,
					const char **xml)
{
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, prefix))
		return FALSE;

	if (!g_at_result_iter_next_unquoted_string(&iter, xml))
		return FALSE;

	return TRUE;
}

static void gnss_report(GAtResult *result, gpointer user_data)
{
	const char *xml;

	DBG("");

	xml = NULL;

	if (!gnss_parse_report(result, "+CPOSR:", &xml)) {
		ofono_error("Unable to parse CPOSR notification");
		return;
	}

	if (xml == NULL) {
		ofono_error("Unable to parse CPOSR notification");
		return;
	}

	DBG("%s", xml);
}

static void at_gnss_reset_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gnss *gnss = user_data;

	DBG("");

	ofono_gnss_notify_posr_reset(gnss);
}

static void at_gnss_not_supported(struct ofono_gnss *gnss)
{
	ofono_error("gnss not supported by this modem.");

	ofono_gnss_remove(gnss);
}

static void at_gnss_cposr_support_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_gnss *gnss = user_data;
	struct gnss_data *ad = ofono_gnss_get_data(gnss);

	DBG("");

	if (!ok) {
		at_gnss_not_supported(gnss);
		return;
	}

	g_at_chat_register(ad->chat, "+CPOSR:", gnss_report,
				FALSE, gnss, NULL);

	if (ad->vendor == OFONO_VENDOR_STE)
		g_at_chat_register(ad->chat, "*EPOSADRR:", at_gnss_reset_notify,
					FALSE, gnss, NULL);

	ofono_gnss_register(gnss);
}

static void at_gnss_cpos_support_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_gnss *gnss = user_data;
	struct gnss_data *ad = ofono_gnss_get_data(gnss);

	DBG("");

	if (!ok) {
		at_gnss_not_supported(gnss);
		return;
	}

	g_at_chat_send(ad->chat, "AT+CPOSR=?",
			none_prefix, at_gnss_cposr_support_cb, gnss, NULL);
}

static int at_gnss_probe(struct ofono_gnss *gnss, unsigned int vendor,
				void *user)
{
	GAtChat *chat = user;
	struct gnss_data *gd;

	DBG("");

	gd = g_try_new0(struct gnss_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	gd->chat = g_at_chat_clone(chat);
	gd->vendor = vendor;

	ofono_gnss_set_data(gnss, gd);

	g_at_chat_send(gd->chat, "AT+CPOS=?",
			none_prefix, at_gnss_cpos_support_cb, gnss, NULL);

	return 0;
}

static void at_gnss_remove(struct ofono_gnss *gnss)
{
	struct gnss_data *gd = ofono_gnss_get_data(gnss);

	DBG("");

	ofono_gnss_set_data(gnss, NULL);

	g_at_chat_unref(gd->chat);
	g_free(gd);
}

static struct ofono_gnss_driver driver = {
	.name			= "atmodem",
	.probe			= at_gnss_probe,
	.remove			= at_gnss_remove,
	.send_element		= at_gnss_send_element,
	.set_position_reporting	= at_gnss_position_reporting,
};

void at_gnss_init(void)
{
	ofono_gnss_driver_register(&driver);
}

void at_gnss_exit(void)
{
	ofono_gnss_driver_unregister(&driver);
}
