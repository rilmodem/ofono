/*
 *
 *  oFono - Open Source Telephony
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <glib.h>
#include <gdbus.h>
#include <ofono.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/dbus.h>

#include "drivers/mtkmodem/mtk_constants.h"
#include "drivers/mtkmodem/mtkrequest.h"
#include "drivers/mtkmodem/mtkutil.h"
#include "mtksettings.h"

#define MTK_SETTINGS_INTERFACE "org.ofono.MtkSettings"

struct mtk_settings_data {
	struct ofono_modem *modem;
	GRil *ril;
	ofono_bool_t has_3g;
	ofono_bool_t has_3g_pending;
	DBusMessage *pending;
};

static void set_3g(struct mtk_settings_data *msd, ofono_bool_t has_3g)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(msd->modem);
	dbus_bool_t value = has_3g;

	if (msd->has_3g == has_3g)
		return;

	ofono_dbus_signal_property_changed(conn, path, MTK_SETTINGS_INTERFACE,
						"Has3G", DBUS_TYPE_BOOLEAN,
						&value);
	msd->has_3g = has_3g;
}

static void set_3g_cb(struct ril_msg *message, gpointer user_data)
{
	struct mtk_settings_data *msd = user_data;
	DBusMessage *reply;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: Error setting 3G", __func__);

		msd->has_3g_pending = msd->has_3g;

		reply = __ofono_error_failed(msd->pending);
		__ofono_dbus_pending_reply(&msd->pending, reply);

		return;
	}

	g_ril_print_response_no_args(msd->ril, message);

	reply = dbus_message_new_method_return(msd->pending);
	__ofono_dbus_pending_reply(&msd->pending, reply);

	set_3g(msd, msd->has_3g_pending);

	mtk_reset_all_modems();
}

static DBusMessage *set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct mtk_settings_data *msd = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (msd->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_strcmp0(property, "Has3G") == 0) {
		dbus_bool_t value;
		struct parcel rilp;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (msd->has_3g_pending == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		/*
		 * We can only set to true, as setting to false could be
		 * confusing in a multi-sim environment (>2 SIM)
		 */
		if (value == FALSE)
			return __ofono_error_invalid_args(msg);

		g_mtk_request_set_3g_capability(msd->ril, &rilp);

		if (g_ril_send(msd->ril, MTK_RIL_REQUEST_SET_3G_CAPABILITY,
				&rilp, set_3g_cb, msd, NULL) == 0) {
			ofono_error("%s: unable to set 3G for slot", __func__);
			return __ofono_error_failed(msg);
		}

		msd->pending = dbus_message_ref(msg);
		msd->has_3g_pending = value;

		return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct mtk_settings_data *msd = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "Has3G",
				DBUS_TYPE_BOOLEAN, &msd->has_3g);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static const GDBusMethodTable mtk_settings_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, set_property) },
	{ }
};

static const GDBusSignalTable mtk_settings_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};


static void register_interface(struct mtk_settings_data *msd)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (!g_dbus_register_interface(conn, ofono_modem_get_path(msd->modem),
					MTK_SETTINGS_INTERFACE,
					mtk_settings_methods,
					mtk_settings_signals,
					NULL, msd, NULL)) {
		ofono_error("Could not create %s interface",
				MTK_SETTINGS_INTERFACE);
		return;
	}

	ofono_modem_add_interface(msd->modem, MTK_SETTINGS_INTERFACE);
}

static void unregister_interface(struct mtk_settings_data *msd)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	ofono_modem_remove_interface(msd->modem, MTK_SETTINGS_INTERFACE);

	g_dbus_unregister_interface(conn,
					ofono_modem_get_path(msd->modem),
					MTK_SETTINGS_INTERFACE);
}

struct mtk_settings_data *mtk_settings_create(struct ofono_modem *modem,
						GRil *ril, ofono_bool_t has_3g)
{
	struct mtk_settings_data *msd = g_try_malloc0(sizeof(*msd));

	DBG("");

	if (msd == NULL) {
		ofono_error("%s: Cannot allocate mtk_settings_data", __func__);
		return NULL;
	}

	msd->modem = modem;
	msd->ril = g_ril_clone(ril);

	msd->has_3g = has_3g;
	msd->has_3g_pending = has_3g;

	register_interface(msd);

	return msd;
}

void mtk_settings_remove(struct mtk_settings_data *msd)
{
	DBG("");

	if (msd == NULL)
		return;

	unregister_interface(msd);

	g_ril_unref(msd->ril);
	g_free(msd);
}
