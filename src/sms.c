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

#include <string.h>
#include <stdio.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "util.h"
#include "sim.h"
#include "smsutil.h"

#define uninitialized_var(x) x = x

#define SMS_MANAGER_INTERFACE "org.ofono.SmsManager"

#define SMS_MANAGER_FLAG_CACHED 0x1

static gboolean tx_next(gpointer user_data);

struct sms_manager_data {
	struct ofono_sms_ops *ops;
	int flags;
	DBusMessage *pending;
	struct ofono_phone_number sca;
	struct sms_assembly *assembly;
	guint ref;
	GQueue *txq;
	time_t last_mms;
	gint tx_source;
};

struct pending_pdu {
	unsigned char pdu[176];
	int tpdu_len;
	int pdu_len;
};

static struct sms_manager_data *sms_manager_create()
{
	struct sms_manager_data *sms;

	sms = g_new0(struct sms_manager_data, 1);

	sms->sca.type = 129;
	sms->ref = 1;

	sms->assembly = sms_assembly_new();
	sms->txq = g_queue_new();

	return sms;
}

static void sms_manager_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct sms_manager_data *data = modem->sms_manager;

	if (data->tx_source) {
		g_source_remove(data->tx_source);
		data->tx_source = 0;
	}

	if (data->assembly) {
		sms_assembly_free(data->assembly);
		data->assembly = NULL;
	}

	if (data->txq) {
		g_queue_foreach(data->txq, (GFunc)g_free, NULL);
		g_queue_free(data->txq);
		data->txq = NULL;
	}

	g_free(data);
}

static void set_sca(struct ofono_modem *modem,
			const struct ofono_phone_number *sca)
{
	struct sms_manager_data *sms = modem->sms_manager;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *value;

	if (sms->sca.type == sca->type &&
			!strcmp(sms->sca.number, sca->number))
		return;

	sms->sca.type = sca->type;
	strncpy(sms->sca.number, sca->number, OFONO_MAX_PHONE_NUMBER_LENGTH);
	sms->sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';

	value = phone_number_to_string(&sms->sca);

	ofono_dbus_signal_property_changed(conn, modem->path,
						SMS_MANAGER_INTERFACE,
						"ServiceCenterAddress",
						DBUS_TYPE_STRING, &value);
}

static DBusMessage *generate_get_properties_reply(struct ofono_modem *modem,
							DBusMessage *msg)
{
	struct sms_manager_data *sms = modem->sms_manager;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *sca;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	sca = phone_number_to_string(&sms->sca);

	ofono_dbus_dict_append(&dict, "ServiceCenterAddress", DBUS_TYPE_STRING,
				&sca);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void sms_sca_query_cb(const struct ofono_error *error,
				const struct ofono_phone_number *sca, void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto out;

	set_sca(modem, sca);

	sms->flags |= SMS_MANAGER_FLAG_CACHED;

out:
	if (sms->pending) {
		DBusMessage *reply = generate_get_properties_reply(modem,
								sms->pending);
		__ofono_dbus_pending_reply(&sms->pending, reply);
	}
}

static DBusMessage *sms_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;

	if (sms->pending)
		return __ofono_error_busy(msg);

	if (!sms->ops->sca_query)
		return __ofono_error_not_implemented(msg);

	if (sms->flags & SMS_MANAGER_FLAG_CACHED)
		return generate_get_properties_reply(modem, msg);

	sms->pending = dbus_message_ref(msg);

	sms->ops->sca_query(modem, sms_sca_query_cb, modem);

	return NULL;
}

static void sca_set_query_callback(const struct ofono_error *error,
					const struct ofono_phone_number *sca,
					void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Set SCA succeeded, but query failed");
		sms->flags &= ~SMS_MANAGER_FLAG_CACHED;
		reply = __ofono_error_failed(sms->pending);
		__ofono_dbus_pending_reply(&sms->pending, reply);
		return;
	}

	set_sca(modem, sca);

	reply = dbus_message_new_method_return(sms->pending);
	__ofono_dbus_pending_reply(&sms->pending, reply);
}

static void sca_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Setting SCA failed");
		__ofono_dbus_pending_reply(&sms->pending,
					__ofono_error_failed(sms->pending));
		return;
	}

	sms->ops->sca_query(modem, sca_set_query_callback, modem);
}

static DBusMessage *sms_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (sms->pending)
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

	if (!strcmp(property, "ServiceCenterAddress")) {
		const char *value;
		struct ofono_phone_number sca;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (strlen(value) == 0 || !valid_phone_number_format(value))
			return __ofono_error_invalid_format(msg);

		if (!sms->ops->sca_set)
			return __ofono_error_not_implemented(msg);

		string_to_phone_number(value, &sca);

		sms->pending = dbus_message_ref(msg);

		sms->ops->sca_set(modem, &sca, sca_set_callback, modem);
		return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

static void tx_finished(const struct ofono_error *error, int mr, void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;
	struct pending_pdu *pdu;

	ofono_debug("tx_finished");

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Sending failed, retrying in 5 seconds...");
		sms->tx_source = g_timeout_add_seconds(5, tx_next, modem);
		return;
	}

	pdu = g_queue_pop_head(sms->txq);
	g_free(pdu);

	ofono_debug("Peeking in the queue");

	if (g_queue_peek_head(sms->txq)) {
		ofono_debug("Scheduling next");
		sms->tx_source = g_timeout_add(0, tx_next, modem);
	}
}

static gboolean tx_next(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sms_manager_data *sms = modem->sms_manager;
	time_t ts;
	int send_mms = 0;
	struct pending_pdu *pdu = g_queue_peek_head(sms->txq);
	struct ofono_error error;

	error.type = OFONO_ERROR_TYPE_NO_ERROR;

	ofono_debug("tx_next: %p", pdu);

	sms->tx_source = 0;

	if (!pdu)
		return FALSE;

	ts = time(NULL);

	if ((g_queue_get_length(sms->txq) > 1) &&
			((ts - sms->last_mms) > 60))
		send_mms = 1;

	sms->ops->submit(modem, pdu->pdu, pdu->pdu_len, pdu->tpdu_len, send_mms,
				tx_finished, modem);

	return FALSE;
}

static void set_ref_and_to(GSList *msg_list, guint16 ref, int offset,
				const char *to)
{
	GSList *l;
	struct sms *sms;

	for (l = msg_list; l; l = l->next) {
		sms = l->data;

		if (offset != 0) {
			sms->submit.ud[offset] = (ref & 0xf0) >> 8;
			sms->submit.ud[offset+1] = (ref & 0x0f);
		}

		sms_address_from_string(&sms->submit.daddr, to);
	}
}

static void append_tx_queue(struct ofono_modem *modem, GSList *msg_list)
{
	struct sms_manager_data *sms = modem->sms_manager;
	struct sms *s;
	GSList *l;
	struct pending_pdu *pdu;
	gboolean start = FALSE;

	if (g_queue_peek_head(sms->txq) == NULL)
		start = TRUE;

	for (l = msg_list; l; l = l->next) {
		s = l->data;

		pdu = g_new(struct pending_pdu, 1);

		sms_encode(s, &pdu->pdu_len, &pdu->tpdu_len, pdu->pdu);

		ofono_debug("pdu_len: %d, tpdu_len: %d",
				pdu->pdu_len, pdu->tpdu_len);

		g_queue_push_tail(sms->txq, pdu);
	}

	if (start)
		sms->tx_source = g_timeout_add(0, tx_next, modem);
}

static DBusMessage *sms_send_message(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct sms_manager_data *sms = modem->sms_manager;
	char **tos;
	int num_to;
	char *text;
	int i;
	GSList *msg_list;
	int ref_offset;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
					&tos, &num_to, DBUS_TYPE_STRING, &text,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (num_to == 0) {
		dbus_free_string_array(tos);
		return __ofono_error_invalid_format(msg);
	}

	ofono_debug("Got %d recipients", num_to);

	for (i = 0; i < num_to; i++) {
		if (valid_phone_number_format(tos[i]))
			continue;

		dbus_free_string_array(tos);
		return __ofono_error_invalid_format(msg);
	}

	msg_list = sms_text_prepare(text, 0, TRUE, &ref_offset);

	if (!msg_list) {
		dbus_free_string_array(tos);
		return __ofono_error_invalid_format(msg);
	}

	for (i = 0; i < num_to; i++) {
		ofono_debug("ref: %d, offset: %d", sms->ref, ref_offset);
		set_ref_and_to(msg_list, sms->ref, ref_offset, tos[i]);
		append_tx_queue(modem, msg_list);

		if (sms->ref == 65536)
			sms->ref = 1;
		else
			sms->ref = sms->ref + 1;
	}

	dbus_free_string_array(tos);
	g_slist_foreach(msg_list, (GFunc)g_free, NULL);
	g_slist_free(msg_list);

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable sms_manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	sms_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SetProperty",	"sv",	"",		sms_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SendMessage",	"ass",	"",		sms_send_message },
	{ }
};

static GDBusSignalTable sms_manager_signals[] = {
	{ "PropertyChanged",	"sv"		},
	{ "IncomingMessage",	"sa{sv}"	},
	{ "ImmediateMessage",	"sa{sv}"	},
	{ }
};

static void dispatch_app_datagram(struct ofono_modem *modem, int dst, int src,
					unsigned char *buf, long len)
{
	ofono_debug("Got app datagram for dst port: %d, src port: %d",
			dst, src);
	ofono_debug("Contents-Len: %ld", len);
}

static void dispatch_text_message(struct ofono_modem *modem,
					const char *message,
					enum sms_class cls,
					const struct sms_address *addr,
					const struct sms_scts *scts)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char buf[128];
	const char *signal_name;
	time_t ts;
	struct tm remote;
	const char *str = buf;

	if (!message)
		return;

	if (cls == SMS_CLASS_0)
		signal_name = "ImmediateMessage";
	else
		signal_name = "IncomingMessage";

	signal = dbus_message_new_signal(modem->path, SMS_MANAGER_INTERFACE,
						signal_name);

	if (!signal)
		return;

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &message);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	ts = sms_scts_to_time(scts, &remote);

	strftime(buf, 127, "%a, %d %b %Y %H:%M:%S %z", localtime(&ts));
	buf[127] = '\0';
	ofono_dbus_dict_append(&dict, "LocalSentTime", DBUS_TYPE_STRING, &str);

	strftime(buf, 127, "%a, %d %b %Y %H:%M:%S %z", &remote);
	buf[127] = '\0';
	ofono_dbus_dict_append(&dict, "SentTime", DBUS_TYPE_STRING, &str);

	str = sms_address_to_string(addr);
	ofono_dbus_dict_append(&dict, "Sender", DBUS_TYPE_STRING, &str);

	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(conn, signal);
}

static void sms_dispatch(struct ofono_modem *modem, GSList *sms_list)
{
	GSList *l;
	const struct sms *sms;
	enum sms_charset uninitialized_var(old_charset);
	enum sms_class cls;
	int srcport = -1;
	int dstport = -1;

	if (sms_list == NULL)
		return;

	/* Qutoting 23.040: The TP elements in the SMS‑SUBMIT PDU, apart from
	 * TP‑MR, TP-SRR, TP‑UDL and TP‑UD, should remain unchanged for each
	 * SM which forms part of a concatenated SM, otherwise this may lead
	 * to irrational behaviour
	 *
	 * This means that we assume that at least the charset is the same
	 * across all parts of the SMS in the case of 8-bit data.  Other
	 * cases can be handled by converting to UTF8.
	 *
	 * We also check that if 8-bit or 16-bit application addressing is
	 * used, the addresses are the same across all segments.
	 */

	for (l = sms_list; l; l = l->next) {
		guint8 dcs;
		gboolean comp = FALSE;
		enum sms_charset charset;
		int cdst = -1;
		int csrc = -1;
		gboolean is_8bit;

		sms = l->data;
		dcs = sms->deliver.dcs;

		if (sms_mwi_dcs_decode(dcs, NULL, &charset, NULL, NULL))
			cls = SMS_CLASS_UNSPECIFIED;
		else if (!sms_dcs_decode(dcs, &cls, &charset, &comp, NULL)) {
			ofono_error("The deliver DCS is not recognized");
			return;
		}

		if (comp) {
			ofono_error("Compressed data not supported");
			return;
		}

		if (l == sms_list)
			old_charset = charset;

		if (charset == SMS_CHARSET_8BIT && charset != old_charset) {
			ofono_error("Can't concatenate disparate charsets");
			return;
		}

		if (sms_extract_app_port(sms, &cdst, &csrc, &is_8bit) &&
				(l == sms_list)) {
			srcport = is_8bit ? csrc : (csrc << 8);
			dstport = is_8bit ? cdst : (cdst << 8);
		}

		if (srcport != csrc || dstport != cdst) {
			ofono_error("Source / Destination ports across "
					"concatenated message are not the "
					"same, ignoring");
			return;
		}
	}

	/* Handle datagram */
	if (old_charset == SMS_CHARSET_8BIT) {
		unsigned char *buf;
		long len;

		if (srcport == -1 || dstport == -1) {
			ofono_error("Got an 8-bit encoded message, however "
					"no valid src/address port, ignore");
			return;
		}

		buf = sms_decode_datagram(sms_list, &len);

		if (!buf)
			return;

		dispatch_app_datagram(modem, dstport, srcport, buf, len);

		g_free(buf);
	} else {
		char *message = sms_decode_text(sms_list);

		if (!message)
			return;

		sms = sms_list->data;

		dispatch_text_message(modem, message, cls, &sms->deliver.oaddr,
					&sms->deliver.scts);
		g_free(message);
	}
}

static void handle_deliver(struct ofono_modem *modem,
				const struct sms *incoming)
{
	struct sms_manager_data *sms = modem->sms_manager;
	GSList *l;
	guint16 ref;
	guint8 max;
	guint8 seq;

	if (sms_extract_concatenation(incoming, &ref, &max, &seq)) {
		GSList *sms_list;

		if (!sms->assembly)
			return;

		sms_list = sms_assembly_add_fragment(sms->assembly,
							incoming, time(NULL),
							&incoming->deliver.oaddr,
							ref, max, seq);

		if (!sms_list)
			return;

		sms_dispatch(modem, sms_list);
		g_slist_foreach(sms_list, (GFunc)g_free, NULL);
		g_slist_free(sms_list);

		return;
	}

	l = g_slist_append(NULL, (void *)incoming);
	sms_dispatch(modem, l);
	g_slist_free(l);
}

void ofono_sms_deliver_notify(struct ofono_modem *modem, unsigned char *pdu,
				int len, int tpdu_len)
{
	struct sms sms;
	enum sms_class cls;
	gboolean discard;

	if (!sms_decode(pdu, len, FALSE, tpdu_len, &sms)) {
		ofono_error("Unable to decode PDU");
		return;
	}

	if (sms.type != SMS_TYPE_DELIVER) {
		ofono_error("Expecting a DELIVER pdu");
		return;
	}

	if (sms.deliver.pid == SMS_PID_TYPE_SM_TYPE_0) {
		ofono_debug("Explicitly ignoring type 0 SMS");
		return;
	}

	/* This is an older style MWI notification, process MWI
	 * headers and handle it like any other message */
	if (sms.deliver.pid == SMS_PID_TYPE_RETURN_CALL) {
		ofono_handle_sms_mwi(modem, &sms, &discard);

		if (discard)
			return;

		goto out;
	}

	/* The DCS indicates this is an MWI notification, process it
	 * and then handle the User-Data as any other message */
	if (sms_mwi_dcs_decode(sms.deliver.dcs, NULL, NULL, NULL, NULL)) {
		ofono_handle_sms_mwi(modem, &sms, &discard);

		if (discard)
			return;

		goto out;
	}

	if (!sms_dcs_decode(sms.deliver.dcs, &cls, NULL, NULL, NULL)) {
		ofono_error("Unknown / Reserved DCS.  Ignoring");
		return;
	}

	switch (sms.deliver.pid) {
	case SMS_PID_TYPE_ME_DOWNLOAD:
		if (cls == SMS_CLASS_1) {
			ofono_error("ME Download message ignored");
			return;
		}

		break;
	case SMS_PID_TYPE_ME_DEPERSONALIZATION:
		if (sms.deliver.dcs == 0x11) {
			ofono_error("ME Depersonalization message ignored");
			return;
		}

		break;
	case SMS_PID_TYPE_USIM_DOWNLOAD:
	case SMS_PID_TYPE_ANSI136:
		if (cls == SMS_CLASS_2) {
			ofono_error("(U)SIM Download messages not supported");
			return;
		}

		/* Otherwise handle in a "normal" way */
		break;
	default:
		break;
	}

	/* Check to see if the SMS has any other MWI related headers,
	 * as sometimes they are "tacked on" by the SMSC.
	 * While we're doing this we also check for messages containing
	 * WCMP headers or headers that can't possibly be in a normal
	 * message.  If we find messages like that, we ignore them.
	 */
	if (sms.deliver.udhi) {
		struct sms_udh_iter iter;
		enum sms_iei iei;

		if (!sms_udh_iter_init(&sms, &iter))
			goto out;

		while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
				SMS_IEI_INVALID) {
			if (iei > 0x25) {
				ofono_error("Reserved / Unknown / USAT"
						"header in use, ignore");
				return;
			}

			switch (iei) {
			case SMS_IEI_SPECIAL_MESSAGE_INDICATION:
			case SMS_IEI_ENHANCED_VOICE_MAIL_INFORMATION:
				/* TODO: ignore if not in the very first
				 * segment of a concatenated SM so as not
				 * to repeat the indication.  */
				ofono_handle_sms_mwi(modem, &sms, &discard);

				if (discard)
					return;

				goto out;
			case SMS_IEI_WCMP:
				ofono_error("No support for WCMP, ignoring");
				return;
			default:
				sms_udh_iter_next(&iter);
			}
		}
	}

out:
	handle_deliver(modem, &sms);
}

void ofono_sms_status_notify(struct ofono_modem *modem, unsigned char *pdu,
				int len, int tpdu_len)
{
	ofono_error("SMS Status-Report not yet handled");
}

int ofono_sms_manager_register(struct ofono_modem *modem,
					struct ofono_sms_ops *ops)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	if (ofono_message_waiting_register(modem))
		return -1;

	modem->sms_manager = sms_manager_create();

	if (!modem->sms_manager)
		return -1;

	modem->sms_manager->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					SMS_MANAGER_INTERFACE,
					sms_manager_methods,
					sms_manager_signals,
					NULL, modem,
					sms_manager_destroy)) {
		ofono_error("Could not register SmsManager interface");
		sms_manager_destroy(modem);

		return -1;
	}

	ofono_debug("SmsManager interface for modem: %s created",
			modem->path);

	ofono_modem_add_interface(modem, SMS_MANAGER_INTERFACE);

	return 0;
}

void ofono_sms_manager_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	g_dbus_unregister_interface(conn, modem->path,
					SMS_MANAGER_INTERFACE);

	ofono_modem_remove_interface(modem, SMS_MANAGER_INTERFACE);

	ofono_message_waiting_unregister(modem);
}
