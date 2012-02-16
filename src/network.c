/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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
#include "simutil.h"
#include "util.h"
#include "storage.h"

#define SETTINGS_STORE "netreg"
#define SETTINGS_GROUP "Settings"

#define NETWORK_REGISTRATION_FLAG_HOME_SHOW_PLMN	0x1
#define NETWORK_REGISTRATION_FLAG_ROAMING_SHOW_SPN	0x2
#define NETWORK_REGISTRATION_FLAG_READING_PNN		0x4

enum network_registration_mode {
	NETWORK_REGISTRATION_MODE_AUTO =	0,
	NETWORK_REGISTRATION_MODE_MANUAL =	2,
	NETWORK_REGISTRATION_MODE_AUTO_ONLY =	5, /* Out of range of 27.007 */
};

/* 27.007 Section 7.3 <stat> */
enum operator_status {
	OPERATOR_STATUS_UNKNOWN =	0,
	OPERATOR_STATUS_AVAILABLE =	1,
	OPERATOR_STATUS_CURRENT =	2,
	OPERATOR_STATUS_FORBIDDEN =	3,
};

struct ofono_netreg {
	int status;
	int location;
	int cellid;
	int technology;
	int mode;
	char *base_station;
	struct network_operator_data *current_operator;
	GSList *operator_list;
	struct ofono_network_registration_ops *ops;
	int flags;
	DBusMessage *pending;
	int signal_strength;
	struct sim_spdi *spdi;
	struct sim_eons *eons;
	struct ofono_sim *sim;
	struct ofono_sim_context *sim_context;
	GKeyFile *settings;
	char *imsi;
	struct ofono_watchlist *status_watches;
	const struct ofono_netreg_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	unsigned int hfp_watch;
	unsigned int spn_watch;
};

struct network_operator_data {
	char name[OFONO_MAX_OPERATOR_NAME_LENGTH + 1];
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int status;
	unsigned int techs;
	const struct sim_eons_operator_info *eons_info;
	struct ofono_netreg *netreg;
};

static GSList *g_drivers = NULL;

static const char *registration_mode_to_string(int mode)
{
	switch (mode) {
	case NETWORK_REGISTRATION_MODE_AUTO:
		return "auto";
	case NETWORK_REGISTRATION_MODE_AUTO_ONLY:
		return "auto-only";
	case NETWORK_REGISTRATION_MODE_MANUAL:
		return "manual";
	}

	return "unknown";
}

static inline const char *network_operator_status_to_string(int status)
{
	switch (status) {
	case OPERATOR_STATUS_AVAILABLE:
		return "available";
	case OPERATOR_STATUS_CURRENT:
		return "current";
	case OPERATOR_STATUS_FORBIDDEN:
		return "forbidden";
	}

	return "unknown";
}

static char **network_operator_technologies(struct network_operator_data *opd)
{
	unsigned int ntechs = 0;
	char **techs;
	unsigned int i;

	for (i = 0; i < sizeof(opd->techs) * 8; i++) {
		if (opd->techs & (1 << i))
			ntechs += 1;
	}

	techs = g_new0(char *, ntechs + 1);
	ntechs = 0;

	for (i = 0; i < sizeof(opd->techs) * 8; i++) {
		if (!(opd->techs & (1 << i)))
			continue;

		techs[ntechs++] = g_strdup(registration_tech_to_string(i));
	}

	return techs;
}

static void registration_status_callback(const struct ofono_error *error,
					int status, int lac, int ci, int tech,
					void *data)
{
	struct ofono_netreg *netreg = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during registration status query");
		return;
	}

	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void init_register(const struct ofono_error *error, void *data)
{
	struct ofono_netreg *netreg = data;

	if (netreg->driver->registration_status == NULL)
		return;

	netreg->driver->registration_status(netreg,
					registration_status_callback, netreg);
}

static void enforce_auto_only(struct ofono_netreg *netreg)
{
	if (netreg->mode != NETWORK_REGISTRATION_MODE_MANUAL)
		return;

	if (netreg->driver->register_auto == NULL)
		return;

	netreg->driver->register_auto(netreg, init_register, netreg);
}

static void set_registration_mode(struct ofono_netreg *netreg, int mode)
{
	DBusConnection *conn;
	const char *strmode;
	const char *path;

	if (netreg->mode == mode)
		return;

	if (mode == NETWORK_REGISTRATION_MODE_AUTO_ONLY)
		enforce_auto_only(netreg);

	netreg->mode = mode;

	if (netreg->settings) {
		const char *mode;

		if (netreg->mode == NETWORK_REGISTRATION_MODE_MANUAL)
			mode = "manual";
		else
			mode = "auto";

		g_key_file_set_string(netreg->settings, SETTINGS_GROUP,
					"Mode", mode);
		storage_sync(netreg->imsi, SETTINGS_STORE, netreg->settings);
	}

	strmode = registration_mode_to_string(mode);

	conn = ofono_dbus_get_connection();
	path = __ofono_atom_get_path(netreg->atom);

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"Mode", DBUS_TYPE_STRING, &strmode);
}

static void register_callback(const struct ofono_error *error, void *data)
{
	struct ofono_netreg *netreg = data;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(netreg->pending);
	else
		reply = __ofono_error_failed(netreg->pending);

	__ofono_dbus_pending_reply(&netreg->pending, reply);

	if (netreg->driver->registration_status == NULL)
		return;

	netreg->driver->registration_status(netreg,
						registration_status_callback,
						netreg);
}

static struct network_operator_data *
	network_operator_create(const struct ofono_network_operator *op)
{
	struct network_operator_data *opd;

	opd = g_new0(struct network_operator_data, 1);

	memcpy(&opd->name, op->name, sizeof(opd->name));
	memcpy(&opd->mcc, op->mcc, sizeof(opd->mcc));
	memcpy(&opd->mnc, op->mnc, sizeof(opd->mnc));

	opd->status = op->status;

	if (op->tech != -1)
		opd->techs |= 1 << op->tech;

	return opd;
}

static void network_operator_destroy(gpointer user_data)
{
	struct network_operator_data *op = user_data;

	g_free(op);
}

static gint network_operator_compare(gconstpointer a, gconstpointer b)
{
	const struct network_operator_data *opda = a;
	const struct ofono_network_operator *opb = b;

	int comp1;
	int comp2;

	comp1 = strcmp(opda->mcc, opb->mcc);
	comp2 = strcmp(opda->mnc, opb->mnc);

	return comp1 != 0 ? comp1 : comp2;
}

static gint network_operator_data_compare(gconstpointer a, gconstpointer b)
{
	const struct network_operator_data *opa = a;
	const struct network_operator_data *opb = b;

	int comp1;
	int comp2;

	comp1 = strcmp(opa->mcc, opb->mcc);
	comp2 = strcmp(opa->mnc, opb->mnc);

	return comp1 != 0 ? comp1 : comp2;
}

static const char *network_operator_build_path(struct ofono_netreg *netreg,
							const char *mcc,
							const char *mnc)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/operator/%s%s",
			__ofono_atom_get_path(netreg->atom),
			mcc, mnc);

	return path;
}

static void set_network_operator_status(struct network_operator_data *opd,
					int status)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_netreg *netreg = opd->netreg;
	const char *status_str;
	const char *path;

	if (opd->status == status)
		return;

	opd->status = status;

	/* Don't emit for the case where only operator name is reported */
	if (opd->mcc[0] == '\0' && opd->mnc[0] == '\0')
		return;

	status_str = network_operator_status_to_string(status);
	path = network_operator_build_path(netreg, opd->mcc, opd->mnc);

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_OPERATOR_INTERFACE,
					"Status", DBUS_TYPE_STRING,
					&status_str);
}

static void set_network_operator_techs(struct network_operator_data *opd,
					unsigned int techs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_netreg *netreg = opd->netreg;
	char **technologies;
	const char *path;

	if (opd->techs == techs)
		return;

	opd->techs = techs;
	technologies = network_operator_technologies(opd);
	path = network_operator_build_path(netreg, opd->mcc, opd->mnc);

	ofono_dbus_signal_array_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"Technologies", DBUS_TYPE_STRING,
					&technologies);
	g_strfreev(technologies);
}

static char *get_operator_display_name(struct ofono_netreg *netreg)
{
	struct network_operator_data *opd = netreg->current_operator;
	const char *plmn;
	const char *spn;
	static char name[1024];
	static char mccmnc[OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 1];
	int len = sizeof(name);
	int home_or_spdi;

	/*
	 * The name displayed to user depends on whether we're in a home
	 * PLMN or roaming and on configuration bits from the SIM, all
	 * together there are four cases to consider.
	 */

	if (opd == NULL) {
		g_strlcpy(name, "", len);
		return name;
	}

	plmn = opd->name;

	/*
	 * This is a fallback on some really broken hardware which do not
	 * report the COPS name
	 */
	if (plmn[0] == '\0') {
		snprintf(mccmnc, sizeof(mccmnc), "%s%s", opd->mcc, opd->mnc);
		plmn = mccmnc;
	}

	if (opd->eons_info && opd->eons_info->longname)
		plmn = opd->eons_info->longname;

	spn = ofono_sim_get_spn(netreg->sim);

	if (spn == NULL || strlen(spn) == 0) {
		g_strlcpy(name, plmn, len);
		return name;
	}

	if (netreg->status == NETWORK_REGISTRATION_STATUS_REGISTERED)
		home_or_spdi = TRUE;
	else
		home_or_spdi = sim_spdi_lookup(netreg->spdi,
							opd->mcc, opd->mnc);

	if (home_or_spdi)
		if (netreg->flags & NETWORK_REGISTRATION_FLAG_HOME_SHOW_PLMN)
			/* Case 1 */
			snprintf(name, len, "%s (%s)", spn, plmn);
		else
			/* Case 2 */
			snprintf(name, len, "%s", spn);
	else
		if (netreg->flags & NETWORK_REGISTRATION_FLAG_ROAMING_SHOW_SPN)
			/* Case 3 */
			snprintf(name, len, "%s (%s)", spn, plmn);
		else
			/* Case 4 */
			snprintf(name, len, "%s", plmn);

	return name;
}

static void netreg_emit_operator_display_name(struct ofono_netreg *netreg)
{
	const char *operator = get_operator_display_name(netreg);

	ofono_dbus_signal_property_changed(ofono_dbus_get_connection(),
					__ofono_atom_get_path(netreg->atom),
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"Name", DBUS_TYPE_STRING, &operator);
}

static void set_network_operator_name(struct network_operator_data *opd,
					const char *name)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_netreg *netreg = opd->netreg;
	const char *path;

	if (name[0] == '\0')
		return;

	if (!strncmp(opd->name, name, OFONO_MAX_OPERATOR_NAME_LENGTH))
		return;

	strncpy(opd->name, name, OFONO_MAX_OPERATOR_NAME_LENGTH);
	opd->name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

	/*
	 * If we have Enhanced Operator Name info on the SIM, we always use
	 * that, so do not need to emit the signal here
	 */
	if (opd->eons_info && opd->eons_info->longname)
		return;

	if (opd == netreg->current_operator)
		netreg_emit_operator_display_name(netreg);

	/* Don't emit when only operator name is reported */
	if (opd->mcc[0] == '\0' && opd->mnc[0] == '\0')
		return;

	path = network_operator_build_path(netreg, opd->mcc, opd->mnc);

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_OPERATOR_INTERFACE,
					"Name", DBUS_TYPE_STRING, &name);
}

static void set_network_operator_eons_info(struct network_operator_data *opd,
				const struct sim_eons_operator_info *eons_info)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_netreg *netreg = opd->netreg;
	const struct sim_eons_operator_info *old_eons_info = opd->eons_info;
	const char *path;
	const char *oldname;
	const char *newname;
	const char *oldinfo;
	const char *newinfo;

	if (old_eons_info == NULL && eons_info == NULL)
		return;

	path = network_operator_build_path(netreg, opd->mcc, opd->mnc);
	opd->eons_info = eons_info;

	if (old_eons_info && old_eons_info->longname)
		oldname = old_eons_info->longname;
	else
		oldname = opd->name;

	if (eons_info && eons_info->longname)
		newname = eons_info->longname;
	else
		newname = opd->name;

	if (oldname != newname && strcmp(oldname, newname)) {
		ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_OPERATOR_INTERFACE,
					"Name", DBUS_TYPE_STRING, &newname);

		if (opd == netreg->current_operator)
			netreg_emit_operator_display_name(netreg);
	}

	if (old_eons_info && old_eons_info->info)
		oldinfo = old_eons_info->info;
	else
		oldinfo = "";

	if (eons_info && eons_info->info)
		newinfo = eons_info->info;
	else
		newinfo = "";

	if (oldinfo != newinfo && strcmp(oldinfo, newinfo))
		ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_OPERATOR_INTERFACE,
					"AdditionalInformation",
					DBUS_TYPE_STRING, &newinfo);
}

static void append_operator_properties(struct network_operator_data *opd,
					DBusMessageIter *dict)
{
	const char *name = opd->name;
	const char *status = network_operator_status_to_string(opd->status);
	char mccmnc[OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 1];

	if (opd->eons_info && opd->eons_info->longname)
		name = opd->eons_info->longname;

	if (name[0] == '\0') {
		snprintf(mccmnc, sizeof(mccmnc), "%s%s", opd->mcc, opd->mnc);
		name = mccmnc;
	}

	ofono_dbus_dict_append(dict, "Name", DBUS_TYPE_STRING, &name);

	ofono_dbus_dict_append(dict, "Status", DBUS_TYPE_STRING, &status);

	if (*opd->mcc != '\0') {
		const char *mcc = opd->mcc;
		ofono_dbus_dict_append(dict, "MobileCountryCode",
					DBUS_TYPE_STRING, &mcc);
	}

	if (*opd->mnc != '\0') {
		const char *mnc = opd->mnc;
		ofono_dbus_dict_append(dict, "MobileNetworkCode",
					DBUS_TYPE_STRING, &mnc);
	}

	if (opd->techs != 0) {
		char **technologies = network_operator_technologies(opd);

		ofono_dbus_dict_append_array(dict, "Technologies",
						DBUS_TYPE_STRING,
						&technologies);

		g_strfreev(technologies);
	}

	if (opd->eons_info && opd->eons_info->info) {
		const char *additional = opd->eons_info->info;

		ofono_dbus_dict_append(dict, "AdditionalInformation",
					DBUS_TYPE_STRING, &additional);
	}
}

static DBusMessage *network_operator_get_properties(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct network_operator_data *opd = data;
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

	append_operator_properties(opd, &dict);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *network_operator_register(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct network_operator_data *opd = data;
	struct ofono_netreg *netreg = opd->netreg;

	if (netreg->mode == NETWORK_REGISTRATION_MODE_AUTO_ONLY)
		return __ofono_error_access_denied(msg);

	if (netreg->pending)
		return __ofono_error_busy(msg);

	if (netreg->driver->register_manual == NULL)
		return __ofono_error_not_implemented(msg);

	netreg->pending = dbus_message_ref(msg);

	netreg->driver->register_manual(netreg, opd->mcc, opd->mnc,
					register_callback, netreg);

	set_registration_mode(netreg, NETWORK_REGISTRATION_MODE_MANUAL);

	return NULL;
}

static GDBusMethodTable network_operator_methods[] = {
	{ "GetProperties",  "",  "a{sv}",  network_operator_get_properties },
	{ "Register",       "",  "",       network_operator_register,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable network_operator_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static gboolean network_operator_dbus_register(struct ofono_netreg *netreg,
					struct network_operator_data *opd)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;

	path = network_operator_build_path(netreg, opd->mcc, opd->mnc);

	if (!g_dbus_register_interface(conn, path,
					OFONO_NETWORK_OPERATOR_INTERFACE,
					network_operator_methods,
					network_operator_signals,
					NULL, opd,
					network_operator_destroy)) {
		ofono_error("Could not register NetworkOperator %s", path);
		return FALSE;
	}

	opd->netreg = netreg;
	opd->eons_info = NULL;

	if (netreg->eons)
		opd->eons_info = sim_eons_lookup(netreg->eons,
							opd->mcc, opd->mnc);

	return TRUE;
}

static gboolean network_operator_dbus_unregister(struct ofono_netreg *netreg,
					struct network_operator_data *opd)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;

	path = network_operator_build_path(netreg, opd->mcc, opd->mnc);

	return g_dbus_unregister_interface(conn, path,
					OFONO_NETWORK_OPERATOR_INTERFACE);
}

static GSList *compress_operator_list(const struct ofono_network_operator *list,
					int total)
{
	GSList *oplist = 0;
	GSList *o;
	int i;
	struct network_operator_data *opd;

	for (i = 0; i < total; i++) {
		o = NULL;

		if (list[i].mcc[0] == '\0' || list[i].mnc[0] == '\0')
			continue;

		if (oplist)
			o = g_slist_find_custom(oplist, &list[i],
						network_operator_compare);

		if (o == NULL) {
			opd = network_operator_create(&list[i]);
			oplist = g_slist_prepend(oplist, opd);
		} else if (o && list[i].tech != -1) {
			opd = o->data;
			opd->techs |= 1 << list[i].tech;
		}
	}

	if (oplist)
		oplist = g_slist_reverse(oplist);

	return oplist;
}

static gboolean update_operator_list(struct ofono_netreg *netreg, int total,
				const struct ofono_network_operator *list)
{
	GSList *n = NULL;
	GSList *o;
	GSList *compressed;
	GSList *c;
	gboolean changed = FALSE;

	compressed = compress_operator_list(list, total);

	for (c = compressed; c; c = c->next) {
		struct network_operator_data *copd = c->data;

		o = g_slist_find_custom(netreg->operator_list, copd,
					network_operator_data_compare);

		if (o) { /* Update and move to a new list */
			set_network_operator_status(o->data, copd->status);
			set_network_operator_techs(o->data, copd->techs);
			set_network_operator_name(o->data, copd->name);

			n = g_slist_prepend(n, o->data);
			netreg->operator_list =
				g_slist_remove(netreg->operator_list, o->data);
		} else {
			/* New operator */
			struct network_operator_data *opd;

			opd = g_memdup(copd,
					sizeof(struct network_operator_data));

			if (!network_operator_dbus_register(netreg, opd)) {
				g_free(opd);
				continue;
			}

			n = g_slist_prepend(n, opd);
			changed = TRUE;
		}
	}

	g_slist_foreach(compressed, (GFunc)g_free, NULL);
	g_slist_free(compressed);

	if (n)
		n = g_slist_reverse(n);

	if (netreg->operator_list)
		changed = TRUE;

	for (o = netreg->operator_list; o; o = o->next)
		network_operator_dbus_unregister(netreg, o->data);

	g_slist_free(netreg->operator_list);

	netreg->operator_list = n;

	return changed;
}

static DBusMessage *network_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_netreg *netreg = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	const char *status = registration_status_to_string(netreg->status);
	const char *operator;
	const char *mode = registration_mode_to_string(netreg->mode);

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "Status", DBUS_TYPE_STRING, &status);
	ofono_dbus_dict_append(&dict, "Mode", DBUS_TYPE_STRING, &mode);

	if (netreg->location != -1) {
		dbus_uint16_t location = netreg->location;
		ofono_dbus_dict_append(&dict, "LocationAreaCode",
					DBUS_TYPE_UINT16, &location);
	}

	if (netreg->cellid != -1) {
		dbus_uint32_t cellid = netreg->cellid;
		ofono_dbus_dict_append(&dict, "CellId",
					DBUS_TYPE_UINT32, &cellid);
	}

	if (netreg->technology != -1) {
		const char *technology =
			registration_tech_to_string(netreg->technology);

		ofono_dbus_dict_append(&dict, "Technology", DBUS_TYPE_STRING,
					&technology);
	}

	if (netreg->current_operator) {
		if (netreg->current_operator->mcc[0] != '\0') {
			const char *mcc = netreg->current_operator->mcc;
			ofono_dbus_dict_append(&dict, "MobileCountryCode",
						DBUS_TYPE_STRING, &mcc);
		}

		if (netreg->current_operator->mnc[0] != '\0') {
			const char *mnc = netreg->current_operator->mnc;
			ofono_dbus_dict_append(&dict, "MobileNetworkCode",
						DBUS_TYPE_STRING, &mnc);
		}
	}

	operator = get_operator_display_name(netreg);
	ofono_dbus_dict_append(&dict, "Name", DBUS_TYPE_STRING, &operator);

	if (netreg->signal_strength != -1) {
		unsigned char strength = netreg->signal_strength;

		ofono_dbus_dict_append(&dict, "Strength", DBUS_TYPE_BYTE,
					&strength);
	}

	if (netreg->base_station)
		ofono_dbus_dict_append(&dict, "BaseStation", DBUS_TYPE_STRING,
					&netreg->base_station);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *network_register(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_netreg *netreg = data;

	if (netreg->mode == NETWORK_REGISTRATION_MODE_AUTO_ONLY)
		return __ofono_error_access_denied(msg);

	if (netreg->pending)
		return __ofono_error_busy(msg);

	if (netreg->driver->register_auto == NULL)
		return __ofono_error_not_implemented(msg);

	netreg->pending = dbus_message_ref(msg);

	netreg->driver->register_auto(netreg, register_callback, netreg);

	set_registration_mode(netreg, NETWORK_REGISTRATION_MODE_AUTO);

	return NULL;
}

static void append_operator_struct(struct ofono_netreg *netreg,
					struct network_operator_data *opd,
					DBusMessageIter *iter)
{
	DBusMessageIter entry, dict;
	const char *path;

	path = network_operator_build_path(netreg, opd->mcc, opd->mnc);

	dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_operator_properties(opd, &dict);
	dbus_message_iter_close_container(&entry, &dict);
	dbus_message_iter_close_container(iter, &entry);
}

static void append_operator_struct_list(struct ofono_netreg *netreg,
					DBusMessageIter *array)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	char **children;
	char path[256];
	GSList *l;

	snprintf(path, sizeof(path), "%s/operator",
			__ofono_atom_get_path(netreg->atom));

	if (!dbus_connection_list_registered(conn, path, &children)) {
		DBG("Unable to obtain registered NetworkOperator(s)");
		return;
	}

	/*
	 * Quoting 27.007: "The list of operators shall be in order: home
	 * network, networks referenced in SIM or active application in the
	 * UICC (GSM or USIM) in the following order: HPLMN selector, User
	 * controlled PLMN selector, Operator controlled PLMN selector and
	 * PLMN selector (in the SIM or GSM application), and other networks."
	 * Thus we must make sure we return the list in the same order,
	 * if possible.  Luckily the operator_list is stored in order already
	 */
	for (l = netreg->operator_list; l; l = l->next) {
		struct network_operator_data *opd = l->data;
		char mnc[OFONO_MAX_MNC_LENGTH + 1];
		char mcc[OFONO_MAX_MCC_LENGTH + 1];
		int j;

		for (j = 0; children[j]; j++) {
			sscanf(children[j], "%3[0-9]%[0-9]", mcc, mnc);

			if (!strcmp(opd->mcc, mcc) && !strcmp(opd->mnc, mnc))
				append_operator_struct(netreg, opd, array);
		}
	}

	dbus_free_string_array(children);
}

static void operator_list_callback(const struct ofono_error *error, int total,
				const struct ofono_network_operator *list,
				void *data)
{
	struct ofono_netreg *netreg = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error occurred during operator list");
		__ofono_dbus_pending_reply(&netreg->pending,
					__ofono_error_failed(netreg->pending));
		return;
	}

	update_operator_list(netreg, total, list);

	reply = dbus_message_new_method_return(netreg->pending);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_OBJECT_PATH_AS_STRING
					DBUS_TYPE_ARRAY_AS_STRING
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING
					DBUS_STRUCT_END_CHAR_AS_STRING,
					&array);
	append_operator_struct_list(netreg, &array);
	dbus_message_iter_close_container(&iter, &array);

	__ofono_dbus_pending_reply(&netreg->pending, reply);
}

static DBusMessage *network_scan(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_netreg *netreg = data;

	if (netreg->mode == NETWORK_REGISTRATION_MODE_AUTO_ONLY)
		return __ofono_error_access_denied(msg);

	if (netreg->pending)
		return __ofono_error_busy(msg);

	if (netreg->driver->list_operators == NULL)
		return __ofono_error_not_implemented(msg);

	netreg->pending = dbus_message_ref(msg);

	netreg->driver->list_operators(netreg, operator_list_callback, netreg);

	return NULL;
}

static DBusMessage *network_get_operators(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_netreg *netreg = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_OBJECT_PATH_AS_STRING
					DBUS_TYPE_ARRAY_AS_STRING
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING
					DBUS_STRUCT_END_CHAR_AS_STRING,
					&array);
	append_operator_struct_list(netreg, &array);
	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static GDBusMethodTable network_registration_methods[] = {
	{ "GetProperties",  "",  "a{sv}",	network_get_properties },
	{ "Register",       "",  "",		network_register,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetOperators",   "",  "a(oa{sv})",	network_get_operators },
	{ "Scan",           "",  "a(oa{sv})",	network_scan,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable network_registration_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static void set_registration_status(struct ofono_netreg *netreg, int status)
{
	const char *str_status = registration_status_to_string(status);
	const char *path = __ofono_atom_get_path(netreg->atom);
	DBusConnection *conn = ofono_dbus_get_connection();

	netreg->status = status;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"Status", DBUS_TYPE_STRING,
					&str_status);
}

static void set_registration_location(struct ofono_netreg *netreg, int lac)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(netreg->atom);
	dbus_uint16_t dbus_lac = lac;

	if (lac > 0xffff)
		return;

	netreg->location = lac;

	if (netreg->location == -1)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"LocationAreaCode",
					DBUS_TYPE_UINT16, &dbus_lac);
}

static void set_registration_cellid(struct ofono_netreg *netreg, int ci)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(netreg->atom);
	dbus_uint32_t dbus_ci = ci;

	netreg->cellid = ci;

	if (netreg->cellid == -1)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"CellId", DBUS_TYPE_UINT32, &dbus_ci);
}

static void set_registration_technology(struct ofono_netreg *netreg, int tech)
{
	const char *tech_str = registration_tech_to_string(tech);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(netreg->atom);

	netreg->technology = tech;

	if (netreg->technology == -1)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"Technology", DBUS_TYPE_STRING,
					&tech_str);
}

void __ofono_netreg_set_base_station_name(struct ofono_netreg *netreg,
						const char *name)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(netreg->atom);
	const char *base_station = name ? name : "";

	/* Cell ID changed, but we don't have a cell name, nothing to do */
	if (netreg->base_station == NULL && name == NULL)
		return;

	if (netreg->base_station)
		g_free(netreg->base_station);

	if (name == NULL) {
		netreg->base_station = NULL;

		/*
		 * We just got unregistered, set name to NULL
		 * but don't emit signal
		 */
		if (netreg->current_operator == NULL)
			return;
	} else {
		netreg->base_station = g_strdup(name);
	}

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"BaseStation", DBUS_TYPE_STRING,
					&base_station);
}

unsigned int __ofono_netreg_add_status_watch(struct ofono_netreg *netreg,
				ofono_netreg_status_notify_cb_t notify,
				void *data, ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *item;

	DBG("%p", netreg);

	if (netreg == NULL)
		return 0;

	if (notify == NULL)
		return 0;

	item = g_new0(struct ofono_watchlist_item, 1);

	item->notify = notify;
	item->destroy = destroy;
	item->notify_data = data;

	return __ofono_watchlist_add_item(netreg->status_watches, item);
}

gboolean __ofono_netreg_remove_status_watch(struct ofono_netreg *netreg,
						unsigned int id)
{
	DBG("%p", netreg);

	return __ofono_watchlist_remove_item(netreg->status_watches, id);
}

static void notify_status_watches(struct ofono_netreg *netreg)
{
	struct ofono_watchlist_item *item;
	GSList *l;
	ofono_netreg_status_notify_cb_t notify;
	const char *mcc = NULL;
	const char *mnc = NULL;

	if (netreg->current_operator) {
		mcc = netreg->current_operator->mcc;
		mnc = netreg->current_operator->mnc;
	}

	for (l = netreg->status_watches->items; l; l = l->next) {
		item = l->data;
		notify = item->notify;

		notify(netreg->status, netreg->location, netreg->cellid,
			netreg->technology, mcc, mnc, item->notify_data);
	}
}

static void reset_available(struct network_operator_data *old,
				const struct ofono_network_operator *new)
{
	if (old == NULL)
		return;

	if (new == NULL || network_operator_compare(old, new) != 0)
		set_network_operator_status(old, OPERATOR_STATUS_AVAILABLE);
}

static void current_operator_callback(const struct ofono_error *error,
				const struct ofono_network_operator *current,
				void *data)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_netreg *netreg = data;
	const char *path = __ofono_atom_get_path(netreg->atom);
	GSList *op = NULL;

	DBG("%p, %p", netreg, netreg->current_operator);

	/*
	 * Sometimes we try to query COPS right when we roam off the cell,
	 * in which case the operator information frequently comes in bogus.
	 * We ignore it here
	 */
	if (netreg->status != NETWORK_REGISTRATION_STATUS_REGISTERED &&
			netreg->status != NETWORK_REGISTRATION_STATUS_ROAMING)
		current = NULL;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during current operator");
		return;
	}

	if (netreg->current_operator == NULL && current == NULL)
		return;

	/* We got a new network operator, reset the previous one's status */
	/* It will be updated properly later */
	reset_available(netreg->current_operator, current);

	if (current)
		op = g_slist_find_custom(netreg->operator_list, current,
					network_operator_compare);

	if (op) {
		struct network_operator_data *opd = op->data;
		unsigned int techs = opd->techs;

		if (current->tech != -1) {
			techs |= 1 << current->tech;
			set_network_operator_techs(opd, techs);
		}

		set_network_operator_status(opd, OPERATOR_STATUS_CURRENT);
		set_network_operator_name(opd, current->name);

		if (netreg->current_operator == op->data)
			return;

		netreg->current_operator = op->data;
		goto emit;
	}

	if (current) {
		struct network_operator_data *opd;

		opd = network_operator_create(current);

		if (opd->mcc[0] != '\0' && opd->mnc[0] != '\0' &&
				!network_operator_dbus_register(netreg, opd)) {
			g_free(opd);
			return;
		} else
			opd->netreg = netreg;

		netreg->current_operator = opd;
		netreg->operator_list = g_slist_append(netreg->operator_list,
							opd);
	} else {
		/* We don't free this here because operator is registered */
		/* Taken care of elsewhere */
		netreg->current_operator = NULL;
	}

emit:
	netreg_emit_operator_display_name(netreg);

	if (netreg->current_operator) {
		if (netreg->current_operator->mcc[0] != '\0') {
			const char *mcc = netreg->current_operator->mcc;
			ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"MobileCountryCode",
					DBUS_TYPE_STRING, &mcc);
		}

		if (netreg->current_operator->mnc[0] != '\0') {
			const char *mnc = netreg->current_operator->mnc;
			ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"MobileNetworkCode",
					DBUS_TYPE_STRING, &mnc);
		}
	}

	notify_status_watches(netreg);
}

static void signal_strength_callback(const struct ofono_error *error,
					int strength, void *data)
{
	struct ofono_netreg *netreg = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during signal strength query");
		return;
	}

	ofono_netreg_strength_notify(netreg, strength);
}

static void notify_emulator_status(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	switch (GPOINTER_TO_INT(data)) {
	case NETWORK_REGISTRATION_STATUS_REGISTERED:
		ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_SERVICE, 1);
		ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_ROAMING, 0);
		break;
	case NETWORK_REGISTRATION_STATUS_ROAMING:
		ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_SERVICE, 1);
		ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_ROAMING, 1);
		break;
	default:
		ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_SERVICE, 0);
		ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_ROAMING, 0);
	}
}

void ofono_netreg_status_notify(struct ofono_netreg *netreg, int status,
			int lac, int ci, int tech)
{
	if (netreg == NULL)
		return;

	DBG("%s status %d tech %d", __ofono_atom_get_path(netreg->atom),
							status, tech);

	if (netreg->status != status) {
		struct ofono_modem *modem;

		set_registration_status(netreg, status);

		modem = __ofono_atom_get_modem(netreg->atom);
		__ofono_modem_foreach_registered_atom(modem,
					OFONO_ATOM_TYPE_EMULATOR_HFP,
					notify_emulator_status,
					GINT_TO_POINTER(netreg->status));
	}

	if (netreg->location != lac)
		set_registration_location(netreg, lac);

	if (netreg->cellid != ci)
		set_registration_cellid(netreg, ci);

	if (netreg->technology != tech)
		set_registration_technology(netreg, tech);

	if (netreg->status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
		netreg->status == NETWORK_REGISTRATION_STATUS_ROAMING) {
		if (netreg->driver->current_operator != NULL)
			netreg->driver->current_operator(netreg,
					current_operator_callback, netreg);

		if (netreg->driver->strength != NULL)
			netreg->driver->strength(netreg,
					signal_strength_callback, netreg);
	} else {
		struct ofono_error error;

		error.type = OFONO_ERROR_TYPE_NO_ERROR;
		error.error = 0;

		current_operator_callback(&error, NULL, netreg);
		__ofono_netreg_set_base_station_name(netreg, NULL);

		netreg->signal_strength = -1;
	}

	notify_status_watches(netreg);
}

void ofono_netreg_time_notify(struct ofono_netreg *netreg,
				struct ofono_network_time *info)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(netreg->atom);

	if (info == NULL)
		return;

	__ofono_nettime_info_received(modem, info);
}

static void sim_csp_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *user_data)
{
	struct ofono_netreg *netreg = user_data;
	int i = 0;

	if (!ok)
		return;

	if (total_length < 18)
		return;

	/*
	 * According to CPHS 4.2, EFcsp is an array of two-byte service
	 * entries, each consisting of a one byte service group
	 * identifier followed by 8 bits; each bit is indicating
	 * availability of a specific service or feature.
	 *
	 * The PLMN mode bit, if present, indicates whether manual
	 * operator selection should be disabled or enabled. When
	 * unset, the device is forced to automatic mode; when set,
	 * manual selection is to be enabled. The latter is also the
	 * default.
	 */
	while (i < total_length &&
			data[i] != SIM_CSP_ENTRY_VALUE_ADDED_SERVICES)
		i += 2;

	if (i == total_length)
		return;

	if ((data[i + 1] & 0x80) != 0) {
		if (netreg->mode == NETWORK_REGISTRATION_MODE_AUTO_ONLY)
			set_registration_mode(netreg,
						NETWORK_REGISTRATION_MODE_AUTO);

		return;
	}

	set_registration_mode(netreg, NETWORK_REGISTRATION_MODE_AUTO_ONLY);
}

static void sim_csp_changed(int id, void *userdata)
{
	struct ofono_netreg *netreg = userdata;

	ofono_sim_read(netreg->sim_context, SIM_EF_CPHS_CSP_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_csp_read_cb, netreg);
}

static void init_registration_status(const struct ofono_error *error,
					int status, int lac, int ci, int tech,
					void *data)
{
	struct ofono_netreg *netreg = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during registration status query");
		return;
	}

	ofono_netreg_status_notify(netreg, status, lac, ci, tech);

	/*
	 * Bootstrap our signal strength value without waiting for the
	 * stack to report it
	 */
	if (netreg->status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
		netreg->status == NETWORK_REGISTRATION_STATUS_ROAMING) {
		if (netreg->driver->strength != NULL)
			netreg->driver->strength(netreg,
					signal_strength_callback, netreg);
	}

	if (netreg->mode != NETWORK_REGISTRATION_MODE_MANUAL &&
		(status == NETWORK_REGISTRATION_STATUS_NOT_REGISTERED ||
			status == NETWORK_REGISTRATION_STATUS_DENIED ||
			status == NETWORK_REGISTRATION_STATUS_UNKNOWN)) {
		if (netreg->driver->register_auto != NULL)
			netreg->driver->register_auto(netreg, init_register,
							netreg);
	}

	if (netreg->driver->register_manual == NULL) {
		set_registration_mode(netreg,
					NETWORK_REGISTRATION_MODE_AUTO_ONLY);
		return;
	}

	if (netreg->sim_context) {
		ofono_sim_read(netreg->sim_context, SIM_EF_CPHS_CSP_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				sim_csp_read_cb, netreg);

		ofono_sim_add_file_watch(netreg->sim_context,
						SIM_EF_CPHS_CSP_FILEID,
						sim_csp_changed, netreg, NULL);
	}
}

static void notify_emulator_strength(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	int val = 0;

	if (GPOINTER_TO_INT(data) > 0)
		val = (GPOINTER_TO_INT(data) - 1) / 20 + 1;

	ofono_emulator_set_indicator(em, OFONO_EMULATOR_IND_SIGNAL, val);
}

void ofono_netreg_strength_notify(struct ofono_netreg *netreg, int strength)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem;

	if (netreg->signal_strength == strength)
		return;

	/*
	 * Theoretically we can get signal strength even when not registered
	 * to any network.  However, what do we do with it in that case?
	 */
	if (netreg->status != NETWORK_REGISTRATION_STATUS_REGISTERED &&
			netreg->status != NETWORK_REGISTRATION_STATUS_ROAMING)
		return;

	DBG("strength %d", strength);

	netreg->signal_strength = strength;

	if (strength != -1) {
		const char *path = __ofono_atom_get_path(netreg->atom);
		unsigned char strength = netreg->signal_strength;

		ofono_dbus_signal_property_changed(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					"Strength", DBUS_TYPE_BYTE,
					&strength);
	}

	modem = __ofono_atom_get_modem(netreg->atom);
	__ofono_modem_foreach_registered_atom(modem,
				OFONO_ATOM_TYPE_EMULATOR_HFP,
				notify_emulator_strength,
				GINT_TO_POINTER(netreg->signal_strength));
}

static void sim_opl_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *user_data)
{
	struct ofono_netreg *netreg = user_data;
	int total;
	GSList *l;

	if (!ok) {
		if (record > 0)
			goto optimize;

		return;
	}

	if (record_length < 8 || length < record_length)
		return;

	total = length / record_length;

	sim_eons_add_opl_record(netreg->eons, data, record_length);

	if (record != total)
		return;

optimize:
	sim_eons_optimize(netreg->eons);

	for (l = netreg->operator_list; l; l = l->next) {
		struct network_operator_data *opd = l->data;
		const struct sim_eons_operator_info *eons_info;

		eons_info = sim_eons_lookup(netreg->eons, opd->mcc, opd->mnc);

		set_network_operator_eons_info(opd, eons_info);
	}
}

static void sim_pnn_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *user_data)
{
	struct ofono_netreg *netreg = user_data;
	int total;

	if (!ok)
		goto check;

	if (length < 3 || record_length < 3 || length < record_length)
		goto check;

	total = length / record_length;

	if (netreg->eons == NULL)
		netreg->eons = sim_eons_new(total);

	sim_eons_add_pnn_record(netreg->eons, record, data, record_length);

	if (record != total)
		return;

check:
	netreg->flags &= ~NETWORK_REGISTRATION_FLAG_READING_PNN;

	/*
	 * If PNN is not present then OPL is not useful, don't
	 * retrieve it.  If OPL is not there then PNN[1] will
	 * still be used for the HPLMN and/or EHPLMN, if PNN
	 * is present.
	 */
	if (netreg->eons && !sim_eons_pnn_is_empty(netreg->eons))
		ofono_sim_read(netreg->sim_context, SIM_EFOPL_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				sim_opl_read_cb, netreg);
}

static void sim_spdi_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *user_data)
{
	struct ofono_netreg *netreg = user_data;

	if (!ok)
		return;

	netreg->spdi = sim_spdi_new(data, length);

	if (netreg->current_operator == NULL)
		return;

	if (netreg->status != NETWORK_REGISTRATION_STATUS_ROAMING)
		return;

	if (!sim_spdi_lookup(netreg->spdi, netreg->current_operator->mcc,
				netreg->current_operator->mnc))
		return;

	netreg_emit_operator_display_name(netreg);
}

static void sim_spn_display_condition_parse(struct ofono_netreg *netreg,
						guint8 dcbyte)
{
	if (dcbyte & SIM_EFSPN_DC_HOME_PLMN_BIT)
		netreg->flags |= NETWORK_REGISTRATION_FLAG_HOME_SHOW_PLMN;

	if (!(dcbyte & SIM_EFSPN_DC_ROAMING_SPN_BIT))
		netreg->flags |= NETWORK_REGISTRATION_FLAG_ROAMING_SHOW_SPN;
}

static void spn_read_cb(const char *spn, const char *dc, void *data)
{
	struct ofono_netreg *netreg = data;

	netreg->flags &= ~(NETWORK_REGISTRATION_FLAG_HOME_SHOW_PLMN |
				NETWORK_REGISTRATION_FLAG_ROAMING_SHOW_SPN);

	if (dc)
		sim_spn_display_condition_parse(netreg, *dc);

	if (netreg->current_operator)
		netreg_emit_operator_display_name(netreg);
}

int ofono_netreg_get_location(struct ofono_netreg *netreg)
{
	if (netreg == NULL)
		return -1;

	return netreg->location;
}

int ofono_netreg_get_cellid(struct ofono_netreg *netreg)
{
	if (netreg == NULL)
		return -1;

	return netreg->cellid;
}

int ofono_netreg_get_status(struct ofono_netreg *netreg)
{
	if (netreg == NULL)
		return -1;

	return netreg->status;
}

int ofono_netreg_get_technology(struct ofono_netreg *netreg)
{
	if (netreg == NULL)
		return -1;

	return netreg->technology;
}

const char *ofono_netreg_get_mcc(struct ofono_netreg *netreg)
{
	if (netreg == NULL)
		return NULL;

	if (netreg->current_operator == NULL)
		return NULL;

	return netreg->current_operator->mcc;
}

const char *ofono_netreg_get_mnc(struct ofono_netreg *netreg)
{
	if (netreg == NULL)
		return NULL;

	if (netreg->current_operator == NULL)
		return NULL;

	return netreg->current_operator->mnc;
}

int ofono_netreg_driver_register(const struct ofono_netreg_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_netreg_driver_unregister(const struct ofono_netreg_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void emulator_remove_handler(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	ofono_emulator_remove_handler(em, data);
}

static void netreg_unregister(struct ofono_atom *atom)
{
	struct ofono_netreg *netreg = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);
	GSList *l;

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						notify_emulator_status,
						GINT_TO_POINTER(0));
	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						notify_emulator_strength,
						GINT_TO_POINTER(0));

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"+COPS");

	__ofono_modem_remove_atom_watch(modem, netreg->hfp_watch);

	__ofono_watchlist_free(netreg->status_watches);
	netreg->status_watches = NULL;

	for (l = netreg->operator_list; l; l = l->next) {
		struct network_operator_data *opd = l->data;

		if (opd->mcc[0] == '\0' && opd->mnc[0] == '\0') {
			g_free(opd);
			continue;
		}

		network_operator_dbus_unregister(netreg, l->data);
	}

	g_slist_free(netreg->operator_list);
	netreg->operator_list = NULL;

	if (netreg->base_station) {
		g_free(netreg->base_station);
		netreg->base_station = NULL;
	}

	if (netreg->settings) {
		storage_close(netreg->imsi, SETTINGS_STORE,
				netreg->settings, TRUE);

		g_free(netreg->imsi);
		netreg->imsi = NULL;
		netreg->settings = NULL;
	}

	if (netreg->spn_watch)
		ofono_sim_remove_spn_watch(netreg->sim, &netreg->spn_watch);

	if (netreg->sim_context) {
		ofono_sim_context_free(netreg->sim_context);
		netreg->sim_context = NULL;
	}

	netreg->sim = NULL;

	g_dbus_unregister_interface(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE);
	ofono_modem_remove_interface(modem,
					OFONO_NETWORK_REGISTRATION_INTERFACE);
}

static void netreg_remove(struct ofono_atom *atom)
{
	struct ofono_netreg *netreg = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (netreg == NULL)
		return;

	if (netreg->driver != NULL && netreg->driver->remove != NULL)
		netreg->driver->remove(netreg);

	sim_eons_free(netreg->eons);
	sim_spdi_free(netreg->spdi);

	g_free(netreg);
}

struct ofono_netreg *ofono_netreg_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_netreg *netreg;
	GSList *l;

	if (driver == NULL)
		return NULL;

	netreg = g_try_new0(struct ofono_netreg, 1);

	if (netreg == NULL)
		return NULL;

	netreg->status = NETWORK_REGISTRATION_STATUS_UNKNOWN;
	netreg->location = -1;
	netreg->cellid = -1;
	netreg->technology = -1;
	netreg->signal_strength = -1;

	netreg->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_NETREG,
						netreg_remove, netreg);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_netreg_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(netreg, vendor, data) < 0)
			continue;

		netreg->driver = drv;
		break;
	}

	return netreg;
}

static void netreg_load_settings(struct ofono_netreg *netreg)
{
	const char *imsi;
	char *strmode;
	gboolean upgrade = FALSE;

	if (netreg->mode == NETWORK_REGISTRATION_MODE_AUTO_ONLY)
		return;

	imsi = ofono_sim_get_imsi(netreg->sim);
	if (imsi == NULL)
		return;

	netreg->settings = storage_open(imsi, SETTINGS_STORE);

	if (netreg->settings == NULL)
		return;

	netreg->imsi = g_strdup(imsi);

	strmode = g_key_file_get_string(netreg->settings, SETTINGS_GROUP,
					"Mode", NULL);

	if (strmode == NULL)
		upgrade = TRUE;
	else if (g_str_equal(strmode, "auto"))
		netreg->mode = NETWORK_REGISTRATION_MODE_AUTO;
	else if (g_str_equal(strmode, "manual"))
		netreg->mode = NETWORK_REGISTRATION_MODE_MANUAL;
	else {
		int mode;

		mode = g_key_file_get_integer(netreg->settings, SETTINGS_GROUP,
						"Mode", NULL);

		switch (mode) {
		case NETWORK_REGISTRATION_MODE_AUTO:
		case NETWORK_REGISTRATION_MODE_MANUAL:
			netreg->mode = mode;
			break;
		}

		upgrade = TRUE;
	}

	g_free(strmode);

	if (upgrade == FALSE)
		return;

	if (netreg->mode == NETWORK_REGISTRATION_MODE_MANUAL)
		strmode = "manual";
	else
		strmode = "auto";

	g_key_file_set_string(netreg->settings, SETTINGS_GROUP,
				"Mode", strmode);
}

static void sim_pnn_opl_changed(int id, void *userdata)
{
	struct ofono_netreg *netreg = userdata;
	GSList *l;

	if (netreg->flags & NETWORK_REGISTRATION_FLAG_READING_PNN)
		return;
	/*
	 * Free references to structures on the netreg->eons list and
	 * update the operator info on D-bus.  If EFpnn/EFopl read succeeds,
	 * operator info will be updated again, otherwise it won't be
	 * updated again.
	 */
	for (l = netreg->operator_list; l; l = l->next)
		set_network_operator_eons_info(l->data, NULL);

	sim_eons_free(netreg->eons);
	netreg->eons = NULL;

	netreg->flags |= NETWORK_REGISTRATION_FLAG_READING_PNN;
	ofono_sim_read(netreg->sim_context, SIM_EFPNN_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED,
			sim_pnn_read_cb, netreg);
}

static void sim_spdi_changed(int id, void *userdata)
{
	struct ofono_netreg *netreg = userdata;

	sim_spdi_free(netreg->spdi);
	netreg->spdi = NULL;

	if (netreg->current_operator &&
			netreg->status == NETWORK_REGISTRATION_STATUS_ROAMING)
		netreg_emit_operator_display_name(netreg);

	ofono_sim_read(netreg->sim_context, SIM_EFSPDI_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_spdi_read_cb, netreg);
}

static void emulator_cops_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_netreg *netreg = userdata;
	struct ofono_error result;
	int val;
	char name[17];
	char buf[32];

	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_SET:
		ofono_emulator_request_next_number(req, &val);
		if (val != 3)
			goto fail;

		ofono_emulator_request_next_number(req, &val);
		if (val != 0)
			goto fail;

		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		ofono_emulator_send_final(em, &result);
		break;

	case OFONO_EMULATOR_REQUEST_TYPE_QUERY:
		strncpy(name, get_operator_display_name(netreg), 16);
		name[16] = '\0';
		sprintf(buf, "+COPS: %d,0,\"%s\"", netreg->mode, name);
		ofono_emulator_send_info(em, buf, TRUE);
		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		ofono_emulator_send_final(em, &result);
		break;

	default:
fail:
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
	};
}

static void emulator_hfp_init(struct ofono_atom *atom, void *data)
{
	struct ofono_netreg *netreg = data;
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	notify_emulator_status(atom, GINT_TO_POINTER(netreg->status));
	notify_emulator_strength(atom,
				GINT_TO_POINTER(netreg->signal_strength));

	ofono_emulator_add_handler(em, "+COPS", emulator_cops_cb, data, NULL);
}

static void emulator_hfp_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED)
		emulator_hfp_init(atom, data);
}

void ofono_netreg_register(struct ofono_netreg *netreg)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(netreg->atom);
	const char *path = __ofono_atom_get_path(netreg->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_NETWORK_REGISTRATION_INTERFACE,
					network_registration_methods,
					network_registration_signals,
					NULL, netreg, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_NETWORK_REGISTRATION_INTERFACE);

		return;
	}

	netreg->status_watches = __ofono_watchlist_new(g_free);

	ofono_modem_add_interface(modem, OFONO_NETWORK_REGISTRATION_INTERFACE);

	if (netreg->driver->registration_status != NULL)
		netreg->driver->registration_status(netreg,
					init_registration_status, netreg);

	netreg->sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);
	if (netreg->sim != NULL) {
		/* Assume that if sim atom exists, it is ready */
		netreg->sim_context = ofono_sim_context_create(netreg->sim);

		netreg_load_settings(netreg);

		netreg->flags |= NETWORK_REGISTRATION_FLAG_READING_PNN;
		ofono_sim_read(netreg->sim_context, SIM_EFPNN_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				sim_pnn_read_cb, netreg);
		ofono_sim_add_file_watch(netreg->sim_context, SIM_EFPNN_FILEID,
						sim_pnn_opl_changed, netreg,
						NULL);
		ofono_sim_add_file_watch(netreg->sim_context, SIM_EFOPL_FILEID,
						sim_pnn_opl_changed, netreg,
						NULL);

		ofono_sim_add_spn_watch(netreg->sim, &netreg->spn_watch,
						spn_read_cb, netreg, NULL);

		if (__ofono_sim_service_available(netreg->sim,
				SIM_UST_SERVICE_PROVIDER_DISPLAY_INFO,
				SIM_SST_SERVICE_PROVIDER_DISPLAY_INFO)) {
			ofono_sim_read(netreg->sim_context, SIM_EFSPDI_FILEID,
					OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
					sim_spdi_read_cb, netreg);

			ofono_sim_add_file_watch(netreg->sim_context,
							SIM_EFSPDI_FILEID,
							sim_spdi_changed,
							netreg, NULL);
		}
	}

	__ofono_atom_register(netreg->atom, netreg_unregister);

	netreg->hfp_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_EMULATOR_HFP,
					emulator_hfp_watch, netreg, NULL);
}

void ofono_netreg_remove(struct ofono_netreg *netreg)
{
	__ofono_atom_free(netreg->atom);
}

void ofono_netreg_set_data(struct ofono_netreg *netreg, void *data)
{
	netreg->driver_data = data;
}

void *ofono_netreg_get_data(struct ofono_netreg *netreg)
{
	return netreg->driver_data;
}
