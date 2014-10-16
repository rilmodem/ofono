/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2014  Intel Corporation. All rights reserved.
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
#include <unistd.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/location-reporting.h>

#include "gatchat.h"
#include "gatresult.h"
#include "gattty.h"

#include "telitmodem.h"

static const char *none_prefix[] = { NULL };
static const char *portcfg_prefix[] = { "#PORTCFG:", NULL };
static const char *gpsctl_prefix[] = { "$GPSP:", NULL };

struct gps_data {
	GAtChat *chat;
};

static void telit_gps_disable_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_location_reporting *lr = cbd->user;
	ofono_location_reporting_disable_cb_t cb = cbd->cb;

	DBG("lr=%p, ok=%d", lr, ok);

	if (!ok) {
		struct ofono_error error;

		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, cbd->data);

		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void telit_location_reporting_disable(struct ofono_location_reporting *lr,
				ofono_location_reporting_disable_cb_t cb,
				void *data)
{
	struct gps_data *gd = ofono_location_reporting_get_data(lr);
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("lr=%p", lr);

	cbd->user = lr;

	if (g_at_chat_send(gd->chat, "AT$GPSP=0", none_prefix,
				telit_gps_disable_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);

	g_free(cbd);
}

static int enable_data_stream(struct ofono_location_reporting *lr)
{
	struct ofono_modem *modem;
	const char *gps_dev;
	GHashTable *options;
	GIOChannel *channel;
	int fd;

	modem = ofono_location_reporting_get_modem(lr);
	gps_dev = ofono_modem_get_string(modem, "GPS");

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		return -1;

	g_hash_table_insert(options, "Baud", "115200");

	channel = g_at_tty_open(gps_dev, options);

	g_hash_table_destroy(options);

	if (channel == NULL)
		return -1;

	fd = g_io_channel_unix_get_fd(channel);

	g_io_channel_set_close_on_unref(channel, FALSE);
	g_io_channel_unref(channel);

	return fd;
}

static void telit_gps_ctl_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_location_reporting_enable_cb_t cb = cbd->cb;
	struct ofono_location_reporting *lr = cbd->user;
	struct ofono_error error;
	int fd;

	DBG("lr=%p ok=%d", lr, ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);

		return;
	}

	fd = enable_data_stream(lr);

	if (fd < 0) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

		return;
	}

	cb(&error, fd, cbd->data);
	close(fd);
}

static void telit_gps_enable_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_location_reporting_enable_cb_t cb = cbd->cb;
	struct ofono_location_reporting *lr = cbd->user;
	struct gps_data *gd = ofono_location_reporting_get_data(lr);
	struct ofono_error error;

	DBG("lr=%p ok=%d", lr, ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);

		g_free(cbd);
		return;
	}

	if( g_at_chat_send(gd->chat, "AT$GPSNMUN=1,0,0,0,0,0,0",
				none_prefix, telit_gps_ctl_cb, cbd, g_free ) > 0 )
		return;

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	g_free(cbd);
}

static void telit_portcfg_check_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_location_reporting_enable_cb_t cb = cbd->cb;
	struct ofono_location_reporting *lr = cbd->user;
	struct gps_data *gd = ofono_location_reporting_get_data(lr);
	struct ofono_error error;
	int requested_portcfg, current_portcfg;
	GAtResultIter iter;

	DBG("lr=%p ok=%d", lr, ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);

		g_free(cbd);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "#PORTCFG:"))
		goto fail;

	if (!g_at_result_iter_next_number(&iter, &requested_portcfg))
		goto fail;

	if (!g_at_result_iter_next_number(&iter, &current_portcfg))
		goto fail;

	if ( current_portcfg != 8 ) {
		ofono_warn("Unable to start GPS, modem configuration invalid");
		ofono_warn("Refer to doc/telit-modem.txt section HE910/GPS");
		goto fail;
	}

	if (g_at_chat_send(gd->chat, "AT$GPSP=1", none_prefix,
				telit_gps_enable_cb, cbd, NULL) > 0)
		return;

fail:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	g_free(cbd);
}

static void telit_location_reporting_enable(struct ofono_location_reporting *lr,
					ofono_location_reporting_enable_cb_t cb,
					void *data)
{
	struct gps_data *gd = ofono_location_reporting_get_data(lr);
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("lr=%p", lr);

	cbd->user = lr;

	if (g_at_chat_send(gd->chat, "AT#PORTCFG?", portcfg_prefix,
				telit_portcfg_check_cb, cbd, NULL) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	g_free(cbd);
}

static void telit_location_reporting_support_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct ofono_location_reporting *lr = user_data;

	if (!ok) {
		ofono_location_reporting_remove(lr);

		return;
	}

	ofono_location_reporting_register(lr);
}

static int telit_location_reporting_probe(struct ofono_location_reporting *lr,
						unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gps_data *gd;

	gd = g_try_new0(struct gps_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	gd->chat = g_at_chat_clone(chat);

	ofono_location_reporting_set_data(lr, gd);

	g_at_chat_send(gd->chat, "AT$GPSP=?", gpsctl_prefix,
					telit_location_reporting_support_cb,
					lr, NULL);

	return 0;
}

static void telit_location_reporting_remove(struct ofono_location_reporting *lr)
{
	struct gps_data *gd = ofono_location_reporting_get_data(lr);

	ofono_location_reporting_set_data(lr, NULL);

	g_at_chat_unref(gd->chat);
	g_free(gd);
}

static struct ofono_location_reporting_driver driver = {
	.name			= "telitmodem",
	.type			= OFONO_LOCATION_REPORTING_TYPE_NMEA,
	.probe			= telit_location_reporting_probe,
	.remove			= telit_location_reporting_remove,
	.enable			= telit_location_reporting_enable,
	.disable		= telit_location_reporting_disable,
};

void telit_location_reporting_init()
{
	ofono_location_reporting_driver_register(&driver);
}

void telit_location_reporting_exit()
{
	ofono_location_reporting_driver_unregister(&driver);
}
