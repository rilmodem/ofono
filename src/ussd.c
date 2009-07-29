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
#include <stdio.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "ussd.h"

#define SUPPLEMENTARY_SERVICES_INTERFACE "org.ofono.SupplementaryServices"

#define USSD_FLAG_PENDING 0x1

enum ussd_state {
	USSD_STATE_IDLE = 0,
	USSD_STATE_ACTIVE = 1,
	USSD_STATE_USER_ACTION = 2
};

struct ussd_data {
	struct ofono_ussd_ops *ops;
	int state;
	DBusMessage *pending;
	int flags;
};

static struct ussd_data *ussd_create()
{
	struct ussd_data *r;

	r = g_try_new0(struct ussd_data, 1);

	return r;
}

static void ussd_destroy(gpointer data)
{
	struct ofono_modem *modem = data;
	struct ussd_data *ussd = modem->ussd;

	g_free(ussd);
}

struct ss_control_entry {
	char *service;
	ss_control_cb_t cb;
};

static struct ss_control_entry *ss_control_entry_create(const char *service,
							ss_control_cb_t cb)
{
	struct ss_control_entry *r;

	r = g_try_new0(struct ss_control_entry, 1);

	if (!r)
		return r;

	r->service = g_strdup(service);
	r->cb = cb;

	return r;
}

static void ss_control_entry_destroy(struct ss_control_entry *ca)
{
	g_free(ca->service);
	g_free(ca);
}

static gint ss_control_entry_compare(gconstpointer a, gconstpointer b)
{
	const struct ss_control_entry *ca = a;
	const struct ss_control_entry *cb = b;
	int ret;

	ret = strcmp(ca->service, cb->service);

	if (ret)
		return ret;

	if (ca->cb < cb->cb)
		return -1;

	if (ca->cb > cb->cb)
		return 1;

	return 0;
}

static gint ss_control_entry_find_by_service(gconstpointer a, gconstpointer b)
{
	const struct ss_control_entry *ca = a;

	return strcmp(ca->service, b);
}

gboolean ss_control_register(struct ofono_modem *modem, const char *str,
				ss_control_cb_t cb)
{
	struct ss_control_entry *entry;

	if (!modem)
		return FALSE;

	entry = ss_control_entry_create(str, cb);

	if (!entry)
		return FALSE;

	modem->ss_control_list = g_slist_append(modem->ss_control_list, entry);

	return TRUE;
}

void ss_control_unregister(struct ofono_modem *modem, const char *str,
				ss_control_cb_t cb)
{
	const struct ss_control_entry entry = { (char *)str, cb };
	GSList *l;

	if (!modem)
		return;

	l = g_slist_find_custom(modem->ss_control_list, &entry,
				ss_control_entry_compare);

	if (!l)
		return;

	ss_control_entry_destroy(l->data);
	modem->ss_control_list = g_slist_remove(modem->ss_control_list,
						l->data);
}

struct ss_passwd_entry {
	char *service;
	ss_passwd_cb_t cb;
};

static struct ss_passwd_entry *ss_passwd_entry_create(const char *service,
							ss_passwd_cb_t cb)
{
	struct ss_passwd_entry *r;

	r = g_try_new0(struct ss_passwd_entry, 1);

	if (!r)
		return r;

	r->service = g_strdup(service);
	r->cb = cb;

	return r;
}

static void ss_passwd_entry_destroy(struct ss_passwd_entry *ca)
{
	g_free(ca->service);
	g_free(ca);
}

static gint ss_passwd_entry_compare(gconstpointer a, gconstpointer b)
{
	const struct ss_passwd_entry *ca = a;
	const struct ss_passwd_entry *cb = b;
	int ret;

	ret = strcmp(ca->service, cb->service);

	if (ret)
		return ret;

	if (ca->cb < cb->cb)
		return -1;

	if (ca->cb > cb->cb)
		return 1;

	return 0;
}

static gint ss_passwd_entry_find_by_service(gconstpointer a, gconstpointer b)
{
	const struct ss_passwd_entry *ca = a;

	return strcmp(ca->service, b);
}

gboolean ss_passwd_register(struct ofono_modem *modem, const char *str,
				ss_passwd_cb_t cb)
{
	struct ss_passwd_entry *entry;

	if (!modem)
		return FALSE;

	entry = ss_passwd_entry_create(str, cb);

	if (!entry)
		return FALSE;

	modem->ss_passwd_list = g_slist_append(modem->ss_passwd_list, entry);

	return TRUE;
}

void ss_passwd_unregister(struct ofono_modem *modem, const char *str,
				ss_passwd_cb_t cb)
{
	const struct ss_passwd_entry entry = { (char *)str, cb };
	GSList *l;

	if (!modem)
		return;

	l = g_slist_find_custom(modem->ss_passwd_list, &entry,
				ss_passwd_entry_compare);

	if (!l)
		return;

	ss_passwd_entry_destroy(l->data);
	modem->ss_passwd_list = g_slist_remove(modem->ss_passwd_list,
						l->data);
}

static gboolean recognized_passwd_change_string(struct ofono_modem *modem,
						int type, char *sc,
						char *sia, char *sib,
						char *sic, char *sid,
						char *dn, DBusMessage *msg)
{
	GSList *l = modem->ss_passwd_list;

	switch (type) {
	case SS_CONTROL_TYPE_ACTIVATION:
	case SS_CONTROL_TYPE_REGISTRATION:
		break;

	default:
		return FALSE;
	}

	if (strcmp(sc, "03") || strlen(dn))
		return FALSE;

	/* If SIC & SID don't match, then we just bail out here */
	if (strcmp(sic, sid)) {
		DBusConnection *conn = ofono_dbus_get_connection();
		DBusMessage *reply = __ofono_error_invalid_format(msg);
		g_dbus_send_message(conn, reply);
		return TRUE;
	}

	while ((l = g_slist_find_custom(l, sia,
			ss_passwd_entry_find_by_service)) != NULL) {
		struct ss_passwd_entry *entry = l->data;

		if (entry->cb(modem, sia, sib, sic, msg))
			return TRUE;

		l = l->next;
	}

	return FALSE;
}

static gboolean recognized_control_string(struct ofono_modem *modem,
						const char *ss_str,
						DBusMessage *msg)
{
	char *str = g_strdup(ss_str);
	char *sc, *sia, *sib, *sic, *sid, *dn;
	int type;
	gboolean ret = FALSE;

	ofono_debug("parsing control string");

	if (parse_ss_control_string(str, &type, &sc,
				&sia, &sib, &sic, &sid, &dn)) {
		GSList *l = modem->ss_control_list;

		ofono_debug("Got parse result: %d, %s, %s, %s, %s, %s, %s",
				type, sc, sia, sib, sic, sid, dn);

		/* A password change string needs to be treated separately
		 * because it uses a fourth SI and is thus not a valid
		 * control string.  */
		if (recognized_passwd_change_string(modem, type, sc,
					sia, sib, sic, sid, dn, msg)) {
			ret = TRUE;
			goto out;
		}

		if (*sid != '\0')
			goto out;

		while ((l = g_slist_find_custom(l, sc,
				ss_control_entry_find_by_service)) != NULL) {
			struct ss_control_entry *entry = l->data;

			if (entry->cb(modem, type, sc, sia, sib, sic, dn, msg)) {
				ret = TRUE;
				goto out;
			}

			l = l->next;
		}

	}

	/* TODO: Handle all strings that control voice calls */

	/* TODO: Handle Multiple subscriber profile DN*59#SEND and *59#SEND
	 */

	/* Note: SIM PIN/PIN2 change and unblock and IMEI presentation
	 * procedures are not handled by the daemon since they are not followed
	 * by SEND and are not valid USSD requests.
	 */

out:
	g_free(str);

	return ret;
}

void ofono_ussd_notify(struct ofono_modem *modem, int status, const char *str)
{
	struct ussd_data *ussd = modem->ussd;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *ussdstr = "USSD";
	const char sig[] = { DBUS_TYPE_STRING, 0 };
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter variant;

	if (status == USSD_STATUS_NOT_SUPPORTED) {
		ussd->state = USSD_STATE_IDLE;
		reply = __ofono_error_not_supported(ussd->pending);
		goto out;
	}

	if (status == USSD_STATUS_TIMED_OUT) {
		ussd->state = USSD_STATE_IDLE;
		reply = __ofono_error_timed_out(ussd->pending);
		goto out;
	}

	/* TODO: Rework this in the Agent framework */
	if (ussd->state == USSD_STATE_ACTIVE) {
		if (status == USSD_STATUS_ACTION_REQUIRED) {
			ofono_error("Unable to handle action required ussd");
			return;
		}

		reply = dbus_message_new_method_return(ussd->pending);

		dbus_message_iter_init_append(reply, &iter);

		dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
						&ussdstr);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, sig,
							&variant);

		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						&str);

		dbus_message_iter_close_container(&iter, &variant);

		ussd->state = USSD_STATE_IDLE;
	} else {
		ofono_error("Received an unsolicited USSD, ignoring for now...");
		ofono_debug("USSD is: status: %d, %s", status, str);

		return;
	}

out:
	g_dbus_send_message(conn, reply);

	dbus_message_unref(ussd->pending);
	ussd->pending = NULL;
}

static void ussd_callback(const struct ofono_error *error, void *data)
{
	struct ussd_data *ussd = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("ussd request failed with error: %s",
				telephony_error_to_str(error));

	ussd->flags &= ~USSD_FLAG_PENDING;

	if (!ussd->pending)
		return;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		ussd->state = USSD_STATE_ACTIVE;
		return;
	}

	reply = __ofono_error_failed(ussd->pending);

	g_dbus_send_message(conn, reply);

	dbus_message_unref(ussd->pending);
	ussd->pending = NULL;
}

static DBusMessage *ussd_initiate(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct ussd_data *ussd = modem->ussd;
	const char *str;

	if (ussd->flags & USSD_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (ussd->state == USSD_STATE_ACTIVE)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &str,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (strlen(str) == 0)
		return __ofono_error_invalid_format(msg);

	ofono_debug("checking if this is a recognized control string");
	if (recognized_control_string(modem, str, msg))
		return NULL;

	ofono_debug("No.., checking if this is a USSD string");
	if (!valid_ussd_string(str))
		return __ofono_error_invalid_format(msg);

	ofono_debug("OK, running USSD request");

	if (!ussd->ops->request)
		return __ofono_error_not_implemented(msg);

	ussd->flags |= USSD_FLAG_PENDING;
	ussd->pending = dbus_message_ref(msg);

	ussd->ops->request(modem, str, ussd_callback, ussd);

	return NULL;
}

static void ussd_cancel_callback(const struct ofono_error *error, void *data)
{
	struct ussd_data *ussd = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("ussd cancel failed with error: %s",
				telephony_error_to_str(error));

	ussd->flags &= ~USSD_FLAG_PENDING;

	if (!ussd->pending)
		return;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		ussd->state = USSD_STATE_IDLE;

		reply = dbus_message_new_method_return(ussd->pending);
	} else
		reply = __ofono_error_failed(ussd->pending);

	__ofono_dbus_pending_reply(&ussd->pending, reply);
}

static DBusMessage *ussd_cancel(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct ussd_data *ussd = modem->ussd;

	if (ussd->flags & USSD_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (ussd->state == USSD_STATE_IDLE)
		return __ofono_error_not_active(msg);

	if (!ussd->ops->cancel)
		return __ofono_error_not_implemented(msg);

	ussd->flags |= USSD_FLAG_PENDING;
	ussd->pending = dbus_message_ref(msg);

	ussd->ops->cancel(modem, ussd_cancel_callback, ussd);

	return NULL;
}

static GDBusMethodTable ussd_methods[] = {
	{ "Initiate",	"s",	"sv",	ussd_initiate,
					G_DBUS_METHOD_FLAG_ASYNC },
	{ "Cancel",	"",	"",	ussd_cancel,
					G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable ussd_signals[] = {
	{ }
};

int ofono_ussd_register(struct ofono_modem *modem, struct ofono_ussd_ops *ops)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->ussd = ussd_create();

	if (modem->ussd == NULL)
		return -1;

	modem->ussd->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					SUPPLEMENTARY_SERVICES_INTERFACE,
					ussd_methods, ussd_signals, NULL,
					modem, ussd_destroy)) {
		ofono_error("Could not create %s interface",
				SUPPLEMENTARY_SERVICES_INTERFACE);

		ussd_destroy(modem->ussd);

		return -1;
	}

	ofono_modem_add_interface(modem, SUPPLEMENTARY_SERVICES_INTERFACE);

	return 0;
}

void ofono_ussd_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (modem->ussd == NULL)
		return;

	ofono_modem_remove_interface(modem, SUPPLEMENTARY_SERVICES_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path,
					SUPPLEMENTARY_SERVICES_INTERFACE);
}
