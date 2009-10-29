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
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"
#include "util.h"
#include "smsutil.h"
#include "simutil.h"

#define CBS_MANAGER_INTERFACE "org.ofono.CbsManager"

static GSList *g_drivers = NULL;

enum etws_topic_type {
	ETWS_TOPIC_TYPE_EARTHQUAKE = 4352,
	ETWS_TOPIC_TYPE_TSUNAMI = 4353,
	ETWS_TOPIC_TYPE_EARTHQUAKE_TSUNAMI = 4354,
	ETWS_TOPIC_TYPE_TEST = 4355,
	ETWS_TOPIC_TYPE_EMERGENCY = 4356,
};

struct ofono_cbs {
	DBusMessage *pending;
	struct cbs_assembly *assembly;
	GSList *topics;
	GSList *new_topics;
	struct ofono_sim *sim;
	struct ofono_netreg *netreg;
	unsigned int netreg_watch;
	unsigned int location_watch;
	unsigned short efcbmi_length;
	GSList *efcbmi_contents;
	unsigned short efcbmir_length;
	GSList *efcbmir_contents;
	unsigned short efcbmid_length;
	GSList *efcbmid_contents;
	guint reset_source;
	int lac;
	int ci;
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	const struct ofono_cbs_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static void cbs_dispatch_base_station_id(struct ofono_cbs *cbs, const char *id)
{
	DBG("Base station id: %s", id);

	if (cbs->netreg == NULL)
		return;

	if (cbs->reset_source) {
		g_source_remove(cbs->reset_source);
		cbs->reset_source = 0;
	}

	__ofono_netreg_set_base_station_name(cbs->netreg, id);
}

static void cbs_dispatch_emergency(struct ofono_cbs *cbs, const char *message,
					enum etws_topic_type topic,
					gboolean alert, gboolean popup)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cbs->atom);
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t boolean;
	const char *emergency_str;

	if (topic == ETWS_TOPIC_TYPE_TEST) {
		ofono_error("Explicitly ignoring ETWS Test messages");
		return;
	}

	switch (topic) {
	case ETWS_TOPIC_TYPE_EARTHQUAKE:
		emergency_str = "Earthquake";
		break;
	case ETWS_TOPIC_TYPE_TSUNAMI:
		emergency_str = "Tsunami";
		break;
	case ETWS_TOPIC_TYPE_EARTHQUAKE_TSUNAMI:
		emergency_str = "Earthquake+Tsunami";
		break;
	case ETWS_TOPIC_TYPE_EMERGENCY:
		emergency_str = "Other";
		break;
	default:
		return;
	};

	signal = dbus_message_new_signal(path, CBS_MANAGER_INTERFACE,
						"EmergencyBroadcast");

	if (!signal)
		return;

	dbus_message_iter_init_append(signal, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &message);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	ofono_dbus_dict_append(&dict, "EmergencyType",
				DBUS_TYPE_STRING, &emergency_str);

	boolean = alert;
	ofono_dbus_dict_append(&dict, "EmergencyAlert",
				DBUS_TYPE_BOOLEAN, &boolean);

	boolean = popup;
	ofono_dbus_dict_append(&dict, "Popup", DBUS_TYPE_BOOLEAN, &boolean);

	dbus_message_iter_close_container(&iter, &dict);
	g_dbus_send_message(conn, signal);
}

static void cbs_dispatch_text(struct ofono_cbs *cbs, enum sms_class cls,
				unsigned short channel, const char *message)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(cbs->atom);

	g_dbus_emit_signal(conn, path, CBS_MANAGER_INTERFACE,
				"IncomingBroadcast",
				DBUS_TYPE_STRING, &message,
				DBUS_TYPE_UINT16, &channel,
				DBUS_TYPE_INVALID);
}

void ofono_cbs_notify(struct ofono_cbs *cbs, const unsigned char *pdu,
				int pdu_len)
{
	struct cbs c;
	enum sms_class cls;
	gboolean udhi;
	gboolean comp;
	GSList *cbs_list;
	enum sms_charset charset;
	char *message;
	char iso639_lang[3];

	if (cbs->assembly == NULL)
		return;

	if (!cbs_decode(pdu, pdu_len, &c)) {
		ofono_error("Unable to decode CBS PDU");
		return;
	}

	if (!cbs_dcs_decode(c.dcs, &udhi, &cls, &charset, &comp, NULL, NULL)) {
		ofono_error("Unknown / Reserved DCS.  Ignoring");
		return;
	}

	if (udhi) {
		ofono_error("CBS messages with UDH not supported");
		return;
	}

	if (charset == SMS_CHARSET_8BIT) {
		ofono_error("Datagram CBS not supported");
		return;
	}

	if (comp) {
		ofono_error("CBS messages with compression not supported");
		return;
	}

	cbs_list = cbs_assembly_add_page(cbs->assembly, &c);

	if (cbs_list == NULL)
		return;

	message = cbs_decode_text(cbs_list, iso639_lang);

	if (message == NULL)
		goto out;

	if (c.message_identifier >= ETWS_TOPIC_TYPE_EARTHQUAKE &&
			c.message_identifier <= ETWS_TOPIC_TYPE_EMERGENCY) {
		gboolean alert = FALSE;
		gboolean popup = FALSE;

		/* 3GPP 23.041 9.4.1.2.1: Alert is encoded in bit 9 */
		if (c.message_code & (1 << 9))
			alert = TRUE;

		/* 3GPP 23.041 9.4.1.2.1: Popup is encoded in bit 8 */
		if (c.message_code & (1 << 8))
			popup = TRUE;

		cbs_dispatch_emergency(cbs, message,
					c.message_identifier, alert, popup);
		goto out;
	}

	/* 3GPP 23.041: NOTE 5:	Code 00 is intended for use by the
	 * network operators for base station IDs.
	 */
	if (c.gs == CBS_GEO_SCOPE_CELL_IMMEDIATE) {
		cbs_dispatch_base_station_id(cbs, message);
		goto out;
	}

	cbs_dispatch_text(cbs, cls, c.message_identifier, message);

out:
	g_free(message);
	g_slist_foreach(cbs_list, (GFunc)g_free, NULL);
	g_slist_free(cbs_list);
}

static DBusMessage *cbs_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_cbs *cbs = data;
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	char *topics;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	topics = cbs_topic_ranges_to_string(cbs->topics);
	ofono_dbus_dict_append(&dict, "Topics", DBUS_TYPE_STRING, &topics);
	g_free(topics);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void cbs_set_topics_cb(const struct ofono_error *error, void *data)
{
	struct ofono_cbs *cbs = data;
	const char *path = __ofono_atom_get_path(cbs->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	char *topics;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		g_slist_foreach(cbs->new_topics, (GFunc)g_free, NULL);
		g_slist_free(cbs->new_topics);
		cbs->new_topics = NULL;

		ofono_debug("Setting Cell Broadcast topics failed");
		__ofono_dbus_pending_reply(&cbs->pending,
					__ofono_error_failed(cbs->pending));
		return;
	}

	g_slist_foreach(cbs->topics, (GFunc)g_free, NULL);
	g_slist_free(cbs->topics);
	cbs->topics = cbs->new_topics;
	cbs->new_topics = NULL;

	reply = dbus_message_new_method_return(cbs->pending);
	__ofono_dbus_pending_reply(&cbs->pending, reply);

	topics = cbs_topic_ranges_to_string(cbs->topics);
	ofono_dbus_signal_property_changed(conn, path,
						CBS_MANAGER_INTERFACE,
						"Topics",
						DBUS_TYPE_STRING, &topics);
	g_free(topics);
}

static DBusMessage *cbs_set_topics(struct ofono_cbs *cbs, const char *value,
					DBusMessage *msg)
{
	GSList *topics;
	GSList *etws_topics = NULL;
	char *topic_str;
	struct cbs_topic_range etws_range = { 4352, 4356 };

	topics = cbs_extract_topic_ranges(value);

	if (topics == NULL && value[0] != '\0')
		return __ofono_error_invalid_format(msg);

	if (!cbs->driver->set_topics)
		return __ofono_error_not_implemented(msg);

	cbs->new_topics = topics;

	if (topics != NULL)
		etws_topics = g_slist_copy(topics);

	etws_topics = g_slist_append(etws_topics, &etws_range);
	topic_str = cbs_topic_ranges_to_string(etws_topics);
	g_slist_free(etws_topics);

	cbs->pending = dbus_message_ref(msg);
	cbs->driver->set_topics(cbs, topic_str, cbs_set_topics_cb, cbs);
	g_free(topic_str);

	return NULL;
}

static DBusMessage *cbs_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_cbs *cbs = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (cbs->pending)
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

	if (!strcmp(property, "Topics")) {
		const char *value;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		return cbs_set_topics(cbs, value, msg);
	}

	return __ofono_error_invalid_args(msg);
}

static GDBusMethodTable cbs_manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cbs_get_properties },
	{ "SetProperty",	"sv",	"",		cbs_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cbs_manager_signals[] = {
	{ "PropertyChanged",	"sv"		},
	{ "IncomingBroadcast",	"sq"		},
	{ "EmergencyBroadcast", "sa{sv}"	},
	{ }
};

int ofono_cbs_driver_register(const struct ofono_cbs_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_cbs_driver_unregister(const struct ofono_cbs_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void cbs_unregister(struct ofono_atom *atom)
{
	struct ofono_cbs *cbs = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	g_dbus_unregister_interface(conn, path, CBS_MANAGER_INTERFACE);
	ofono_modem_remove_interface(modem, CBS_MANAGER_INTERFACE);

	if (cbs->topics) {
		g_slist_foreach(cbs->topics, (GFunc)g_free, NULL);
		g_slist_free(cbs->topics);
		cbs->topics = NULL;
	}

	if (cbs->new_topics) {
		g_slist_foreach(cbs->new_topics, (GFunc)g_free, NULL);
		g_slist_free(cbs->new_topics);
		cbs->new_topics = NULL;
	}

	cbs->sim = NULL;

	if (cbs->reset_source) {
		g_source_remove(cbs->reset_source);
		cbs->reset_source = 0;

		if (cbs->netreg)
			__ofono_netreg_set_base_station_name(cbs->netreg, NULL);
	}

	if (cbs->netreg_watch) {
		if (cbs->location_watch) {
			__ofono_netreg_remove_status_watch(cbs->netreg,
							cbs->location_watch);
			cbs->location_watch = 0;
		}

		__ofono_modem_remove_atom_watch(modem, cbs->netreg_watch);
		cbs->netreg_watch = 0;
		cbs->netreg = NULL;
	}
}

static void cbs_remove(struct ofono_atom *atom)
{
	struct ofono_cbs *cbs = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (cbs == NULL)
		return;

	if (cbs->driver && cbs->driver->remove)
		cbs->driver->remove(cbs);

	cbs_assembly_free(cbs->assembly);
	cbs->assembly = NULL;

	g_free(cbs);
}

struct ofono_cbs *ofono_cbs_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_cbs *cbs;
	GSList *l;

	if (driver == NULL)
		return NULL;

	cbs = g_try_new0(struct ofono_cbs, 1);

	if (cbs == NULL)
		return NULL;

	cbs->assembly = cbs_assembly_new();
	cbs->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_CBS,
						cbs_remove, cbs);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_cbs_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(cbs, vendor, data) < 0)
			continue;

		cbs->driver = drv;
		break;
	}

	return cbs;
}

static void sim_cbmi_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_cbs *cbs = userdata;
	unsigned short mi;
	int i;
	char *str;

	if (!ok)
		return;

	if ((length % 2) == 1 || length < 2)
		return;

	cbs->efcbmi_length = length;

	for (i = 0; i < length; i += 2) {
		struct cbs_topic_range *range;

		if (data[i] == 0xff && data[i+1] == 0xff)
			continue;

		mi = (data[i] << 8) + data[i+1];

		if (mi > 999)
			continue;

		range = g_new0(struct cbs_topic_range, 1);
		range->min = mi;
		range->max = mi;

		cbs->efcbmi_contents = g_slist_prepend(cbs->efcbmi_contents,
							range);
	}

	if (cbs->efcbmi_contents == NULL)
		return;

	cbs->efcbmi_contents = g_slist_reverse(cbs->efcbmi_contents);

	str = cbs_topic_ranges_to_string(cbs->efcbmi_contents);
	ofono_debug("Got cbmi: %s", str);
	g_free(str);

	cbs->efcbmi_length = 0;
	g_slist_foreach(cbs->efcbmi_contents, (GFunc)g_free, NULL);
	g_slist_free(cbs->efcbmi_contents);
	cbs->efcbmi_contents = NULL;
}

static void sim_cbmir_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_cbs *cbs = userdata;
	int i;
	unsigned short min;
	unsigned short max;
	char *str;

	if (!ok)
		return;

	if ((length % 4) != 0)
		return;

	cbs->efcbmir_length = length;

	for (i = 0; i < length; i += 4) {
		struct cbs_topic_range *range;

		if (data[i] == 0xff && data[i+1] == 0xff &&
				data[i+2] == 0xff && data[i+3] == 0xff)
			continue;

		min = (data[i] << 8) + data[i+1];
		max = (data[i+2] << 8) + data[i+3];

		if (min > 999 || max > 999 || min > max)
			continue;

		range = g_new0(struct cbs_topic_range, 1);
		range->min = min;
		range->max = max;

		cbs->efcbmir_contents = g_slist_prepend(cbs->efcbmir_contents,
							range);
	}

	if (cbs->efcbmir_contents == NULL)
		return;

	cbs->efcbmir_contents = g_slist_reverse(cbs->efcbmir_contents);

	str = cbs_topic_ranges_to_string(cbs->efcbmir_contents);
	ofono_debug("Got cbmir: %s", str);
	g_free(str);

	cbs->efcbmir_length = 0;
	g_slist_foreach(cbs->efcbmir_contents, (GFunc)g_free, NULL);
	g_slist_free(cbs->efcbmir_contents);
	cbs->efcbmir_contents = NULL;
}

static void sim_cbmid_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_cbs *cbs = userdata;
	unsigned short mi;
	int i;
	char *str;

	if (!ok)
		return;

	if ((length % 2) == 1 || length < 2)
		return;

	cbs->efcbmid_length = length;

	for (i = 0; i < length; i += 2) {
		struct cbs_topic_range *range;

		if (data[i] == 0xff && data[i+1] == 0xff)
			continue;

		mi = (data[i] << 8) + data[i+1];

		range = g_new0(struct cbs_topic_range, 1);
		range->min = mi;
		range->max = mi;

		cbs->efcbmid_contents = g_slist_prepend(cbs->efcbmid_contents,
							range);
	}

	if (cbs->efcbmid_contents == NULL)
		return;

	cbs->efcbmid_contents = g_slist_reverse(cbs->efcbmid_contents);

	str = cbs_topic_ranges_to_string(cbs->efcbmid_contents);
	ofono_debug("Got cbmid: %s", str);
	g_free(str);

	cbs->efcbmid_length = 0;
	g_slist_foreach(cbs->efcbmid_contents, (GFunc)g_free, NULL);
	g_slist_free(cbs->efcbmid_contents);
	cbs->efcbmid_contents = NULL;
}

static void cbs_got_imsi(struct ofono_cbs *cbs)
{
	const char *imsi = ofono_sim_get_imsi(cbs->sim);

	ofono_debug("Got IMSI: %s", imsi);

	ofono_sim_read(cbs->sim, SIM_EFCBMI_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_cbmi_read_cb, cbs);
	ofono_sim_read(cbs->sim, SIM_EFCBMIR_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_cbmir_read_cb, cbs);
	ofono_sim_read(cbs->sim, SIM_EFCBMID_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_cbmid_read_cb, cbs);
}

static gboolean reset_base_station_name(gpointer user)
{
	struct ofono_cbs *cbs = user;

	cbs->reset_source = 0;

	if (cbs->netreg == NULL)
		goto out;

	__ofono_netreg_set_base_station_name(cbs->netreg, NULL);

out:
	return FALSE;
}

static void cbs_location_changed(int status, int lac, int ci, int tech,
					const char *mcc, const char *mnc,
					void *data)
{
	struct ofono_cbs *cbs = data;
	gboolean plmn_changed = FALSE;
	gboolean lac_changed = FALSE;
	gboolean ci_changed = FALSE;

	DBG("%d, %d, %d, %d, %s%s", status, lac, ci, tech, mcc, mnc);

	if (!mcc || !mnc) {
		if (cbs->mcc[0] == '\0' && cbs->mnc[0] == '\0')
			return;

		memset(cbs->mcc, 0, sizeof(cbs->mcc));
		memset(cbs->mnc, 0, sizeof(cbs->mnc));

		plmn_changed = TRUE;
		goto out;
	}

	if (strcmp(cbs->mcc, mcc) || strcmp(cbs->mnc, mnc)) {
		memcpy(cbs->mcc, mcc, sizeof(cbs->mcc));
		memcpy(cbs->mnc, mnc, sizeof(cbs->mnc));

		plmn_changed = TRUE;
		goto out;
	}

	if (cbs->lac != lac) {
		cbs->lac = lac;

		lac_changed = TRUE;
		goto out;
	}

	if (cbs->ci != ci) {
		cbs->ci = ci;

		ci_changed = TRUE;
		goto out;
	}

	return;

out:
	DBG("%d, %d, %d", plmn_changed, lac_changed, ci_changed);

	/* In order to minimize signal transmissions we wait about X seconds
	 * before reseting the base station id.  The hope is that we receive
	 * another cell broadcast with the new base station name within
	 * that time
	 */
	if (lac_changed || ci_changed) {
		cbs->reset_source = 
			g_timeout_add_seconds(3, reset_base_station_name, cbs);
	}

	cbs_assembly_location_changed(cbs->assembly, plmn_changed,
					lac_changed, ci_changed);
}

static void netreg_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_cbs *cbs = data;
	const char *mcc;
	const char *mnc;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		cbs->location_watch = 0;
		cbs->netreg = 0;
		return;
	}

	cbs->netreg = __ofono_atom_get_data(atom);
	cbs->location_watch = __ofono_netreg_add_status_watch(cbs->netreg,
					cbs_location_changed, cbs, NULL);

	mcc = ofono_netreg_get_mcc(cbs->netreg);
	mnc = ofono_netreg_get_mnc(cbs->netreg);

	if (mcc && mnc) {
		memcpy(cbs->mcc, mcc, sizeof(cbs->mcc));
		memcpy(cbs->mnc, mnc, sizeof(cbs->mnc));
	} else {
		memset(cbs->mcc, 0, sizeof(cbs->mcc));
		memset(cbs->mnc, 0, sizeof(cbs->mnc));
	}

	cbs->lac = ofono_netreg_get_location(cbs->netreg);
	cbs->ci = ofono_netreg_get_cellid(cbs->netreg);

	/* Clear out the cbs assembly just in case, worst case
	 * we will receive the cell broadcasts again
	 */
	cbs_assembly_location_changed(cbs->assembly, TRUE, TRUE, TRUE);
}

void ofono_cbs_register(struct ofono_cbs *cbs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(cbs->atom);
	const char *path = __ofono_atom_get_path(cbs->atom);
	struct ofono_atom *sim_atom;
	struct ofono_atom *netreg_atom;

	if (!g_dbus_register_interface(conn, path,
					CBS_MANAGER_INTERFACE,
					cbs_manager_methods,
					cbs_manager_signals,
					NULL, cbs, NULL)) {
		ofono_error("Could not create %s interface",
				CBS_MANAGER_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, CBS_MANAGER_INTERFACE);

	sim_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SIM);

	if (sim_atom) {
		cbs->sim = __ofono_atom_get_data(sim_atom);

		if (ofono_sim_get_ready(cbs->sim) == TRUE)
			cbs_got_imsi(cbs);
	}

	cbs->netreg_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_NETREG,
					netreg_watch, cbs, NULL);

	netreg_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_NETREG);

	if (netreg_atom && __ofono_atom_get_registered(netreg_atom))
		netreg_watch(netreg_atom,
				OFONO_ATOM_WATCH_CONDITION_REGISTERED, cbs);

	__ofono_atom_register(cbs->atom, cbs_unregister);
}

void ofono_cbs_remove(struct ofono_cbs *cbs)
{
	__ofono_atom_free(cbs->atom);
}

void ofono_cbs_set_data(struct ofono_cbs *cbs, void *data)
{
	cbs->driver_data = data;
}

void *ofono_cbs_get_data(struct ofono_cbs *cbs)
{
	return cbs->driver_data;
}
