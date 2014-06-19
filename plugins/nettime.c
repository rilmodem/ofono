/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012-2013  Jolla Ltd.
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

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/nettime.h>
#include <ofono/types.h>

#include "ofono.h"

#include "common.h"

struct nt_data {
	gboolean time_available;
	gboolean time_pending;

	time_t nw_time_utc;
	time_t received;

	int dst;
	int time_zone;

	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	const char *path;
};

static void init_time(struct ofono_nettime_context *context)
{
	struct nt_data *nt_data = g_new0(struct nt_data, 1);

	nt_data->time_available = FALSE;
	nt_data->time_pending = FALSE;
	nt_data->dst = 0;
	nt_data->time_zone = 0;

	context->data = nt_data;
}

static gboolean encode_time_format(const struct ofono_network_time *time,
					struct tm *tm)
{
	if (time->year < 0)
		return FALSE;

	memset(tm, 0, sizeof(struct tm));
	tm->tm_year = time->year - 1900;
	tm->tm_mon = time->mon - 1;
	tm->tm_mday = time->mday;
	tm->tm_hour = time->hour;
	tm->tm_min = time->min;
	tm->tm_sec = time->sec;
	tm->tm_gmtoff = time->utcoff;
	tm->tm_isdst = time->dst;

	return TRUE;
}

static time_t get_monotonic_time()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static int fill_time_notification(DBusMessage *msg, struct nt_data *ntd)
{
	DBusMessageIter iter, iter_array;
	dbus_int64_t utc_long, received;
	dbus_int32_t dst, timezone;
	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_open_container(&iter,	DBUS_TYPE_ARRAY,
						"{sv}",
						&iter_array);

	if (ntd->time_pending) {
		if (ntd->time_available) {
			utc_long = (dbus_int64_t) ntd->nw_time_utc;
			ofono_dbus_dict_append(&iter_array,
						"UTC",
						DBUS_TYPE_INT64,
						&utc_long);
			dst = (dbus_int32_t) ntd->dst;
			ofono_dbus_dict_append(&iter_array,
						"DST",
						DBUS_TYPE_UINT32,
						&dst);
			timezone = (dbus_int32_t) ntd->time_zone;
			ofono_dbus_dict_append(&iter_array,
						"Timezone",
						DBUS_TYPE_INT32,
						&timezone);
			received = (dbus_int64_t) ntd->received;
			ofono_dbus_dict_append(&iter_array,
						"Received",
						DBUS_TYPE_INT64,
						&received);
		}

		if (ntd->mcc[0] != '\0') {
			char *mcc = ntd->mcc;
			ofono_dbus_dict_append(&iter_array,
						"MobileCountryCode",
						DBUS_TYPE_STRING,
						&mcc);
		}

		if (ntd->mnc[0] != '\0') {
			char *mnc = ntd->mnc;
			ofono_dbus_dict_append(&iter_array,
						"MobileNetworkCode",
						DBUS_TYPE_STRING,
						&mnc);
		}
	} else {
		DBG("fill_time_notification: time not available");
	}

	dbus_message_iter_close_container(&iter, &iter_array);
	return 0;
}

static DBusMessage *get_network_time(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct ofono_nettime_context *context = data;
	struct nt_data *nt_data = context->data;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	fill_time_notification(reply, nt_data);
	return reply;
}

static const GDBusMethodTable nettime_methods[] = {
	{ GDBUS_METHOD("GetNetworkTime",
			NULL, GDBUS_ARGS({ "time", "a{sv}" }),
			get_network_time) },
	{ }
};

static const GDBusSignalTable nettime_signals[] = {
	{ GDBUS_SIGNAL("NetworkTimeChanged",
			GDBUS_ARGS({ "time", "a{sv}" })) },
	{ }
};

static int nettime_probe(struct ofono_nettime_context *context)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(context->modem);

	DBG("Network time probe for modem: %p (%s)", context->modem, path);

	init_time(context);

	if (!g_dbus_register_interface(conn, path,
					OFONO_NETWORK_TIME_INTERFACE,
					nettime_methods,
					nettime_signals,
					NULL,		/* properties */
					context,	/* user data */
					NULL)) {
		ofono_error("Network time: Could not register interface %s, "
				"path %s", OFONO_NETWORK_TIME_INTERFACE, path);
		return 1;
	} else {
		ofono_info("Network time: Registered inteface %s, path %s",
				OFONO_NETWORK_TIME_INTERFACE, path);
	}

	ofono_modem_add_interface(context->modem, OFONO_NETWORK_TIME_INTERFACE);

	return 0;
}

static void nettime_remove(struct ofono_nettime_context *context)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(context->modem);

	DBG("Network time remove for modem: %p (%s)", context->modem, path);

	if (!g_dbus_unregister_interface(conn, path,
					OFONO_NETWORK_TIME_INTERFACE))
		ofono_error("Network time: could not unregister interface %s, "
				"path %s", OFONO_NETWORK_TIME_INTERFACE, path);

	ofono_modem_remove_interface(context->modem,
					OFONO_NETWORK_TIME_INTERFACE);
	g_free(context->data);
}

static void send_signal(struct nt_data *ntd)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *signal =
		dbus_message_new_signal(ntd->path, OFONO_NETWORK_TIME_INTERFACE,
					"NetworkTimeChanged");

	fill_time_notification(signal, ntd);
	g_dbus_send_message(conn, signal);
}

static void nettime_info_received(struct ofono_nettime_context *context,
					struct ofono_network_time *info)
{
	struct ofono_netreg *netreg;
	struct tm t;
	struct nt_data *ntd = context->data;
	const char *mcc;
	const char *mnc;

	if (info == NULL)
		return;

	netreg = __ofono_atom_get_data(__ofono_modem_find_atom(
				context->modem, OFONO_ATOM_TYPE_NETREG));

	ntd->path = ofono_modem_get_path(context->modem);
	mcc = ofono_netreg_get_mcc(netreg);
	mnc = ofono_netreg_get_mnc(netreg);

	if (mcc == NULL)
		ntd->mcc[0] = '\0';
	else
		strcpy(ntd->mcc, mcc);

	if (mnc == NULL)
		ntd->mnc[0] = '\0';
	else
		strcpy(ntd->mnc, mnc);

	ntd->received = get_monotonic_time();
	ntd->time_pending = TRUE;
	ntd->dst = info->dst;
	ntd->time_zone = info->utcoff;

	ntd->time_available = encode_time_format(info, &t);
	if (ntd->time_available == TRUE)
		ntd->nw_time_utc = timegm(&t);

	send_signal(ntd);

	DBG("modem: %p (%s)",
		context->modem, ofono_modem_get_path(context->modem));
	DBG("time: %04d-%02d-%02d %02d:%02d:%02d%c%02d:%02d (DST=%d)",
		info->year, info->mon, info->mday, info->hour,
		info->min, info->sec, info->utcoff > 0 ? '+' : '-',
		abs(info->utcoff) / 3600, (abs(info->utcoff) % 3600) / 60,
		info->dst);
	DBG("UTC timestamp: %li, Received (monotonic time): %li",
		ntd->nw_time_utc, ntd->received);
	DBG("MCC: %s, MNC: %s", ntd->mcc, ntd->mnc);
}

static struct ofono_nettime_driver driver = {
	.name		= "Network Time",
	.probe		= nettime_probe,
	.remove		= nettime_remove,
	.info_received	= nettime_info_received,
};

static int nettime_init(void)
{
	return ofono_nettime_driver_register(&driver);
}

static void nettime_exit(void)
{
	ofono_nettime_driver_unregister(&driver);
}

OFONO_PLUGIN_DEFINE(nettime, "Network Time Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			nettime_init, nettime_exit)
