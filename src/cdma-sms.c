/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010-2011  Nokia Corporation and/or its subsidiary(-ies).
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
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>
#include <sys/time.h>

#include "ofono.h"

#include "cdma-smsutil.h"

static GSList *g_drivers;

struct ofono_cdma_sms {
	const struct ofono_cdma_sms_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static GDBusMethodTable cdma_sms_manager_methods[] = {
	/* TODO */
	{ }
};

static GDBusSignalTable cdma_sms_manager_signals[] = {
	{ "IncomingMessage",	"sa{sv}"	},
	/* TODO */
	{ }
};

static void cdma_dispatch_text_message(struct ofono_cdma_sms *cdma_sms,
					const char *message,
					const char *oaddr)
{
	const char *path = __ofono_atom_get_path(cdma_sms->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *signal_name;

	/* TODO: Support ImmediateMessage */
	signal_name = "IncomingMessage";

	signal = dbus_message_new_signal(path,
					OFONO_CDMA_MESSAGE_MANAGER_INTERFACE,
					signal_name);
	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &message);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "Sender", DBUS_TYPE_STRING, &oaddr);

	/* TODO: Other properties not supported yet */

	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(conn, signal);

	/*TODO: Add the message to history*/
}

static void ofono_cdma_sms_process_wmt_deliver(struct ofono_cdma_sms *cdma_sms,
						const struct cdma_sms *incoming)
{
	char *message;
	const char *oaddr;
	const struct cdma_sms_ud *ud;

	ud = &incoming->p2p_msg.bd.wmt_deliver.ud;

	/*
	 * If incoming message does not contain USER DATA, still
	 * send indication to upper layer but with empty string.
	 */
	if (check_bitmap(incoming->p2p_msg.bd.subparam_bitmap,
				CDMA_SMS_SUBPARAM_ID_USER_DATA) == FALSE)
		message = g_new0(char, 1);
	else
		message = cdma_sms_decode_text(ud);

	if (message == NULL)
		return;

	oaddr = cdma_sms_address_to_string(&incoming->p2p_msg.oaddr);
	if (oaddr == NULL) {
		g_free(message);
		return;
	}

	cdma_dispatch_text_message(cdma_sms, message, oaddr);

	g_free(message);
}

static void ofono_cdma_sms_process_wmt(struct ofono_cdma_sms *cdma_sms,
					struct cdma_sms *incoming)
{
	/* TODO: Add duplicate detection support */

	switch (incoming->p2p_msg.bd.id.msg_type) {
	case CDMA_SMS_MSG_TYPE_RESERVED:
		break;
	case CDMA_SMS_MSG_TYPE_DELIVER:
		ofono_cdma_sms_process_wmt_deliver(cdma_sms, incoming);
		break;
	case CDMA_SMS_MSG_TYPE_SUBMIT:
	case CDMA_SMS_MSG_TYPE_CANCEL:
	case CDMA_SMS_MSG_TYPE_DELIVER_ACK:
	case CDMA_SMS_MSG_TYPE_USER_ACK:
	case CDMA_SMS_MSG_TYPE_READ_ACK:
	case CDMA_SMS_MSG_TYPE_DELIVER_REPORT:
	case CDMA_SMS_MSG_TYPE_SUBMIT_REPORT:
		/* TODO */
		break;
	}
}

static void ofono_cdma_sms_process_p2p(struct ofono_cdma_sms *cdma_sms,
					struct cdma_sms *incoming)
{
	switch (incoming->p2p_msg.teleservice_id) {
	case CDMA_SMS_TELESERVICE_ID_CMT91:
	case CDMA_SMS_TELESERVICE_ID_WPT:
		break; /* TODO: Not supported yet */
	case CDMA_SMS_TELESERVICE_ID_WMT:
		ofono_cdma_sms_process_wmt(cdma_sms, incoming);
		break;
	case CDMA_SMS_TELESERVICE_ID_VMN:
	case CDMA_SMS_TELESERVICE_ID_WAP:
	case CDMA_SMS_TELESERVICE_ID_WEMT:
	case CDMA_SMS_TELESERVICE_ID_SCPT:
	case CDMA_SMS_TELESERVICE_ID_CATPT:
		break; /* TODO: Not supported yet */
	}
}

void ofono_cdma_sms_deliver_notify(struct ofono_cdma_sms *cdma_sms,
					unsigned char *pdu, int tpdu_len)
{
	static struct cdma_sms s;

	DBG("tpdu len %d", tpdu_len);

	memset(&s, 0, sizeof(struct cdma_sms));

	if (cdma_sms_decode(pdu, tpdu_len, &s) == FALSE)
		return;

	switch (s.type) {
	case CDMA_SMS_TP_MSG_TYPE_P2P:
		ofono_cdma_sms_process_p2p(cdma_sms, &s);
		break;
	case CDMA_SMS_TP_MSG_TYPE_BCAST:
	case CDMA_SMS_TP_MSG_TYPE_ACK:
		/*
		 * TODO: Support SMS Broadcast Message and SMS
		 * Acknowledge Message.
		 */
		break;
	}
}

int ofono_cdma_sms_driver_register(const struct ofono_cdma_sms_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_cdma_sms_driver_unregister(const struct ofono_cdma_sms_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void cdma_sms_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	g_dbus_unregister_interface(conn, path,
					OFONO_CDMA_MESSAGE_MANAGER_INTERFACE);

	ofono_modem_remove_interface(modem,
					OFONO_CDMA_MESSAGE_MANAGER_INTERFACE);
}

static void cdma_sms_remove(struct ofono_atom *atom)
{
	struct ofono_cdma_sms *cdma_sms = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cdma_sms == NULL)
		return;

	if (cdma_sms->driver && cdma_sms->driver->remove)
		cdma_sms->driver->remove(cdma_sms);

	g_free(cdma_sms);
}

/*
 * Create a CDMA SMS driver
 *
 * This creates a CDMA SMS driver that is hung off a @modem
 * object. However, for the driver to be used by the system, it has to
 * be registered with the oFono core using ofono_sms_register().
 *
 * This is done once the modem driver determines that SMS is properly
 * supported by the hardware.
 */
struct ofono_cdma_sms *ofono_cdma_sms_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver,
						void *data)
{
	struct ofono_cdma_sms *cdma_sms;
	GSList *l;

	if (driver == NULL)
		return NULL;

	cdma_sms = g_try_new0(struct ofono_cdma_sms, 1);
	if (cdma_sms == NULL)
		return NULL;

	cdma_sms->atom = __ofono_modem_add_atom(modem,
						OFONO_ATOM_TYPE_CDMA_SMS,
						cdma_sms_remove, cdma_sms);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_cdma_sms_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cdma_sms, vendor, data) < 0)
			continue;

		cdma_sms->driver = drv;
		break;
	}

	return cdma_sms;
}

/*
 * Indicate oFono that a CDMA SMS driver is ready for operation
 *
 * This is called after ofono_cdma_sms_create() was done and the modem
 * driver determined that a modem supports SMS correctly. Once this
 * call succeeds, the D-BUS interface for SMS goes live.
 */
void ofono_cdma_sms_register(struct ofono_cdma_sms *cdma_sms)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cdma_sms->atom);
	const char *path = __ofono_atom_get_path(cdma_sms->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CDMA_MESSAGE_MANAGER_INTERFACE,
					cdma_sms_manager_methods,
					cdma_sms_manager_signals,
					NULL, cdma_sms, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CDMA_MESSAGE_MANAGER_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_CDMA_MESSAGE_MANAGER_INTERFACE);

	__ofono_atom_register(cdma_sms->atom, cdma_sms_unregister);
}

void ofono_cdma_sms_remove(struct ofono_cdma_sms *cdma_sms)
{
	__ofono_atom_free(cdma_sms->atom);
}

void ofono_cdma_sms_set_data(struct ofono_cdma_sms *cdma_sms, void *data)
{
	cdma_sms->driver_data = data;
}

void *ofono_cdma_sms_get_data(struct ofono_cdma_sms *cdma_sms)
{
	return cdma_sms->driver_data;
}
