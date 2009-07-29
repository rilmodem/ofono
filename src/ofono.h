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

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE

int __ofono_manager_init();
void __ofono_manager_cleanup();

GSList *ofono_manager_get_modems();

#include <ofono/log.h>

int __ofono_log_init(gboolean detach, gboolean debug);
void __ofono_log_cleanup(void);

void __ofono_toggle_debug(void);

#include <ofono/dbus.h>

int __ofono_dbus_init(DBusConnection *conn);
void __ofono_dbus_cleanup(void);

DBusMessage *__ofono_error_invalid_args(DBusMessage *msg);
DBusMessage *__ofono_error_invalid_format(DBusMessage *msg);
DBusMessage *__ofono_error_not_implemented(DBusMessage *msg);
DBusMessage *__ofono_error_failed(DBusMessage *msg);
DBusMessage *__ofono_error_busy(DBusMessage *msg);
DBusMessage *__ofono_error_not_found(DBusMessage *msg);
DBusMessage *__ofono_error_not_active(DBusMessage *msg);
DBusMessage *__ofono_error_not_supported(DBusMessage *msg);
DBusMessage *__ofono_error_timed_out(DBusMessage *msg);

void __ofono_dbus_pending_reply(DBusMessage **msg, DBusMessage *reply);

#include <ofono/plugin.h>

int __ofono_plugin_init(const char *pattern, const char *exclude);
void __ofono_plugin_cleanup(void);

#include <ofono/history.h>

void __ofono_history_probe_drivers(struct ofono_modem *modem);
void __ofono_history_remove_drivers(struct ofono_modem *modem);

void __ofono_history_call_ended(struct ofono_modem *modem,
				const struct ofono_call *call,
				time_t start, time_t end);

void __ofono_history_call_missed(struct ofono_modem *modem,
				const struct ofono_call *call, time_t when);
