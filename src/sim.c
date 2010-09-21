/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "ofono.h"

#include "common.h"
#include "util.h"
#include "smsutil.h"
#include "simutil.h"
#include "storage.h"
#include "simfs.h"

static GSList *g_drivers = NULL;

static void sim_own_numbers_update(struct ofono_sim *sim);
static void sim_pin_check(struct ofono_sim *sim);
static void sim_set_ready(struct ofono_sim *sim);

struct ofono_sim {
	/* Contents of the SIM file system, in rough initialization order */
	char *iccid;

	char **language_prefs;
	unsigned char *efli;
	unsigned char efli_length;

	enum ofono_sim_password_type pin_type;
	gboolean locked_pins[OFONO_SIM_PASSWORD_SIM_PUK]; /* Number of PINs */

	enum ofono_sim_phase phase;
	unsigned char mnc_length;
	enum ofono_sim_cphs_phase cphs_phase;
	unsigned char cphs_service_table[2];
	unsigned char *efust;
	unsigned char efust_length;
	unsigned char *efest;
	unsigned char efest_length;
	unsigned char *efsst;
	unsigned char efsst_length;

	char *imsi;

	GSList *own_numbers;
	GSList *new_numbers;
	unsigned char efmsisdn_length;
	unsigned char efmsisdn_records;

	GSList *service_numbers;
	gboolean sdn_ready;

	unsigned char *efimg;
	unsigned short efimg_length;

	enum ofono_sim_state state;
	struct ofono_watchlist *state_watches;

	struct sim_fs *simfs;

	DBusMessage *pending;
	const struct ofono_sim_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct msisdn_set_request {
	struct ofono_sim *sim;
	int pending;
	int failed;
	DBusMessage *msg;
};

struct service_number {
	char *id;
	struct ofono_phone_number ph;
};

static const char *const passwd_name[] = {
	[OFONO_SIM_PASSWORD_NONE] = "none",
	[OFONO_SIM_PASSWORD_SIM_PIN] = "pin",
	[OFONO_SIM_PASSWORD_SIM_PUK] = "puk",
	[OFONO_SIM_PASSWORD_PHSIM_PIN] = "phone",
	[OFONO_SIM_PASSWORD_PHFSIM_PIN] = "firstphone",
	[OFONO_SIM_PASSWORD_PHFSIM_PUK] = "firstphonepuk",
	[OFONO_SIM_PASSWORD_SIM_PIN2] = "pin2",
	[OFONO_SIM_PASSWORD_SIM_PUK2] = "puk2",
	[OFONO_SIM_PASSWORD_PHNET_PIN] = "network",
	[OFONO_SIM_PASSWORD_PHNET_PUK] = "networkpuk",
	[OFONO_SIM_PASSWORD_PHNETSUB_PIN] = "netsub",
	[OFONO_SIM_PASSWORD_PHNETSUB_PUK] = "netsubpuk",
	[OFONO_SIM_PASSWORD_PHSP_PIN] = "service",
	[OFONO_SIM_PASSWORD_PHSP_PUK] = "servicepuk",
	[OFONO_SIM_PASSWORD_PHCORP_PIN] = "corp",
	[OFONO_SIM_PASSWORD_PHCORP_PUK] = "corppuk",
};

static const char *sim_passwd_name(enum ofono_sim_password_type type)
{
	return passwd_name[type];
}

static enum ofono_sim_password_type sim_string_to_passwd(const char *name)
{
	int len = sizeof(passwd_name) / sizeof(*passwd_name);
	int i;

	for (i = 0; i < len; i++)
		if (!strcmp(passwd_name[i], name))
			return i;

	return OFONO_SIM_PASSWORD_INVALID;
}

static gboolean password_is_pin(enum ofono_sim_password_type type)
{
	switch (type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
	case OFONO_SIM_PASSWORD_SIM_PIN2:
	case OFONO_SIM_PASSWORD_PHNET_PIN:
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
	case OFONO_SIM_PASSWORD_PHSP_PIN:
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		return TRUE;
	case OFONO_SIM_PASSWORD_SIM_PUK:
	case OFONO_SIM_PASSWORD_PHFSIM_PUK:
	case OFONO_SIM_PASSWORD_SIM_PUK2:
	case OFONO_SIM_PASSWORD_PHNET_PUK:
	case OFONO_SIM_PASSWORD_PHNETSUB_PUK:
	case OFONO_SIM_PASSWORD_PHSP_PUK:
	case OFONO_SIM_PASSWORD_PHCORP_PUK:
	case OFONO_SIM_PASSWORD_INVALID:
	case OFONO_SIM_PASSWORD_NONE:
		return FALSE;
	}

	return FALSE;
}

static enum ofono_sim_password_type puk2pin(enum ofono_sim_password_type type)
{
	switch (type) {
	case OFONO_SIM_PASSWORD_SIM_PUK:
		return OFONO_SIM_PASSWORD_SIM_PIN;
	case OFONO_SIM_PASSWORD_PHFSIM_PUK:
		return OFONO_SIM_PASSWORD_PHFSIM_PIN;
	case OFONO_SIM_PASSWORD_SIM_PUK2:
		return OFONO_SIM_PASSWORD_SIM_PIN2;
	case OFONO_SIM_PASSWORD_PHNET_PUK:
		return OFONO_SIM_PASSWORD_PHNET_PUK;
	case OFONO_SIM_PASSWORD_PHNETSUB_PUK:
		return OFONO_SIM_PASSWORD_PHNETSUB_PIN;
	case OFONO_SIM_PASSWORD_PHSP_PUK:
		return OFONO_SIM_PASSWORD_PHSP_PIN;
	case OFONO_SIM_PASSWORD_PHCORP_PUK:
		return OFONO_SIM_PASSWORD_PHCORP_PIN;
	default:
		return OFONO_SIM_PASSWORD_INVALID;
	}
}

static char **get_own_numbers(GSList *own_numbers)
{
	int nelem = 0;
	GSList *l;
	struct ofono_phone_number *num;
	char **ret;

	if (own_numbers)
		nelem = g_slist_length(own_numbers);

	ret = g_new0(char *, nelem + 1);

	nelem = 0;
	for (l = own_numbers; l; l = l->next) {
		num = l->data;

		ret[nelem++] = g_strdup(phone_number_to_string(num));
	}

	return ret;
}

static char **get_locked_pins(struct ofono_sim *sim)
{
	int i;
	int nelem = 0;
	char **ret;

	for (i = 1; i < OFONO_SIM_PASSWORD_SIM_PUK; i++) {
		if (sim->locked_pins[i] == FALSE)
			continue;

		nelem += 1;
	}

	ret = g_new0(char *, nelem + 1);

	nelem = 0;

	for (i = 1; i < OFONO_SIM_PASSWORD_SIM_PUK; i++) {
		if (sim->locked_pins[i] == FALSE)
			continue;

		ret[nelem] = g_strdup(sim_passwd_name(i));
		nelem += 1;
	}

	return ret;
}

static char **get_service_numbers(GSList *service_numbers)
{
	int nelem;
	GSList *l;
	struct service_number *num;
	char **ret;

	nelem = g_slist_length(service_numbers) * 2;

	ret = g_new0(char *, nelem + 1);

	nelem = 0;
	for (l = service_numbers; l; l = l->next) {
		num = l->data;

		ret[nelem++] = g_strdup(num->id);
		ret[nelem++] = g_strdup(phone_number_to_string(&num->ph));
	}

	return ret;
}

static void service_number_free(struct service_number *num)
{
	g_free(num->id);
	g_free(num);
}

static DBusMessage *sim_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char **own_numbers;
	char **service_numbers;
	char **locked_pins;
	const char *pin_name;
	dbus_bool_t present = sim->state != OFONO_SIM_STATE_NOT_PRESENT;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "Present", DBUS_TYPE_BOOLEAN, &present);

	if (!present)
		goto done;

	if (sim->iccid)
		ofono_dbus_dict_append(&dict, "CardIdentifier",
					DBUS_TYPE_STRING, &sim->iccid);

	if (sim->imsi)
		ofono_dbus_dict_append(&dict, "SubscriberIdentity",
					DBUS_TYPE_STRING, &sim->imsi);

	if (sim->mnc_length) {
		char mcc[OFONO_MAX_MCC_LENGTH + 1];
		char mnc[OFONO_MAX_MNC_LENGTH + 1];
		const char *str;

		strncpy(mcc, sim->imsi, OFONO_MAX_MCC_LENGTH);
		mcc[OFONO_MAX_MCC_LENGTH] = '\0';
		strncpy(mnc, sim->imsi + OFONO_MAX_MCC_LENGTH, sim->mnc_length);
		mnc[sim->mnc_length] = '\0';

		str = mcc;
		ofono_dbus_dict_append(&dict, "MobileCountryCode",
					DBUS_TYPE_STRING, &str);

		str = mnc;
		ofono_dbus_dict_append(&dict, "MobileNetworkCode",
					DBUS_TYPE_STRING, &str);
	}

	own_numbers = get_own_numbers(sim->own_numbers);

	ofono_dbus_dict_append_array(&dict, "SubscriberNumbers",
					DBUS_TYPE_STRING, &own_numbers);
	g_strfreev(own_numbers);

	locked_pins = get_locked_pins(sim);
	ofono_dbus_dict_append_array(&dict, "LockedPins",
					DBUS_TYPE_STRING, &locked_pins);
	g_strfreev(locked_pins);

	if (sim->service_numbers && sim->sdn_ready) {
		service_numbers = get_service_numbers(sim->service_numbers);

		ofono_dbus_dict_append_dict(&dict, "ServiceNumbers",
						DBUS_TYPE_STRING,
						&service_numbers);
		g_strfreev(service_numbers);
	}

	if (sim->language_prefs)
		ofono_dbus_dict_append_array(&dict, "PreferredLanguages",
						DBUS_TYPE_STRING,
						&sim->language_prefs);

	pin_name = sim_passwd_name(sim->pin_type);
	ofono_dbus_dict_append(&dict, "PinRequired",
				DBUS_TYPE_STRING,
				(void *) &pin_name);

done:
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void msisdn_set_done(struct msisdn_set_request *req)
{
	DBusMessage *reply;

	if (req->failed)
		reply = __ofono_error_failed(req->msg);
	else
		reply = dbus_message_new_method_return(req->msg);

	__ofono_dbus_pending_reply(&req->msg, reply);

	/* Re-read the numbers and emit signal if needed */
	sim_own_numbers_update(req->sim);

	g_free(req);
}

static void msisdn_set_cb(int ok, void *data)
{
	struct msisdn_set_request *req = data;

	if (!ok)
		req->failed++;

	req->pending--;

	if (!req->pending)
		msisdn_set_done(req);
}

static gboolean set_own_numbers(struct ofono_sim *sim,
				GSList *new_numbers, DBusMessage *msg)
{
	struct msisdn_set_request *req;
	int record;
	unsigned char efmsisdn[255];
	struct ofono_phone_number *number;

	if (new_numbers && g_slist_length(new_numbers) > sim->efmsisdn_records)
		return FALSE;

	req = g_new0(struct msisdn_set_request, 1);

	req->sim = sim;
	req->msg = dbus_message_ref(msg);

	for (record = 1; record <= sim->efmsisdn_records; record++) {
		if (new_numbers) {
			number = new_numbers->data;
			sim_adn_build(efmsisdn, sim->efmsisdn_length,
					number, NULL);
			new_numbers = new_numbers->next;
		} else {
			memset(efmsisdn, 0xff, sim->efmsisdn_length);
		}

		if (ofono_sim_write(req->sim, SIM_EFMSISDN_FILEID,
				msisdn_set_cb, OFONO_SIM_FILE_STRUCTURE_FIXED,
				record, efmsisdn,
				sim->efmsisdn_length, req) == 0)
			req->pending++;
		else
			req->failed++;
	}

	if (!req->pending)
		msisdn_set_done(req);

	return TRUE;
}

static DBusMessage *sim_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	DBusMessageIter var_elem;
	const char *name, *value;

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);

	if (!strcmp(name, "SubscriberNumbers")) {
		gboolean set_ok = FALSE;
		struct ofono_phone_number *own;
		GSList *own_numbers = NULL;

		if (sim->efmsisdn_length == 0)
			return __ofono_error_busy(msg);

		dbus_message_iter_next(&iter);

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_recurse(&iter, &var);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY ||
				dbus_message_iter_get_element_type(&var) !=
				DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_recurse(&var, &var_elem);

		/* Empty lists are supported */
		while (dbus_message_iter_get_arg_type(&var_elem) !=
				DBUS_TYPE_INVALID) {
			if (dbus_message_iter_get_arg_type(&var_elem) !=
					DBUS_TYPE_STRING)
				goto error;

			dbus_message_iter_get_basic(&var_elem, &value);

			if (!valid_phone_number_format(value))
				goto error;

			own = g_new0(struct ofono_phone_number, 1);
			string_to_phone_number(value, own);

			own_numbers = g_slist_prepend(own_numbers, own);

			dbus_message_iter_next(&var_elem);
		}

		own_numbers = g_slist_reverse(own_numbers);
		set_ok = set_own_numbers(sim, own_numbers, msg);

error:
		g_slist_foreach(own_numbers, (GFunc) g_free, 0);
		g_slist_free(own_numbers);

		if (set_ok)
			return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

static void sim_locked_cb(struct ofono_sim *sim, gboolean locked)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	const char *typestr;
	const char *pin;
	char **locked_pins;
	enum ofono_sim_password_type type;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(sim->pending);

	dbus_message_get_args(sim->pending, NULL, DBUS_TYPE_STRING, &typestr,
					DBUS_TYPE_STRING, &pin,
					DBUS_TYPE_INVALID);

	type = sim_string_to_passwd(typestr);

	/* This is used by lock/unlock pin, no puks allowed */
	sim->locked_pins[type] = locked;
	__ofono_dbus_pending_reply(&sim->pending, reply);

	locked_pins = get_locked_pins(sim);
	ofono_dbus_signal_array_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"LockedPins", DBUS_TYPE_STRING,
						&locked_pins);
	g_strfreev(locked_pins);
}

static void sim_unlock_cb(const struct ofono_error *error, void *data)
{
	struct ofono_sim *sim = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBusMessage *reply = __ofono_error_failed(sim->pending);
		__ofono_dbus_pending_reply(&sim->pending, reply);
		return;
	}

	sim_locked_cb(sim, FALSE);
}

static void sim_lock_cb(const struct ofono_error *error, void *data)
{
	struct ofono_sim *sim = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBusMessage *reply = __ofono_error_failed(sim->pending);
		__ofono_dbus_pending_reply(&sim->pending, reply);
		return;
	}

	sim_locked_cb(sim, TRUE);
}

static DBusMessage *sim_lock_or_unlock(struct ofono_sim *sim, int lock,
					DBusConnection *conn, DBusMessage *msg)
{
	enum ofono_sim_password_type type;
	const char *typestr;
	const char *pin;

	if (!sim->driver->lock)
		return __ofono_error_not_implemented(msg);

	if (sim->pending)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &typestr,
					DBUS_TYPE_STRING, &pin,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	type = sim_string_to_passwd(typestr);

	/*
	 * SIM PIN2 cannot be locked / unlocked according to 27.007,
	 * however the PIN combination can be changed
	 */
	if (password_is_pin(type) == FALSE ||
			type == OFONO_SIM_PASSWORD_SIM_PIN2)
		return __ofono_error_invalid_format(msg);

	if (!is_valid_pin(pin, PIN_TYPE_PIN))
		return __ofono_error_invalid_format(msg);

	sim->pending = dbus_message_ref(msg);

	sim->driver->lock(sim, type, lock, pin,
				lock ? sim_lock_cb : sim_unlock_cb, sim);

	return NULL;
}

static DBusMessage *sim_lock_pin(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;

	return sim_lock_or_unlock(sim, 1, conn, msg);
}

static DBusMessage *sim_unlock_pin(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;

	return sim_lock_or_unlock(sim, 0, conn, msg);
}

static void sim_change_pin_cb(const struct ofono_error *error, void *data)
{
	struct ofono_sim *sim = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&sim->pending,
				__ofono_error_failed(sim->pending));
		return;
	}

	__ofono_dbus_pending_reply(&sim->pending,
				dbus_message_new_method_return(sim->pending));
}

static DBusMessage *sim_change_pin(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;
	enum ofono_sim_password_type type;
	const char *typestr;
	const char *old;
	const char *new;

	if (!sim->driver->change_passwd)
		return __ofono_error_not_implemented(msg);

	if (sim->pending)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &typestr,
					DBUS_TYPE_STRING, &old,
					DBUS_TYPE_STRING, &new,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	type = sim_string_to_passwd(typestr);

	if (password_is_pin(type) == FALSE)
		return __ofono_error_invalid_format(msg);

	if (!is_valid_pin(old, PIN_TYPE_PIN))
		return __ofono_error_invalid_format(msg);

	if (!is_valid_pin(new, PIN_TYPE_PIN))
		return __ofono_error_invalid_format(msg);

	if (!strcmp(new, old))
		return dbus_message_new_method_return(msg);

	sim->pending = dbus_message_ref(msg);
	sim->driver->change_passwd(sim, type, old, new,
					sim_change_pin_cb, sim);

	return NULL;
}

static void sim_enter_pin_cb(const struct ofono_error *error, void *data)
{
	struct ofono_sim *sim = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		reply = __ofono_error_failed(sim->pending);
	else
		reply = dbus_message_new_method_return(sim->pending);

	__ofono_dbus_pending_reply(&sim->pending, reply);

	sim_pin_check(sim);
}

static DBusMessage *sim_enter_pin(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;
	const char *typestr;
	enum ofono_sim_password_type type;
	const char *pin;

	if (!sim->driver->send_passwd)
		return __ofono_error_not_implemented(msg);

	if (sim->pending)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &typestr,
					DBUS_TYPE_STRING, &pin,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	type = sim_string_to_passwd(typestr);

	if (type == OFONO_SIM_PASSWORD_NONE || type != sim->pin_type)
		return __ofono_error_invalid_format(msg);

	if (!is_valid_pin(pin, PIN_TYPE_PIN))
		return __ofono_error_invalid_format(msg);

	sim->pending = dbus_message_ref(msg);
	sim->driver->send_passwd(sim, pin, sim_enter_pin_cb, sim);

	return NULL;
}

static DBusMessage *sim_reset_pin(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;
	const char *typestr;
	enum ofono_sim_password_type type;
	const char *puk;
	const char *pin;

	if (!sim->driver->reset_passwd)
		return __ofono_error_not_implemented(msg);

	if (sim->pending)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &typestr,
					DBUS_TYPE_STRING, &puk,
					DBUS_TYPE_STRING, &pin,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	type = sim_string_to_passwd(typestr);

	if (type == OFONO_SIM_PASSWORD_NONE || type != sim->pin_type)
		return __ofono_error_invalid_format(msg);

	if (!is_valid_pin(puk, PIN_TYPE_PUK))
		return __ofono_error_invalid_format(msg);

	if (!is_valid_pin(pin, PIN_TYPE_PIN))
		return __ofono_error_invalid_format(msg);

	sim->pending = dbus_message_ref(msg);
	sim->driver->reset_passwd(sim, puk, pin, sim_enter_pin_cb, sim);

	return NULL;
}

static GDBusMethodTable sim_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	sim_get_properties	},
	{ "SetProperty",	"sv",	"",		sim_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "ChangePin",		"sss",	"",		sim_change_pin,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "EnterPin",		"ss",	"",		sim_enter_pin,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "ResetPin",		"sss",	"",		sim_reset_pin,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "LockPin",		"ss",	"",		sim_lock_pin,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "UnlockPin",		"ss",	"",		sim_unlock_pin,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable sim_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static gboolean numbers_list_equal(GSList *a, GSList *b)
{
	struct ofono_phone_number *num_a, *num_b;

	while (a || b) {
		if (!a || !b)
			return FALSE;

		num_a = a->data;
		num_b = b->data;

		if (!g_str_equal(num_a->number, num_b->number) ||
				num_a->type != num_b->type)
			return FALSE;

		a = a->next;
		b = b->next;
	}

	return TRUE;
}

static void sim_msisdn_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	int total;
	struct ofono_phone_number ph;

	if (!ok)
		goto check;

	if (record_length < 14 || length < record_length)
		return;

	total = length / record_length;

	sim->efmsisdn_length = record_length;
	sim->efmsisdn_records = total;

	if (sim_adn_parse(data, record_length, &ph, NULL) == TRUE) {
		struct ofono_phone_number *own;

		own = g_new(struct ofono_phone_number, 1);
		memcpy(own, &ph, sizeof(struct ofono_phone_number));
		sim->new_numbers = g_slist_prepend(sim->new_numbers, own);
	}

	if (record != total)
		return;

check:
	/* All records retrieved */
	if (sim->new_numbers)
		sim->new_numbers = g_slist_reverse(sim->new_numbers);

	if (!numbers_list_equal(sim->new_numbers, sim->own_numbers)) {
		const char *path = __ofono_atom_get_path(sim->atom);
		char **own_numbers;
		DBusConnection *conn = ofono_dbus_get_connection();

		g_slist_foreach(sim->own_numbers, (GFunc) g_free, NULL);
		g_slist_free(sim->own_numbers);
		sim->own_numbers = sim->new_numbers;

		own_numbers = get_own_numbers(sim->own_numbers);

		ofono_dbus_signal_array_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"SubscriberNumbers",
						DBUS_TYPE_STRING, &own_numbers);

		g_strfreev(own_numbers);
	} else {
		g_slist_foreach(sim->new_numbers, (GFunc) g_free, NULL);
		g_slist_free(sim->new_numbers);
	}

	sim->new_numbers = NULL;
}

static gint service_number_compare(gconstpointer a, gconstpointer b)
{
	const struct service_number *sdn = a;
	const char *id = b;

	return strcmp(sdn->id, id);
}

static void sim_sdn_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	int total;
	struct ofono_phone_number ph;
	char *alpha;
	struct service_number *sdn;

	if (!ok)
		goto check;

	if (record_length < 14 || length < record_length)
		return;

	total = length / record_length;

	if (sim_adn_parse(data, record_length, &ph, &alpha) == FALSE)
		goto out;


	/* Use phone number if Id is unavailable */
	if (alpha && alpha[0] == '\0') {
		g_free(alpha);
		alpha = NULL;
	}

	if (alpha == NULL)
		alpha = g_strdup(phone_number_to_string(&ph));

	if (sim->service_numbers &&
			g_slist_find_custom(sim->service_numbers,
				alpha, service_number_compare)) {
		ofono_error("Duplicate EFsdn entries for `%s'\n",
				alpha);
		g_free(alpha);

		goto out;
	}

	sdn = g_new(struct service_number, 1);
	sdn->id = alpha;
	memcpy(&sdn->ph, &ph, sizeof(struct ofono_phone_number));

	sim->service_numbers = g_slist_prepend(sim->service_numbers, sdn);

out:
	if (record != total)
		return;

check:
	/* All records retrieved */
	if (sim->service_numbers) {
		char **service_numbers;

		sim->service_numbers = g_slist_reverse(sim->service_numbers);
		sim->sdn_ready = TRUE;

		service_numbers = get_service_numbers(sim->service_numbers);

		ofono_dbus_signal_dict_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"ServiceNumbers",
						DBUS_TYPE_STRING,
						&service_numbers);
		g_strfreev(service_numbers);
	}
}

static void sim_own_numbers_update(struct ofono_sim *sim)
{
	ofono_sim_read(sim, SIM_EFMSISDN_FILEID, OFONO_SIM_FILE_STRUCTURE_FIXED,
			sim_msisdn_read_cb, sim);
}

static void sim_efimg_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	unsigned char *efimg;
	int num_records;

	if (!ok)
		return;

	num_records = length / record_length;

	/*
	 * EFimg descriptors are 9 bytes long.
	 * Byte 1 of the record is the number of descriptors per record.
	 */
	if ((record_length < 10) ||
			((record_length % 9 != 2) && (record_length % 9 != 1)))
		return;

	if (sim->efimg == NULL) {
		sim->efimg = g_try_malloc0(num_records * 9);

		if (sim->efimg == NULL)
			return;

		sim->efimg_length = num_records * 9;
	}

	/*
	 * TBD - if we have more than one descriptor per record,
	 * pick the nicest one.  For now we use the first one.
	 */

	/* copy descriptor into slot for this record */
	efimg = &sim->efimg[(record - 1) * 9];

	memcpy(efimg, &data[1], 9);
}

static void sim_ready(enum ofono_sim_state new_state, void *user)
{
	struct ofono_sim *sim = user;

	if (new_state != OFONO_SIM_STATE_READY)
		return;

	sim_own_numbers_update(sim);

	ofono_sim_read(sim, SIM_EFSDN_FILEID, OFONO_SIM_FILE_STRUCTURE_FIXED,
			sim_sdn_read_cb, sim);
	ofono_sim_read(sim, SIM_EFIMG_FILEID, OFONO_SIM_FILE_STRUCTURE_FIXED,
			sim_efimg_read_cb, sim);
}

static void sim_imsi_cb(const struct ofono_error *error, const char *imsi,
		void *data)
{
	struct ofono_sim *sim = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Unable to read IMSI, emergency calls only");
		return;
	}

	sim->imsi = g_strdup(imsi);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"SubscriberIdentity",
						DBUS_TYPE_STRING, &sim->imsi);

	if (sim->mnc_length) {
		char mcc[OFONO_MAX_MCC_LENGTH + 1];
		char mnc[OFONO_MAX_MNC_LENGTH + 1];
		const char *str;

		strncpy(mcc, sim->imsi, OFONO_MAX_MCC_LENGTH);
		mcc[OFONO_MAX_MCC_LENGTH] = '\0';
		strncpy(mnc, sim->imsi + OFONO_MAX_MCC_LENGTH, sim->mnc_length);
		mnc[sim->mnc_length] = '\0';

		str = mcc;
		ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"MobileCountryCode",
						DBUS_TYPE_STRING, &str);

		str = mnc;
		ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"MobileNetworkCode",
						DBUS_TYPE_STRING, &str);
	}

	sim_set_ready(sim);
}

static void sim_retrieve_imsi(struct ofono_sim *sim)
{
	if (!sim->driver->read_imsi) {
		ofono_error("IMSI retrieval not implemented,"
				" only emergency calls will be available");
		return;
	}

	sim->driver->read_imsi(sim, sim_imsi_cb, sim);
}

static void sim_efsst_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (!ok)
		goto out;

	if (length < 2) {
		ofono_error("EFsst shall contain at least two bytes");
		goto out;
	}

	sim->efsst = g_memdup(data, length);
	sim->efsst_length = length;

out:
	sim_retrieve_imsi(sim);
}

static void sim_efest_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (!ok)
		goto out;

	if (length < 1) {
		ofono_error("EFest shall contain at least one byte");
		goto out;
	}

	sim->efest = g_memdup(data, length);
	sim->efest_length = length;

out:
	sim_retrieve_imsi(sim);
}

static void sim_efust_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (!ok)
		goto out;

	if (length < 1) {
		ofono_error("EFust shall contain at least one byte");
		goto out;
	}

	sim->efust = g_memdup(data, length);
	sim->efust_length = length;

	ofono_sim_read(sim, SIM_EFEST_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efest_read_cb, sim);

	return;

out:
	sim_retrieve_imsi(sim);
}

static void sim_cphs_information_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;

	sim->cphs_phase = OFONO_SIM_CPHS_PHASE_NONE;

	if (!ok || length < 3)
		return;

	if (data[0] == 0x01)
		sim->cphs_phase = OFONO_SIM_CPHS_PHASE_1G;
	else if (data[0] >= 0x02)
		sim->cphs_phase = OFONO_SIM_CPHS_PHASE_2G;

	memcpy(sim->cphs_service_table, data + 1, 2);
}

static void sim_ad_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	int new_mnc_length;

	if (!ok)
		return;

	if (length < 4)
		return;

	new_mnc_length = data[3] & 0xf;

	/* sanity check for potential invalid values */
	if (new_mnc_length < 2 || new_mnc_length > 3)
		return;

	sim->mnc_length = new_mnc_length;
}

static void sim_efphase_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (!ok || length != 1)
		sim->phase = OFONO_SIM_PHASE_3G;
	else
		sim->phase = data[0];
}

static void sim_initialize_after_pin(struct ofono_sim *sim)
{
	ofono_sim_read(sim, SIM_EFPHASE_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efphase_read_cb, sim);

	ofono_sim_read(sim, SIM_EFAD_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_ad_read_cb, sim);

	/*
	 * Read CPHS-support bits, this is still part of the SIM
	 * initialisation but no order is specified for it.
	 */
	ofono_sim_read(sim, SIM_EF_CPHS_INFORMATION_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_cphs_information_read_cb, sim);

	/* Also retrieve the GSM service table */
	if (sim->phase >= OFONO_SIM_PHASE_3G)
		ofono_sim_read(sim, SIM_EFUST_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				sim_efust_read_cb, sim);
	else
		ofono_sim_read(sim, SIM_EFSST_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				sim_efsst_read_cb, sim);
}

static void sim_pin_query_cb(const struct ofono_error *error,
				enum ofono_sim_password_type pin_type,
				void *data)
{
	struct ofono_sim *sim = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	const char *pin_name;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Querying PIN authentication state failed");

		goto checkdone;
	}

	if (sim->pin_type != pin_type) {
		sim->pin_type = pin_type;
		pin_name = sim_passwd_name(pin_type);

		if (pin_type != OFONO_SIM_PASSWORD_NONE &&
				password_is_pin(pin_type) == FALSE)
			pin_type = puk2pin(pin_type);

		if (pin_type != OFONO_SIM_PASSWORD_INVALID)
			sim->locked_pins[pin_type] = TRUE;

		ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"PinRequired", DBUS_TYPE_STRING,
						&pin_name);
	}

checkdone:
	if (pin_type == OFONO_SIM_PASSWORD_NONE)
		sim_initialize_after_pin(sim);
}

static void sim_pin_check(struct ofono_sim *sim)
{
	if (!sim->driver->query_passwd_state) {
		sim_initialize_after_pin(sim);
		return;
	}

	sim->driver->query_passwd_state(sim, sim_pin_query_cb, sim);
}

static void sim_efli_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (!ok)
		return;

	sim->efli = g_memdup(data, length);
	sim->efli_length = length;
}

/* Detect whether the file is in EFli format, as opposed to 51.011 EFlp */
static gboolean sim_efli_format(const unsigned char *ef, int length)
{
	int i;

	if (length & 1)
		return FALSE;

	for (i = 0; i < length; i += 2) {
		if (ef[i] == 0xff && ef[i+1] == 0xff)
			continue;

		/*
		 * ISO 639 country codes are each two lower-case SMS 7-bit
		 * characters while CB DCS language codes are in ranges
		 * (0 - 15) or (32 - 47), so the ranges don't overlap
		 */
		if (g_ascii_isalpha(ef[i]) == 0)
			return FALSE;

		if (g_ascii_isalpha(ef[i+1]) == 0)
			return FALSE;
	}

	return TRUE;
}

static GSList *parse_language_list(const unsigned char *ef, int length)
{
	int i;
	GSList *ret = NULL;

	for (i = 0; i < length; i += 2) {
		if (ef[i] > 0x7f || ef[i+1] > 0x7f)
			continue;

		/*
		 * ISO 639 codes contain only characters that are coded
		 * identically in SMS 7 bit charset, ASCII or UTF8 so
		 * no conversion.
		 */
		ret = g_slist_prepend(ret, g_ascii_strdown((char *)ef + i, 2));
	}

	if (ret)
		ret = g_slist_reverse(ret);

	return ret;
}

static GSList *parse_eflp(const unsigned char *eflp, int length)
{
	int i;
	char code[3];
	GSList *ret = NULL;

	for (i = 0; i < length; i++) {
		if (iso639_2_from_language(eflp[i], code) == FALSE)
			continue;

		ret = g_slist_prepend(ret, g_strdup(code));
	}

	if (ret)
		ret = g_slist_reverse(ret);

	return ret;
}

static char **concat_lang_prefs(GSList *a, GSList *b)
{
	GSList *l, *k;
	char **ret;
	int i = 0;
	int total = g_slist_length(a) + g_slist_length(b);

	if (total == 0)
		return NULL;

	ret = g_new0(char *, total + 1);

	for (l = a; l; l = l->next)
		ret[i++] = g_strdup(l->data);

	for (l = b; l; l = l->next) {
		gboolean duplicate = FALSE;

		for (k = a; k; k = k->next)
			if (!strcmp(k->data, l->data))
				duplicate = TRUE;

		if (duplicate)
			continue;

		ret[i++] = g_strdup(l->data);
	}

	return ret;
}

static void sim_efpl_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	const char *path = __ofono_atom_get_path(sim->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	gboolean efli_format = TRUE;
	GSList *efli = NULL;
	GSList *efpl = NULL;

	if (!ok || length < 2)
		goto skip_efpl;

	efpl = parse_language_list(data, length);

skip_efpl:
	if (sim->efli && sim->efli_length > 0) {
		efli_format = sim_efli_format(sim->efli, sim->efli_length);

		if (efli_format)
			efli = parse_language_list(sim->efli, sim->efli_length);
		else
			efli = parse_eflp(sim->efli, sim->efli_length);
	}

	/*
	 * If efli_format is TRUE, make a list of languages in both files in
	 * order of preference following TS 31.102.
	 * Quoting 31.102 Section 5.1.1.2:
	 * The preferred language selection shall always use the EFLI in
	 * preference to the EFPL at the MF unless:
	 * - if the EFLI has the value 'FFFF' in its highest priority position,
	 *   then the preferred language selection shall be the language
	 *   preference in the EFPL at the MF level
	 * Otherwise in order of preference according to TS 51.011
	 */
	if (efli_format) {
		if (sim->efli_length >= 2 && sim->efli[0] == 0xff &&
				sim->efli[1] == 0xff)
			sim->language_prefs = concat_lang_prefs(NULL, efpl);
		else
			sim->language_prefs = concat_lang_prefs(efli, efpl);
	} else {
		sim->language_prefs = concat_lang_prefs(efpl, efli);
	}

	if (sim->efli) {
		g_free(sim->efli);
		sim->efli = NULL;
		sim->efli_length = 0;
	}

	if (efli) {
		g_slist_foreach(efli, (GFunc)g_free, NULL);
		g_slist_free(efli);
	}

	if (efpl) {
		g_slist_foreach(efpl, (GFunc)g_free, NULL);
		g_slist_free(efpl);
	}

	if (sim->language_prefs != NULL)
		ofono_dbus_signal_array_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"PreferredLanguages",
						DBUS_TYPE_STRING,
						&sim->language_prefs);

	sim_pin_check(sim);
}

static void sim_iccid_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	const char *path = __ofono_atom_get_path(sim->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	char iccid[21]; /* ICCID max length is 20 + 1 for NULL */

	if (!ok || length < 10)
		return;

	extract_bcd_number(data, length, iccid);
	iccid[20] = '\0';
	sim->iccid = g_strdup(iccid);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"CardIdentifier",
						DBUS_TYPE_STRING,
						&sim->iccid);
}

static void sim_initialize(struct ofono_sim *sim)
{
	/*
	 * Perform SIM initialization according to 3GPP 31.102 Section 5.1.1.2
	 * The assumption here is that if sim manager is being initialized,
	 * then sim commands are implemented, and the sim manager is then
	 * responsible for checking the PIN, reading the IMSI and signaling
	 * SIM ready condition.
	 *
	 * The procedure according to 31.102, 51.011, 11.11 and CPHS 4.2 is
	 * roughly:
	 *
	 * Read EFecc
	 * Read EFli and EFpl
	 * SIM Pin check
	 * Request SIM phase (only in 51.011)
	 * Administrative information request (read EFad)
	 * Request CPHS Information (only in CPHS 4.2)
	 * Read EFsst (only in 11.11 & 51.011)
	 * Read EFust (only in 31.102)
	 * Read EFest (only in 31.102)
	 * Read IMSI
	 *
	 * At this point we signal the SIM ready condition and allow
	 * arbitrary files to be written or read, assuming their presence
	 * in the EFust
	 */

	/* Grab the EFiccid which is always available */
	ofono_sim_read(sim, SIM_EF_ICCID_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_iccid_read_cb, sim);

	/* EFecc is read by the voicecall atom */

	/*
	 * According to 31.102 the EFli is read first and EFpl is then
	 * only read if none of the EFli languages are supported by user
	 * interface.  51.011 mandates the exact opposite, making EFpl/EFelp
	 * preferred over EFlp (same EFid as EFli, different format).
	 * However we don't depend on the user interface and so
	 * need to read both files now.
	 */
	ofono_sim_read(sim, SIM_EFLI_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efli_read_cb, sim);
	ofono_sim_read(sim, SIM_EFPL_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efpl_read_cb, sim);
}

int ofono_sim_read_bytes(struct ofono_sim *sim, int id,
			unsigned short offset, unsigned short num_bytes,
			ofono_sim_file_read_cb_t cb, void *data)
{
	if (sim == NULL)
		return -1;

	if (num_bytes == 0)
		return -1;

	return sim_fs_read(sim->simfs, id, OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				offset, num_bytes, cb, data);
}

int ofono_sim_read(struct ofono_sim *sim, int id,
			enum ofono_sim_file_structure expected_type,
			ofono_sim_file_read_cb_t cb, void *data)
{
	if (sim == NULL)
		return -1;

	return sim_fs_read(sim->simfs, id, expected_type, 0, 0, cb, data);
}

int ofono_sim_write(struct ofono_sim *sim, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata)
{
	if (sim == NULL)
		return -1;

	return sim_fs_write(sim->simfs, id, cb, structure, record, data, length,
				userdata);
}

const char *ofono_sim_get_imsi(struct ofono_sim *sim)
{
	if (sim == NULL)
		return NULL;

	return sim->imsi;
}

enum ofono_sim_phase ofono_sim_get_phase(struct ofono_sim *sim)
{
	if (sim == NULL)
		return 0;

	return sim->phase;
}

enum ofono_sim_cphs_phase ofono_sim_get_cphs_phase(struct ofono_sim *sim)
{
	if (sim == NULL)
		return OFONO_SIM_CPHS_PHASE_NONE;

	return sim->cphs_phase;
}

const unsigned char *ofono_sim_get_cphs_service_table(struct ofono_sim *sim)
{
	if (sim == NULL)
		return NULL;

	return sim->cphs_service_table;
}

static void sim_inserted_update(struct ofono_sim *sim)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	dbus_bool_t present = sim->state != OFONO_SIM_STATE_NOT_PRESENT;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"Present",
						DBUS_TYPE_BOOLEAN, &present);
}

static void sim_free_state(struct ofono_sim *sim)
{
	if (sim->simfs) {
		sim_fs_free(sim->simfs);
		sim->simfs = NULL;
	}

	if (sim->iccid) {
		g_free(sim->iccid);
		sim->iccid = NULL;
	}

	if (sim->imsi) {
		g_free(sim->imsi);
		sim->imsi = NULL;
	}

	if (sim->own_numbers) {
		g_slist_foreach(sim->own_numbers, (GFunc)g_free, NULL);
		g_slist_free(sim->own_numbers);
		sim->own_numbers = NULL;
	}

	if (sim->service_numbers) {
		g_slist_foreach(sim->service_numbers,
				(GFunc)service_number_free, NULL);
		g_slist_free(sim->service_numbers);
		sim->service_numbers = NULL;
	}

	if (sim->efli) {
		g_free(sim->efli);
		sim->efli = NULL;
		sim->efli_length = 0;
	}

	if (sim->language_prefs) {
		g_strfreev(sim->language_prefs);
		sim->language_prefs = NULL;
	}

	if (sim->efust) {
		g_free(sim->efust);
		sim->efust = NULL;
		sim->efust_length = 0;
	}

	if (sim->efest) {
		g_free(sim->efest);
		sim->efest = NULL;
		sim->efest_length = 0;
	}

	if (sim->efsst) {
		g_free(sim->efsst);
		sim->efsst = NULL;
		sim->efsst_length = 0;
	}

	sim->mnc_length = 0;

	if (sim->efimg) {
		g_free(sim->efimg);
		sim->efimg = NULL;
		sim->efimg_length = 0;
	}
}

void ofono_sim_inserted_notify(struct ofono_sim *sim, ofono_bool_t inserted)
{
	ofono_sim_state_event_cb_t notify;
	GSList *l;

	if (inserted == TRUE && sim->state == OFONO_SIM_STATE_NOT_PRESENT)
		sim->state = OFONO_SIM_STATE_INSERTED;
	else if (inserted == FALSE && sim->state != OFONO_SIM_STATE_NOT_PRESENT)
		sim->state = OFONO_SIM_STATE_NOT_PRESENT;
	else
		return;

	if (!__ofono_atom_get_registered(sim->atom))
		return;

	sim_inserted_update(sim);

	for (l = sim->state_watches->items; l; l = l->next) {
		struct ofono_watchlist_item *item = l->data;
		notify = item->notify;

		notify(sim->state, item->notify_data);
	}

	if (inserted)
		sim_initialize(sim);
	else
		sim_free_state(sim);
}

unsigned int ofono_sim_add_state_watch(struct ofono_sim *sim,
					ofono_sim_state_event_cb_t notify,
					void *data, ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *item;

	DBG("%p", sim);

	if (sim == NULL)
		return 0;

	if (notify == NULL)
		return 0;

	item = g_new0(struct ofono_watchlist_item, 1);

	item->notify = notify;
	item->destroy = destroy;
	item->notify_data = data;

	return __ofono_watchlist_add_item(sim->state_watches, item);
}

void ofono_sim_remove_state_watch(struct ofono_sim *sim, unsigned int id)
{
	__ofono_watchlist_remove_item(sim->state_watches, id);
}

enum ofono_sim_state ofono_sim_get_state(struct ofono_sim *sim)
{
	if (sim == NULL)
		return OFONO_SIM_STATE_NOT_PRESENT;

	return sim->state;
}

static void sim_set_ready(struct ofono_sim *sim)
{
	GSList *l;
	ofono_sim_state_event_cb_t notify;

	if (sim == NULL)
		return;

	if (sim->state != OFONO_SIM_STATE_INSERTED)
		return;

	sim->state = OFONO_SIM_STATE_READY;

	sim_fs_check_version(sim->simfs);

	for (l = sim->state_watches->items; l; l = l->next) {
		struct ofono_watchlist_item *item = l->data;
		notify = item->notify;

		notify(sim->state, item->notify_data);
	}
}

int ofono_sim_driver_register(const struct ofono_sim_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_sim_driver_unregister(const struct ofono_sim_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void sim_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);
	struct ofono_sim *sim = __ofono_atom_get_data(atom);

	__ofono_watchlist_free(sim->state_watches);
	sim->state_watches = NULL;

	g_dbus_unregister_interface(conn, path, OFONO_SIM_MANAGER_INTERFACE);
	ofono_modem_remove_interface(modem, OFONO_SIM_MANAGER_INTERFACE);
}

static void sim_remove(struct ofono_atom *atom)
{
	struct ofono_sim *sim = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (sim == NULL)
		return;

	if (sim->driver && sim->driver->remove)
		sim->driver->remove(sim);

	sim_free_state(sim);

	g_free(sim);
}

struct ofono_sim *ofono_sim_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_sim *sim;
	GSList *l;

	if (driver == NULL)
		return NULL;

	sim = g_try_new0(struct ofono_sim, 1);

	if (sim == NULL)
		return NULL;

	sim->phase = OFONO_SIM_PHASE_UNKNOWN;
	sim->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_SIM,
						sim_remove, sim);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_sim_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(sim, vendor, data) < 0)
			continue;

		sim->driver = drv;
		break;
	}

	return sim;
}

void ofono_sim_register(struct ofono_sim *sim)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(sim->atom);
	const char *path = __ofono_atom_get_path(sim->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_SIM_MANAGER_INTERFACE,
					sim_methods, sim_signals, NULL,
					sim, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_SIM_MANAGER_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_SIM_MANAGER_INTERFACE);
	sim->state_watches = __ofono_watchlist_new(g_free);
	sim->simfs = sim_fs_new(sim, sim->driver);

	__ofono_atom_register(sim->atom, sim_unregister);

	ofono_sim_add_state_watch(sim, sim_ready, sim, NULL);

	if (sim->state > OFONO_SIM_STATE_NOT_PRESENT)
		sim_initialize(sim);
}

void ofono_sim_remove(struct ofono_sim *sim)
{
	__ofono_atom_free(sim->atom);
}

void ofono_sim_set_data(struct ofono_sim *sim, void *data)
{
	sim->driver_data = data;
}

void *ofono_sim_get_data(struct ofono_sim *sim)
{
	return sim->driver_data;
}
