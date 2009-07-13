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

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "dbus-gsm.h"
#include "modem.h"
#include "driver.h"
#include "common.h"
#include "sim.h"
#include "simutil.h"
#include "util.h"

#define NETWORK_REGISTRATION_INTERFACE "org.ofono.NetworkRegistration"
#define NETWORK_OPERATOR_INTERFACE "org.ofono.NetworkOperator"

#define NETWORK_REGISTRATION_FLAG_REQUESTING_OPLIST 0x1
#define NETWORK_REGISTRATION_FLAG_PENDING 0x2
#define NETWORK_REGISTRATION_FLAG_HOME_SHOW_PLMN 0x4
#define NETWORK_REGISTRATION_FLAG_ROAMING_SHOW_SPN 0x8

#define AUTO_REGISTER 1

/* How often we update the operator list, in seconds */
#define OPERATOR_LIST_UPDATE_TIME 300

struct network_registration_data {
	int status;
	int location;
	int cellid;
	int technology;
	struct ofono_network_operator *current_operator;
	GSList *operator_list;
	struct ofono_network_registration_ops *ops;
	int flags;
	DBusMessage *pending;
	int signal_strength;
	char *spname;
	struct sim_spdi *spdi;
	struct sim_eons *eons;
};

static void operator_list_callback(const struct ofono_error *error, int total,
				const struct ofono_network_operator *list,
				void *data);

static void current_operator_callback(const struct ofono_error *error,
				const struct ofono_network_operator *current,
				void *data);

static void signal_strength_callback(const struct ofono_error *error,
					int strength, void *data);

static void registration_status_callback(const struct ofono_error *error,
					int status, int lac, int ci, int tech,
					void *data);

struct ofono_network_operator_data {
	struct ofono_network_operator *operator;
	struct ofono_modem *modem;
};

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

static inline const char *registration_status_to_string(int status)
{
	switch (status) {
	case NETWORK_REGISTRATION_STATUS_NOT_REGISTERED:
		return "unregistered";
	case NETWORK_REGISTRATION_STATUS_REGISTERED:
		return "registered";
	case NETWORK_REGISTRATION_STATUS_SEARCHING:
		return "searching";
	case NETWORK_REGISTRATION_STATUS_DENIED:
		return "denied";
	case NETWORK_REGISTRATION_STATUS_UNKNOWN:
		return "unknown";
	case NETWORK_REGISTRATION_STATUS_ROAMING:
		return "roaming";
	}

	return "";
}

static inline const char *registration_tech_to_string(int tech)
{
	switch (tech) {
	case ACCESS_TECHNOLOGY_GSM:
		return "GSM";
	case ACCESS_TECHNOLOGY_GSM_COMPACT:
		return "GSMCompact";
	case ACCESS_TECHNOLOGY_UTRAN:
		return "UTRAN";
	case ACCESS_TECHNOLOGY_GSM_EGPRS:
		return "GSM+EGPS";
	case ACCESS_TECHNOLOGY_UTRAN_HSDPA:
		return "UTRAN+HSDPA";
	case ACCESS_TECHNOLOGY_UTRAN_HSUPA:
		return "UTRAN+HSUPA";
	case ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA:
		return "UTRAN+HSDPA+HSUPA";
	case ACCESS_TECHNOLOGY_EUTRAN:
		return "EUTRAN";
	default:
		return "";
	}
}

static void register_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct network_registration_data *netreg = modem->network_registration;
	DBusConnection *conn = dbus_gsm_connection();
	DBusMessage *reply;

	if (!netreg->pending)
		goto out;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(netreg->pending);
	else
		reply = dbus_gsm_failed(netreg->pending);

	g_dbus_send_message(conn, reply);

	dbus_message_unref(netreg->pending);
	netreg->pending = NULL;

out:
	netreg->flags &= ~NETWORK_REGISTRATION_FLAG_PENDING;

	if (netreg->ops->registration_status)
		netreg->ops->registration_status(modem,
					registration_status_callback, modem);
}

/* Must use dbus_gsm_free_string_array on network_operators */
static void network_operator_populate_registered(struct ofono_modem *modem,
						char ***network_operators)
{
	DBusConnection *conn = dbus_gsm_connection();
	char **children;
	int i;
	int prefix_len;
	int num_children;
	GSList *l;
	char path[MAX_DBUS_PATH_LEN];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	int op_path_len;

	prefix_len = snprintf(path, MAX_DBUS_PATH_LEN, "%s/operator",
				modem->path);

	if (!dbus_connection_list_registered(conn, path, &children)) {
		ofono_debug("Unable to obtain registered NetworkOperator(s)");
		*network_operators = g_try_new0(char *, 1);
		return;
	}

	for (i = 0; children[i]; i++)
		;

	num_children = i;

	*network_operators = g_try_new0(char *, num_children + 1);
	
	/* Enough to store '/' + MCC + MNC + null */
	op_path_len = prefix_len;
	op_path_len += OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 2;

	/* Quoting 27.007: "The list of operators shall be in order: home
	 * network, networks referenced in SIM or active application in the
	 * UICC (GSM or USIM) in the following order: HPLMN selector, User
	 * controlled PLMN selector, Operator controlled PLMN selector and
	 * PLMN selector (in the SIM or GSM application), and other networks."
	 * Thus we must make sure we return the list in the same order,
	 * if possible.  Luckily the operator_list is stored in order already
	 */
	i = 0;
	for (l = modem->network_registration->operator_list; l; l = l->next) {
		struct ofono_network_operator *op = l->data;
		int j;

		for (j = 0; children[j]; j++) {
			sscanf(children[j], "%3[0-9]%[0-9]", mcc, mnc);
			if (!strcmp(op->mcc, mcc) && !strcmp(op->mnc, mnc)) {
				(*network_operators)[i] =
					g_try_new(char, op_path_len);
				snprintf((*network_operators)[i], op_path_len,
						"%s/%s", path, children[j]);
				++i;
			}
		}
	}

	dbus_free_string_array(children);
}

static void network_operator_destroy(gpointer userdata)
{
	struct ofono_network_operator_data *op = userdata;

	g_free(op);
}

static gint network_operator_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_network_operator *opa = a;
	const struct ofono_network_operator *opb = b;

	int comp1;
	int comp2;

	comp1 = strcmp(opa->mcc, opb->mcc);
	comp2 = strcmp(opa->mnc, opb->mnc);

	return comp1 != 0 ? comp1 : comp2;
}

static inline const char *network_operator_build_path(struct ofono_modem *modem,
							struct ofono_network_operator *oper)
{
	static char path[MAX_DBUS_PATH_LEN];

	snprintf(path, MAX_DBUS_PATH_LEN, "%s/operator/%s%s",
			modem->path, oper->mcc, oper->mnc);

	return path;
}

static void network_operator_emit_available_operators(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();
	char **network_operators;

	network_operator_populate_registered(modem, &network_operators);

	dbus_gsm_signal_array_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"AvailableOperators",
						DBUS_TYPE_OBJECT_PATH,
						&network_operators);

	dbus_gsm_free_string_array(network_operators);
}

static void set_network_operator_status(struct ofono_modem *modem,
					struct ofono_network_operator *op,
					int status)
{
	DBusConnection *conn = dbus_gsm_connection();
	const char *status_str;
	const char *path;

	if (op->status == status)
		return;

	op->status = status;

	status_str = network_operator_status_to_string(status);
	path = network_operator_build_path(modem, op);

	dbus_gsm_signal_property_changed(conn, path, NETWORK_OPERATOR_INTERFACE,
						"Status", DBUS_TYPE_STRING,
						&status_str);
}

static void set_network_operator_technology(struct ofono_modem *modem,
						struct ofono_network_operator *op,
						int tech)
{
	DBusConnection *conn = dbus_gsm_connection();
	const char *tech_str;
	const char *path;

	if (op->tech == tech)
		return;

	op->tech = tech;
	tech_str = registration_tech_to_string(tech);
	path = network_operator_build_path(modem, op);

	dbus_gsm_signal_property_changed(conn, path, NETWORK_OPERATOR_INTERFACE,
						"Technology", DBUS_TYPE_STRING,
						&tech_str);
}

static char *get_operator_display_name(struct ofono_modem *modem)
{
	struct network_registration_data *netreg = modem->network_registration;
	const char *plmn;
	static char name[1024];
	int len = sizeof(name);
	int home_or_spdi;

	/* The name displayed to user depends on whether we're in a home
	 * PLMN or roaming and on configuration bits from the SIM, all
	 * together there are four cases to consider.  */

	if (!netreg->current_operator) {
		g_strlcpy(name, "", len);
		return name;
	}

	plmn = netreg->current_operator->name;
	if (netreg->current_operator->override_name)
		plmn = netreg->current_operator->override_name;

	if (!netreg->spname || strlen(netreg->spname) == 0) {
		g_strlcpy(name, plmn, len);
		return name;
	}

	if (netreg->status == NETWORK_REGISTRATION_STATUS_REGISTERED)
		home_or_spdi = TRUE;
	else
		home_or_spdi = sim_spdi_lookup(netreg->spdi,
						netreg->current_operator->mcc,
						netreg->current_operator->mnc);

	if (home_or_spdi)
		if (netreg->flags & NETWORK_REGISTRATION_FLAG_HOME_SHOW_PLMN)
			/* Case 1 */
			snprintf(name, len, "%s (%s)", netreg->spname, plmn);
		else
			/* Case 2 */
			snprintf(name, len, "%s", netreg->spname);
	else
		if (netreg->flags & NETWORK_REGISTRATION_FLAG_ROAMING_SHOW_SPN)
			/* Case 3 */
			snprintf(name, len, "%s (%s)", netreg->spname, plmn);
		else
			/* Case 4 */
			snprintf(name, len, "%s", plmn);

	return name;
}

static void set_network_operator_name(struct ofono_modem *modem,
					struct ofono_network_operator *op,
					const char *name)
{
	struct network_registration_data *netreg = modem->network_registration;
	DBusConnection *conn = dbus_gsm_connection();
	const char *path;
	const char *operator;

	if (!strncmp(op->name, name, OFONO_MAX_OPERATOR_NAME_LENGTH))
		return;

	strncpy(op->name, name, OFONO_MAX_OPERATOR_NAME_LENGTH);
	op->name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

	if (!op->override_name) {
		path = network_operator_build_path(modem, op);

		dbus_gsm_signal_property_changed(conn, path,
					NETWORK_OPERATOR_INTERFACE,
					"Name", DBUS_TYPE_STRING,
					&name);

		if (op == netreg->current_operator) {
			operator = get_operator_display_name(modem);

			dbus_gsm_signal_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"Operator", DBUS_TYPE_STRING,
						&operator);
		}
	}
}

static DBusMessage *network_operator_get_properties(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct ofono_network_operator_data *op = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	const char *name = op->operator->name;
	const char *status =
		network_operator_status_to_string(op->operator->status);

	if (op->operator->override_name)
		name = op->operator->override_name;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	dbus_gsm_dict_append(&dict, "Name", DBUS_TYPE_STRING, &name);

	dbus_gsm_dict_append(&dict, "Status", DBUS_TYPE_STRING, &status);

	if (*op->operator->mcc != '\0') {
		const char *mcc = op->operator->mcc;
		dbus_gsm_dict_append(&dict, "MobileCountryCode",
					DBUS_TYPE_STRING, &mcc);
	}

	if (*op->operator->mnc != '\0') {
		const char *mnc = op->operator->mnc;
		dbus_gsm_dict_append(&dict, "MobileNetworkCode",
					DBUS_TYPE_STRING, &mnc);
	}

	if (op->operator->tech != -1) {
		const char *technology =
			registration_tech_to_string(op->operator->tech);

		dbus_gsm_dict_append(&dict, "Technology", DBUS_TYPE_STRING,
					&technology);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *network_operator_register(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_network_operator_data *op = data;
	struct network_registration_data *netreg = op->modem->network_registration;

	if (netreg->flags & NETWORK_REGISTRATION_FLAG_PENDING)
		return dbus_gsm_busy(msg);

	if (netreg->ops->register_manual == NULL)
		return dbus_gsm_not_implemented(msg);

	netreg->flags |= NETWORK_REGISTRATION_FLAG_PENDING;
	netreg->pending = dbus_message_ref(msg);

	netreg->ops->register_manual(op->modem, op->operator,
					register_callback, op->modem);

	return NULL;
}

static GDBusMethodTable network_operator_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	network_operator_get_properties },
	{ "Register",		"",	"",		network_operator_register,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable network_operator_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static gboolean network_operator_dbus_register(struct ofono_modem *modem,
						struct ofono_network_operator *op)
{
	DBusConnection *conn = dbus_gsm_connection();
	const char *path;

	struct ofono_network_operator_data *opd =
		g_try_new(struct ofono_network_operator_data, 1);

	if (!opd)
		return FALSE;

	opd->operator = op;
	opd->modem = modem;

	path = network_operator_build_path(modem, op);

	if (!g_dbus_register_interface(conn, path, NETWORK_OPERATOR_INTERFACE,
					network_operator_methods,
					network_operator_signals,
					NULL, opd,
					network_operator_destroy)) {
		ofono_error("Could not register NetworkOperator %s", path);
		network_operator_destroy(opd);

		return FALSE;
	}

	return TRUE;
}

static gboolean network_operator_dbus_unregister(struct ofono_modem *modem,
						struct ofono_network_operator *op)
{
	DBusConnection *conn = dbus_gsm_connection();
	const char *path = network_operator_build_path(modem, op);

	return g_dbus_unregister_interface(conn, path,
					NETWORK_OPERATOR_INTERFACE);
}

static struct network_registration_data *network_registration_create()
{
	struct network_registration_data *data;

	data = g_try_new0(struct network_registration_data, 1);
	if (data == NULL)
		return data;

	data->status = NETWORK_REGISTRATION_STATUS_UNKNOWN;
	data->location = -1;
	data->cellid = -1;
	data->technology = -1;
	data->signal_strength = -1;

	return data;
}

static void network_registration_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct network_registration_data *data = modem->network_registration;
	GSList *l;

	for (l = data->operator_list; l; l = l->next) {
		network_operator_dbus_unregister(modem, l->data);
		g_free(l->data);
	}

	g_slist_free(data->operator_list);

	if (data->eons)
		sim_eons_free(data->eons);

	if (data->spdi)
		sim_spdi_free(data->spdi);

	if (data->spname)
		g_free(data->spname);

	g_free(data);

	modem->network_registration = 0;
}

static DBusMessage *network_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct network_registration_data *netreg = modem->network_registration;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	const char *status = registration_status_to_string(netreg->status);
	const char *operator;

	char **network_operators;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	dbus_gsm_dict_append(&dict, "Status", DBUS_TYPE_STRING, &status);

	if (netreg->location != -1) {
		dbus_uint16_t location = netreg->location;
		dbus_gsm_dict_append(&dict, "LocationAreaCode",
					DBUS_TYPE_UINT16, &location);
	}

	if (netreg->cellid != -1) {
		dbus_uint32_t cellid = netreg->cellid;
		dbus_gsm_dict_append(&dict, "CellId",
					DBUS_TYPE_UINT32, &cellid);
	}

	if (netreg->technology != -1) {
		const char *technology =
			registration_tech_to_string(netreg->technology);

		dbus_gsm_dict_append(&dict, "Technology", DBUS_TYPE_STRING,
					&technology);
	}

	operator = get_operator_display_name(modem);
	dbus_gsm_dict_append(&dict, "Operator", DBUS_TYPE_STRING, &operator);

	network_operator_populate_registered(modem, &network_operators);

	dbus_gsm_dict_append_array(&dict, "AvailableOperators",
					DBUS_TYPE_OBJECT_PATH,
					&network_operators);

	dbus_gsm_free_string_array(network_operators);

	if (netreg->signal_strength != -1) {
		dbus_uint16_t strength = netreg->signal_strength;
		dbus_gsm_dict_append(&dict, "Strength", DBUS_TYPE_UINT16,
					&strength);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *network_register(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct network_registration_data *netreg = modem->network_registration;

	if (netreg->flags & NETWORK_REGISTRATION_FLAG_PENDING)
		return dbus_gsm_busy(msg);

	if (netreg->ops->register_auto == NULL)
		return dbus_gsm_not_implemented(msg);

	netreg->flags |= NETWORK_REGISTRATION_FLAG_PENDING;
	netreg->pending = dbus_message_ref(msg);

	netreg->ops->register_auto(modem, register_callback, modem);

	return NULL;
}

static DBusMessage *network_deregister(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct network_registration_data *netreg = modem->network_registration;

	if (netreg->flags & NETWORK_REGISTRATION_FLAG_PENDING)
		return dbus_gsm_busy(msg);

	if (netreg->ops->deregister == NULL)
		return dbus_gsm_not_implemented(msg);

	netreg->flags |= NETWORK_REGISTRATION_FLAG_PENDING;
	netreg->pending = dbus_message_ref(msg);

	netreg->ops->deregister(modem, register_callback, modem);

	return NULL;
}

static GDBusMethodTable network_registration_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	network_get_properties	},
	{ "Register",		"",	"",		network_register,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "Deregister",		"",	"",		network_deregister,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable network_registration_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static void update_network_operator_list(struct ofono_modem *modem)
{
	struct network_registration_data *netreg = modem->network_registration;

	if (netreg->flags & NETWORK_REGISTRATION_FLAG_REQUESTING_OPLIST)
		return;

	if (!netreg->ops->list_operators)
		return;

	netreg->flags |= NETWORK_REGISTRATION_FLAG_REQUESTING_OPLIST;
	netreg->ops->list_operators(modem, operator_list_callback, modem);
}

static gboolean update_network_operator_list_cb(void *user_data)
{
	struct ofono_modem *modem = user_data;

	update_network_operator_list(modem);

	return TRUE;
}

static gboolean update_network_operator_list_init(void *user_data)
{
	struct ofono_modem *modem = user_data;

	update_network_operator_list(modem);

	return FALSE;
}

static void set_registration_status(struct ofono_modem *modem, int status)
{
	const char *str_status = registration_status_to_string(status);
	struct network_registration_data *netreg = modem->network_registration;
	DBusConnection *conn = dbus_gsm_connection();

	netreg->status = status;

	dbus_gsm_signal_property_changed(conn, modem->path,
					NETWORK_REGISTRATION_INTERFACE,
					"Status", DBUS_TYPE_STRING,
					&str_status);
}

static void set_registration_location(struct ofono_modem *modem, int lac)
{
	struct network_registration_data *netreg = modem->network_registration;
	DBusConnection *conn = dbus_gsm_connection();
	dbus_uint16_t dbus_lac = lac;

	if (lac > 0xffff)
		return;

	netreg->location = lac;

	if (netreg->location == -1)
		return;

	dbus_gsm_signal_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"LocationAreaCode",
						DBUS_TYPE_UINT16, &dbus_lac);
}

static void set_registration_cellid(struct ofono_modem *modem, int ci)
{
	struct network_registration_data *netreg = modem->network_registration;
	DBusConnection *conn = dbus_gsm_connection();
	dbus_uint32_t dbus_ci = ci;

	netreg->cellid = ci;

	if (netreg->cellid == -1)
		return;

	dbus_gsm_signal_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"CellId", DBUS_TYPE_UINT32,
						&dbus_ci);
}

static void set_registration_technology(struct ofono_modem *modem, int tech)
{
	struct network_registration_data *netreg = modem->network_registration;
	const char *tech_str = registration_tech_to_string(tech);
	DBusConnection *conn = dbus_gsm_connection();

	netreg->technology = tech;

	if (netreg->technology == -1)
		return;

	dbus_gsm_signal_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"Technology", DBUS_TYPE_STRING,
						&tech_str);
}

static void initialize_network_registration(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (!g_dbus_register_interface(conn, modem->path,
					NETWORK_REGISTRATION_INTERFACE,
					network_registration_methods,
					network_registration_signals,
					NULL, modem,
					network_registration_destroy)) {
		ofono_error("Could not register NetworkRegistration interface");
		network_registration_destroy(modem);

		return;
	}

	ofono_debug("NetworkRegistration interface for modem: %s created",
			modem->path);

	modem_add_interface(modem, NETWORK_REGISTRATION_INTERFACE);

	if (modem->network_registration->ops->list_operators) {
		g_timeout_add_seconds(OPERATOR_LIST_UPDATE_TIME,
					update_network_operator_list_cb, modem);

		g_timeout_add_seconds(5, update_network_operator_list_init,
					modem);
	}
}

void ofono_network_registration_notify(struct ofono_modem *modem, int status,
			int lac, int ci, int tech)
{
	struct network_registration_data *netreg = modem->network_registration;

	if (!netreg)
		return;

	if (netreg->status != status)
		set_registration_status(modem, status);

	if (netreg->location != lac)
		set_registration_location(modem, lac);

	if (netreg->cellid != ci)
		set_registration_cellid(modem, ci);

	if (netreg->technology != tech)
		set_registration_technology(modem, tech);

	if (netreg->status == 1 || netreg->status == 5) {
		if (netreg->ops->current_operator)
			netreg->ops->current_operator(modem,
					current_operator_callback, modem);
	} else {
		struct ofono_error error;

		error.type = OFONO_ERROR_TYPE_NO_ERROR;
		error.error = 0;

		current_operator_callback(&error, NULL, modem);

		netreg->signal_strength = -1;
	}
}

static void network_operator_name_override(struct ofono_modem *modem,
					struct ofono_network_operator *op)
{
	op->override_name =
		ofono_operator_name_sim_override(modem, op->mcc, op->mnc);
}

static void operator_list_callback(const struct ofono_error *error, int total,
					const struct ofono_network_operator *list,
					void *data)
{
	struct ofono_modem *modem = data;
	struct network_registration_data *netreg = modem->network_registration;
	GSList *n = NULL;
	GSList *o;
	int i;
	gboolean need_to_emit = FALSE;

	netreg->flags &= ~NETWORK_REGISTRATION_FLAG_REQUESTING_OPLIST;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during operator list");
		return;
	}

	for (i = 0; i < total; i++) {
		o = g_slist_find_custom(netreg->operator_list, &list[i],
					network_operator_compare);

		if (o) { /* Update and move to a new list */
			set_network_operator_status(modem, o->data,
							list[i].status);

			set_network_operator_technology(modem, o->data,
							list[i].tech);

			set_network_operator_name(modem, o->data,
							list[i].name);

			n = g_slist_prepend(n, o->data);
			netreg->operator_list =
				g_slist_remove(netreg->operator_list, o->data);
		} else {
			/* New operator */
			struct ofono_network_operator *op =
				g_memdup(&list[i],
					sizeof(struct ofono_network_operator));
			if (!op)
				continue;

			network_operator_name_override(modem, op);

			if (network_operator_dbus_register(modem, op)) {
				n = g_slist_prepend(n, op);
				need_to_emit = TRUE;
			}
		}
	}

	if (n)
		n = g_slist_reverse(n);

	if (netreg->operator_list)
		need_to_emit = TRUE;

	for (o = netreg->operator_list; o; o = o->next) {
		network_operator_dbus_unregister(modem, o->data);
		g_free(o->data);
	}

	g_slist_free(netreg->operator_list);

	netreg->operator_list = n;

	if (need_to_emit)
		network_operator_emit_available_operators(modem);
}

static void current_operator_callback(const struct ofono_error *error,
				const struct ofono_network_operator *current,
				void *data)
{
	DBusConnection *conn = dbus_gsm_connection();
	struct ofono_modem *modem = data;
	struct network_registration_data *netreg = modem->network_registration;
	GSList *op = NULL;
	const char *operator;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during current operator");
		return;
	}

	if (!netreg->current_operator && !current)
		return;

	/* We got a new network operator, reset the previous one's status */
	/* It will be updated properly later */
	if (netreg->current_operator &&
		(!current ||
			network_operator_compare(current, netreg->current_operator)))
		set_network_operator_status(modem, netreg->current_operator,
						OPERATOR_STATUS_AVAILABLE);

	if (current)
		op = g_slist_find_custom(netreg->operator_list, current,
					network_operator_compare);

	if (op) {
		set_network_operator_status(modem, op->data,
						OPERATOR_STATUS_CURRENT);
		set_network_operator_technology(modem, op->data,
						current->tech);
		set_network_operator_name(modem, op->data, current->name);

		if (netreg->current_operator == op->data)
			return;

		netreg->current_operator = op->data;
		goto emit;
	}

	if (current) {
		netreg->current_operator =
			g_memdup(current,
				sizeof(struct ofono_network_operator));
		if (!netreg->current_operator) {
			ofono_error("Unable to allocate current operator");
			return;
		}

		network_operator_name_override(modem, netreg->current_operator);

		netreg->current_operator->status = OPERATOR_STATUS_CURRENT;

		netreg->operator_list = g_slist_append(netreg->operator_list,
						netreg->current_operator);

		network_operator_dbus_register(modem, netreg->current_operator);
		network_operator_emit_available_operators(modem);
	} else {
		/* We don't free this here because operator is registered */
		/* Taken care of elsewhere */
		netreg->current_operator = NULL;
	}

emit:
	operator = get_operator_display_name(modem);

	dbus_gsm_signal_property_changed(conn, modem->path,
					NETWORK_REGISTRATION_INTERFACE,
					"Operator", DBUS_TYPE_STRING,
					&operator);
}

static void registration_status_callback(const struct ofono_error *error,
					int status, int lac, int ci, int tech,
					void *data)
{
	struct ofono_modem *modem = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during registration status query");
		return;
	}

	ofono_network_registration_notify(modem, status, lac, ci, tech);
}

static void init_registration_status(const struct ofono_error *error,
					int status, int lac, int ci, int tech,
					void *data)
{
	struct ofono_modem *modem = data;
	struct network_registration_data *netreg = modem->network_registration;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during registration status query");
		return;
	}

	ofono_network_registration_notify(modem, status, lac, ci, tech);

	/* Bootstrap our signal strength value without waiting for the
	 * stack to report it
	 */
	if (netreg->status == 1 || netreg->status == 5) {
		if (netreg->ops->signal_strength)
			netreg->ops->signal_strength(modem,
					signal_strength_callback, modem);
	}

	if (AUTO_REGISTER && (status == 0 || status == 3))
		netreg->ops->register_auto(modem, register_callback, modem);
}

void ofono_signal_strength_notify(struct ofono_modem *modem, int strength)
{
	struct network_registration_data *netreg = modem->network_registration;
	DBusConnection *conn = dbus_gsm_connection();

	if (netreg->signal_strength == strength)
		return;

	/* Theoretically we can get signal strength even when not registered
	 * to any network.  However, what do we do with it in that case?
	 */
	if (netreg->status != NETWORK_REGISTRATION_STATUS_REGISTERED &&
		netreg->status != NETWORK_REGISTRATION_STATUS_ROAMING)
		return;

	netreg->signal_strength = strength;

	if (strength != -1) {
		dbus_uint16_t strength = netreg->signal_strength;

		dbus_gsm_signal_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"Strength", DBUS_TYPE_UINT16,
						&strength);
	}
}

static void signal_strength_callback(const struct ofono_error *error,
					int strength, void *data)
{
	struct ofono_modem *modem = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error during signal strength query");
		return;
	}

	ofono_signal_strength_notify(modem, strength);
}

static void sim_opl_read_cb(struct ofono_modem *modem, int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct network_registration_data *netreg = modem->network_registration;
	int total = length / record_length;

	if (!ok)
		return;

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

	if (length < 8 || record_length < 8 || length < record_length)
		return;

	/* If we don't have PNN, ignore this completely */
	if (!netreg->eons)
		return;

	sim_eons_add_pnn_record(netreg->eons, record, data, record_length);

	if (record == total)
		sim_eons_optimize(netreg->eons);
}

static void sim_pnn_read_cb(struct ofono_modem *modem, int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct network_registration_data *netreg = modem->network_registration;
	int total = length / record_length;

	if (!ok)
		return;

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

	if (length < 3 || record_length < 3 || length < record_length)
		return;

	if (!netreg->eons)
		netreg->eons = sim_eons_new(total);

	sim_eons_add_pnn_record(netreg->eons, record, data, record_length);

	/* If PNN is not present then OPL is not useful, don't
	 * retrieve it.  If OPL is not there then PNN[1] will
	 * still be used for the HPLMN and/or EHPLMN, if PNN
	 * is present.  */
	if (record == total && !sim_eons_pnn_is_empty(netreg->eons))
		ofono_sim_read(modem, SIM_EFOPL_FILEID, sim_opl_read_cb, NULL);
}

static void sim_spdi_read_cb(struct ofono_modem *modem, int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct network_registration_data *netreg = modem->network_registration;

	if (!ok)
		return;

	if (structure != OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		return;

	netreg->spdi = sim_spdi_new(data, length);

	if (!netreg->current_operator)
		return;

	if (netreg->status == NETWORK_REGISTRATION_STATUS_ROAMING) {
		DBusConnection *conn = dbus_gsm_connection();
		const char *operator;

		if (!sim_spdi_lookup(netreg->spdi,
					netreg->current_operator->mcc,
					netreg->current_operator->mnc))
			return;

		operator = get_operator_display_name(modem);

		dbus_gsm_signal_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"Operator", DBUS_TYPE_STRING,
						&operator);
	}
}

static void sim_spn_read_cb(struct ofono_modem *modem, int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct network_registration_data *netreg = modem->network_registration;
	unsigned char dcbyte;
	char *spn;

	if (!ok)
		return;

	if (structure != OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		return;

	dcbyte = data[0];

	/* TS 31.102 says:
	 *
	 * the string shall use:
	 *
	 * - either the SMS default 7-bit coded alphabet as defined in
	 *   TS 23.038 [5] with bit 8 set to 0. The string shall be left
	 *   justified. Unused bytes shall be set to 'FF'.
	 *
	 * - or one of the UCS2 code options defined in the annex of TS
	 *   31.101 [11].
	 *
	 * 31.101 has no such annex though.  51.101 refers to Annex B of
	 * itself which is not there either.  11.11 contains the same
	 * paragraph as 51.101 and has an Annex B which we implement.
	 */
	spn = sim_string_to_utf8(data + 1, length - 1);

	if (!spn) {
		ofono_error("EFspn read successfully, but couldn't parse");
		return;
	}

	if (strlen(spn) == 0) {
		g_free(spn);
		return;
	}

	netreg->spname = spn;
	ofono_sim_read(modem, SIM_EFSPDI_FILEID, sim_spdi_read_cb, NULL);

	if (dcbyte & SIM_EFSPN_DC_HOME_PLMN_BIT)
		netreg->flags |= NETWORK_REGISTRATION_FLAG_HOME_SHOW_PLMN;

	if (!(dcbyte & SIM_EFSPN_DC_ROAMING_SPN_BIT))
		netreg->flags |= NETWORK_REGISTRATION_FLAG_ROAMING_SHOW_SPN;

	if (netreg->current_operator) {
		DBusConnection *conn = dbus_gsm_connection();
		const char *operator;

		operator = get_operator_display_name(modem);

		dbus_gsm_signal_property_changed(conn, modem->path,
						NETWORK_REGISTRATION_INTERFACE,
						"Operator", DBUS_TYPE_STRING,
						&operator);
	}
}

static void sim_ready(struct ofono_modem *modem)
{
	ofono_sim_read(modem, SIM_EFPNN_FILEID, sim_pnn_read_cb, NULL);
	ofono_sim_read(modem, SIM_EFSPN_FILEID, sim_spn_read_cb, NULL);
}

int ofono_network_registration_register(struct ofono_modem *modem,
					struct ofono_network_registration_ops *ops)
{
	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->network_registration = network_registration_create();
	if (modem->network_registration == NULL)
		return -1;

	modem->network_registration->ops = ops;

	initialize_network_registration(modem);

	if (ops->registration_status)
		ops->registration_status(modem, init_registration_status,
						modem);

	return 0;
}

void ofono_network_registration_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();

	g_dbus_unregister_interface(conn, modem->path,
					NETWORK_REGISTRATION_INTERFACE);
	modem_remove_interface(modem, NETWORK_REGISTRATION_INTERFACE);
}

