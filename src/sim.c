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

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdint.h>

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
#include "stkutil.h"

#define SIM_FLAG_READING_SPN	0x1

struct ofono_sim {
	int flags;

	/* Contents of the SIM file system, in rough initialization order */
	char *iccid;

	char **language_prefs;
	unsigned char *efli;
	unsigned char efli_length;
	gboolean language_prefs_update;

	enum ofono_sim_password_type pin_type;
	gboolean locked_pins[OFONO_SIM_PASSWORD_SIM_PUK]; /* Number of PINs */

	int pin_retries[OFONO_SIM_PASSWORD_INVALID];

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
	gboolean fixed_dialing;
	gboolean barred_dialing;

	char *imsi;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];

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

	char *spn;
	char *spn_dc;
	struct ofono_watchlist *spn_watches;
	unsigned int ef_spn_watch;
	unsigned int cphs_spn_watch;
	unsigned int cphs_spn_short_watch;

	struct sim_fs *simfs;
	struct ofono_sim_context *context;
	struct ofono_sim_context *early_context;

	unsigned char *iidf_image;
	unsigned int *iidf_watch_ids;

	DBusMessage *pending;
	const struct ofono_sim_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	unsigned int hfp_watch;
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

static void sim_own_numbers_update(struct ofono_sim *sim);

static GSList *g_drivers = NULL;

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
		return OFONO_SIM_PASSWORD_PHNET_PIN;
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

static void **get_pin_retries(struct ofono_sim *sim)
{
	int i, nelem;
	void **ret;

	for (i = 1, nelem = 0; i < OFONO_SIM_PASSWORD_INVALID; i++) {
		if (sim->pin_retries[i] == -1)
			continue;

		nelem += 1;
	}

	ret = g_new0(void *, nelem * 2 + 1);

	for (i = 1, nelem = 0; i < OFONO_SIM_PASSWORD_INVALID; i++) {
		if (sim->pin_retries[i] == -1)
			continue;

		ret[nelem++] = (void *) sim_passwd_name(i);
		ret[nelem++] = &sim->pin_retries[i];
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

static void call_state_watches(struct ofono_sim *sim)
{
	GSList *l;
	ofono_sim_state_event_cb_t notify;

	for (l = sim->state_watches->items; l; l = l->next) {
		struct ofono_watchlist_item *item = l->data;
		notify = item->notify;

		notify(sim->state, item->notify_data);
	}
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
	void **pin_retries;
	dbus_bool_t present = sim->state != OFONO_SIM_STATE_NOT_PRESENT;
	dbus_bool_t fdn;
	dbus_bool_t bdn;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
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

	fdn = sim->fixed_dialing;
	ofono_dbus_dict_append(&dict, "FixedDialing", DBUS_TYPE_BOOLEAN, &fdn);

	bdn = sim->barred_dialing;
	ofono_dbus_dict_append(&dict, "BarredDialing", DBUS_TYPE_BOOLEAN, &bdn);

	if (sim->mcc[0] != '\0' && sim->mnc[0] != '\0') {
		const char *str;
		str = sim->mcc;
		ofono_dbus_dict_append(&dict, "MobileCountryCode",
					DBUS_TYPE_STRING, &str);

		str = sim->mnc;
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

	pin_retries = get_pin_retries(sim);
	ofono_dbus_dict_append_dict(&dict, "Retries", DBUS_TYPE_BYTE,
								&pin_retries);
	g_free(pin_retries);

done:
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void sim_pin_retries_query_cb(const struct ofono_error *error,
					int retries[OFONO_SIM_PASSWORD_INVALID],
					void *data)
{
	struct ofono_sim *sim = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	void **pin_retries;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Querying remaining pin retries failed");
		return;
	}

	if (!memcmp(retries, sim->pin_retries, sizeof(sim->pin_retries)))
		return;

	memcpy(sim->pin_retries, retries, sizeof(sim->pin_retries));

	pin_retries = get_pin_retries(sim);
	ofono_dbus_signal_dict_property_changed(conn, path,
					OFONO_SIM_MANAGER_INTERFACE, "Retries",
					DBUS_TYPE_BYTE,	&pin_retries);
	g_free(pin_retries);
}

static void sim_pin_retries_check(struct ofono_sim *sim)
{
	if (sim->driver->query_pin_retries == NULL)
		return;

	sim->driver->query_pin_retries(sim, sim_pin_retries_query_cb, sim);
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
			/* Set number length */
			efmsisdn[sim->efmsisdn_length - 14] = 1;
		}

		if (ofono_sim_write(req->sim->context, SIM_EFMSISDN_FILEID,
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

	sim_pin_retries_check(sim);
}

static void sim_unlock_cb(const struct ofono_error *error, void *data)
{
	struct ofono_sim *sim = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBusMessage *reply = __ofono_error_failed(sim->pending);

		__ofono_dbus_pending_reply(&sim->pending, reply);
		__ofono_sim_recheck_pin(sim);

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
		__ofono_sim_recheck_pin(sim);

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

	if (sim->driver->lock == NULL)
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

	if (!__ofono_is_valid_sim_pin(pin, type))
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

		__ofono_sim_recheck_pin(sim);

		return;
	}

	__ofono_dbus_pending_reply(&sim->pending,
				dbus_message_new_method_return(sim->pending));

	sim_pin_retries_check(sim);
}

static DBusMessage *sim_change_pin(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;
	enum ofono_sim_password_type type;
	const char *typestr;
	const char *old;
	const char *new;

	if (sim->driver->change_passwd == NULL)
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

	if (!__ofono_is_valid_sim_pin(old, type))
		return __ofono_error_invalid_format(msg);

	if (!__ofono_is_valid_sim_pin(new, type))
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

	__ofono_sim_recheck_pin(sim);
}

static DBusMessage *sim_enter_pin(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_sim *sim = data;
	const char *typestr;
	enum ofono_sim_password_type type;
	const char *pin;

	if (sim->driver->send_passwd == NULL)
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

	if (password_is_pin(type) == FALSE)
		return __ofono_error_invalid_format(msg);

	if (!__ofono_is_valid_sim_pin(pin, type))
		return __ofono_error_invalid_format(msg);

	sim->pending = dbus_message_ref(msg);
	sim->driver->send_passwd(sim, pin, sim_enter_pin_cb, sim);

	return NULL;
}

static void sim_get_image_cb(struct ofono_sim *sim,
				unsigned char id, char *xpm, gboolean cache)
{
	DBusMessage *reply;
	DBusMessageIter iter, array;
	int xpm_len;

	if (xpm == NULL) {
		reply = __ofono_error_failed(sim->pending);
		__ofono_dbus_pending_reply(&sim->pending, reply);
		return;
	}

	xpm_len = strlen(xpm);

	reply = dbus_message_new_method_return(sim->pending);
	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);

	dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
						&xpm, xpm_len);
	dbus_message_iter_close_container(&iter, &array);

	__ofono_dbus_pending_reply(&sim->pending, reply);

	if (cache)
		sim_fs_cache_image(sim->simfs, (const char *) xpm, id);

	g_free(xpm);
}

static void sim_iidf_read_clut_cb(int ok, int length, int record,
					const unsigned char *data,
					int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	unsigned char id;
	unsigned char *efimg;
	unsigned short iidf_len;
	unsigned short clut_len;
	char *xpm;

	DBG("ok: %d", ok);

	dbus_message_get_args(sim->pending, NULL, DBUS_TYPE_BYTE, &id,
					DBUS_TYPE_INVALID);
	id -= 1;
	efimg = &sim->efimg[id * 9];

	if (!ok) {
		sim_get_image_cb(sim, id, NULL, FALSE);
		goto done;
	}

	iidf_len = efimg[7] << 8 | efimg[8];

	if (sim->iidf_image[3] == 0)
		clut_len = 256 * 3;
	else
		clut_len = sim->iidf_image[3] * 3;

	xpm = stk_image_to_xpm(sim->iidf_image, iidf_len, efimg[2],
					data, clut_len);
	sim_get_image_cb(sim, id, xpm, TRUE);

done:
	g_free(sim->iidf_image);
	sim->iidf_image = NULL;
}

static void sim_iidf_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	unsigned char id;
	unsigned char *efimg;
	unsigned short iidf_id;
	unsigned short offset;
	unsigned short clut_len;

	DBG("ok: %d", ok);

	dbus_message_get_args(sim->pending, NULL, DBUS_TYPE_BYTE, &id,
					DBUS_TYPE_INVALID);
	id -= 1;
	efimg = &sim->efimg[id * 9];

	if (!ok) {
		sim_get_image_cb(sim, id, NULL, FALSE);
		return;
	}

	if (efimg[2] == STK_IMG_SCHEME_BASIC) {
		char *xpm = stk_image_to_xpm(data, length, efimg[2], NULL, 0);
		sim_get_image_cb(sim, id, xpm, TRUE);
		return;
	}

	offset = data[4] << 8 | data[5];

	if (data[3] == 0)
		clut_len = 256 * 3;
	else
		clut_len = data[3] * 3;

	iidf_id = efimg[3] << 8 | efimg[4];
	sim->iidf_image = g_memdup(data, length);

	/* read the clut data */
	ofono_sim_read_bytes(sim->context, iidf_id, offset, clut_len,
					sim_iidf_read_clut_cb, sim);
}

static void sim_image_data_changed(int id, void *userdata)
{
	/* TODO: notify D-bus clients */
}

static void sim_get_image(struct ofono_sim *sim, unsigned char id,
				gpointer user_data)
{
	unsigned char *efimg;
	char *image;
	unsigned short iidf_id;
	unsigned short iidf_offset;
	unsigned short iidf_len;

	if (sim->efimg_length <= id * 9) {
		sim_get_image_cb(sim, id, NULL, FALSE);
		return;
	}

	image = sim_fs_get_cached_image(sim->simfs, id);
	if (image != NULL)
		sim_get_image_cb(sim, id, image, FALSE);

	efimg = &sim->efimg[id * 9];

	iidf_id = efimg[3] << 8 | efimg[4];
	iidf_offset = efimg[5] << 8 | efimg[6];
	iidf_len = efimg[7] << 8 | efimg[8];

	/* read the image data */
	if (image == NULL)
		ofono_sim_read_bytes(sim->context, iidf_id, iidf_offset,
					iidf_len, sim_iidf_read_cb, sim);

	if (sim->iidf_watch_ids[id] > 0)
		return;

	sim->iidf_watch_ids[id] = ofono_sim_add_file_watch(sim->context,
					iidf_id, sim_image_data_changed,
					sim, NULL);
}

static DBusMessage *sim_get_icon(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_sim *sim = data;
	unsigned char id;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_BYTE, &id,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	/* zero means no icon */
	if (id == 0)
		return __ofono_error_invalid_args(msg);

	if (sim->pending)
		return __ofono_error_busy(msg);

	if (sim->efimg == NULL)
		return __ofono_error_not_implemented(msg);

	sim->pending = dbus_message_ref(msg);

	sim_get_image(sim, id - 1, sim);

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

	if (sim->driver->reset_passwd == NULL)
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

	if (!__ofono_is_valid_sim_pin(puk, type))
		return __ofono_error_invalid_format(msg);

	type = puk2pin(type);

	if (!__ofono_is_valid_sim_pin(pin, type))
		return __ofono_error_invalid_format(msg);

	sim->pending = dbus_message_ref(msg);
	sim->driver->reset_passwd(sim, puk, pin, sim_enter_pin_cb, sim);

	return NULL;
}

static const GDBusMethodTable sim_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			sim_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, sim_set_property) },
	{ GDBUS_ASYNC_METHOD("ChangePin",
			GDBUS_ARGS({ "type", "s" }, { "oldpin", "s" },
						{ "newpin", "s" }), NULL,
			sim_change_pin) },
	{ GDBUS_ASYNC_METHOD("EnterPin",
			GDBUS_ARGS({ "type", "s" }, { "pin", "s" }), NULL,
			sim_enter_pin) },
	{ GDBUS_ASYNC_METHOD("ResetPin",
			GDBUS_ARGS({ "type", "s" }, { "puk", "s" },
						{ "newpin", "s" }), NULL,
			sim_reset_pin) },
	{ GDBUS_ASYNC_METHOD("LockPin",
			GDBUS_ARGS({ "type", "s" }, { "pin", "s" }), NULL,
			sim_lock_pin) },
	{ GDBUS_ASYNC_METHOD("UnlockPin",
			GDBUS_ARGS({ "type", "s" }, { "pin", "s" }), NULL,
			sim_unlock_pin) },
	{ GDBUS_ASYNC_METHOD("GetIcon",
			GDBUS_ARGS({ "id", "y" }),
			GDBUS_ARGS({ "icon", "ay" }),
			sim_get_icon) },
	{ }
};

static const GDBusSignalTable sim_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static gboolean numbers_list_equal(GSList *a, GSList *b)
{
	struct ofono_phone_number *num_a, *num_b;

	while (a || b) {
		if (a == NULL || b == NULL)
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

	if (record_length < 14 || length < record_length) {
		ofono_error("EFmsidn shall at least contain 14 bytes");
		return;
	}

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
		ofono_error("Duplicate EFsdn entries for `%s'",
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
		sim->service_numbers = g_slist_reverse(sim->service_numbers);
		sim->sdn_ready = TRUE;
	}

	if (sim->sdn_ready) {
		char **service_numbers;

		service_numbers = get_service_numbers(sim->service_numbers);

		ofono_dbus_signal_dict_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"ServiceNumbers",
						DBUS_TYPE_STRING,
						&service_numbers);
		g_strfreev(service_numbers);
	}
}

static void sim_service_numbers_changed(int id, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (sim->service_numbers) {
		g_slist_foreach(sim->service_numbers,
				(GFunc)service_number_free, NULL);
		g_slist_free(sim->service_numbers);
		sim->service_numbers = NULL;
	}

	ofono_sim_read(sim->context, SIM_EFSDN_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED, sim_sdn_read_cb, sim);
}

static void sim_own_numbers_update(struct ofono_sim *sim)
{
	ofono_sim_read(sim->context, SIM_EFMSISDN_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED, sim_msisdn_read_cb,
			sim);
}

static void sim_own_numbers_changed(int id, void *userdata)
{
	struct ofono_sim *sim = userdata;

	sim_own_numbers_update(sim);
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

		sim->iidf_watch_ids = g_try_new0(unsigned int, num_records);
		if (sim->iidf_watch_ids == NULL) {
			g_free(sim->efimg);
			sim->efimg = NULL;
			return;
		}

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

static void sim_efimg_changed(int id, void *userdata)
{
	struct ofono_sim *sim = userdata;
	int i, watch;

	if (sim->efimg != NULL) {
		for (i = sim->efimg_length / 9 - 1; i >= 0; i--) {
			watch = sim->iidf_watch_ids[i];
			if (watch == 0)
				continue;

			ofono_sim_remove_file_watch(sim->context, watch);
		}

		g_free(sim->efimg);
		sim->efimg = NULL;
		sim->efimg_length = 0;
		g_free(sim->iidf_watch_ids);
		sim->iidf_watch_ids = NULL;
	}

	ofono_sim_read(sim->context, SIM_EFIMG_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED, sim_efimg_read_cb, sim);

	/* TODO: notify D-bus clients */
}

static void sim_ready(enum ofono_sim_state new_state, void *user)
{
	struct ofono_sim *sim = user;

	if (new_state != OFONO_SIM_STATE_READY)
		return;

	sim_own_numbers_update(sim);
	ofono_sim_add_file_watch(sim->context, SIM_EFMSISDN_FILEID,
					sim_own_numbers_changed, sim, NULL);

	ofono_sim_read(sim->context, SIM_EFSDN_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED, sim_sdn_read_cb, sim);
	ofono_sim_add_file_watch(sim->context, SIM_EFSDN_FILEID,
					sim_service_numbers_changed, sim, NULL);

	ofono_sim_read(sim->context, SIM_EFIMG_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED, sim_efimg_read_cb, sim);
	ofono_sim_add_file_watch(sim->context, SIM_EFIMG_FILEID,
					sim_efimg_changed, sim, NULL);
}

static void sim_set_ready(struct ofono_sim *sim)
{
	if (sim == NULL)
		return;

	if (sim->state != OFONO_SIM_STATE_INSERTED &&
			sim->state != OFONO_SIM_STATE_LOCKED_OUT)
		return;

	sim->state = OFONO_SIM_STATE_READY;

	sim_fs_check_version(sim->simfs);

	call_state_watches(sim);
}

static void sim_imsi_obtained(struct ofono_sim *sim, const char *imsi)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);

	sim->imsi = g_strdup(imsi);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"SubscriberIdentity",
						DBUS_TYPE_STRING, &sim->imsi);

	if (sim->mnc_length) {
		const char *str;

		strncpy(sim->mcc, sim->imsi, OFONO_MAX_MCC_LENGTH);
		sim->mcc[OFONO_MAX_MCC_LENGTH] = '\0';
		strncpy(sim->mnc, sim->imsi + OFONO_MAX_MCC_LENGTH,
			sim->mnc_length);
		sim->mnc[sim->mnc_length] = '\0';

		str = sim->mcc;
		ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"MobileCountryCode",
						DBUS_TYPE_STRING, &str);

		str = sim->mnc;
		ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"MobileNetworkCode",
						DBUS_TYPE_STRING, &str);
	}

	sim_set_ready(sim);

}

static void sim_imsi_cb(const struct ofono_error *error, const char *imsi,
			void *data)
{
	struct ofono_sim *sim = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Unable to read IMSI, emergency calls only");
		return;
	}

	sim_imsi_obtained(sim, imsi);
}

static void sim_efimsi_cb(const struct ofono_error *error,
				const unsigned char *data, int len, void *user)
{
	struct ofono_sim *sim = user;
	char imsi[17]; /* IMSI max length is 15 + 1 for NULL + 1 waste */
	unsigned char imsi_len;
	unsigned char parity;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto error;

	if (len != 9)
		goto error;

	imsi_len = data[0];

	if (imsi_len == 0 || imsi_len > 8)
		goto error;

	/* The low 3 bits of the first byte should be set to binary 001 */
	if ((data[1] & 0x7) != 0x1)
		goto error;

	/* Save off the parity bit */
	parity = (data[1] >> 3) & 1;

	extract_bcd_number(data + 1, imsi_len, imsi);
	imsi[16] = '\0';

	if ((strlen(imsi + 1) % 2) != parity)
		goto error;

	sim_imsi_obtained(sim, imsi + 1);
	return;

error:
	ofono_error("Unable to read IMSI, emergency calls only");
}

static void sim_retrieve_imsi(struct ofono_sim *sim)
{
	if (sim->driver->read_imsi) {
		sim->driver->read_imsi(sim, sim_imsi_cb, sim);
		return;
	}

	if (sim->driver->read_file_transparent == NULL) {
		ofono_error("IMSI retrieval not implemented,"
			" only emergency calls will be available");
		return;
	}

	sim->driver->read_file_transparent(sim, SIM_EFIMSI_FILEID, 0, 9,
						sim_efimsi_cb, sim);
}

static void sim_fdn_enabled(struct ofono_sim *sim)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	dbus_bool_t val;

	sim->fixed_dialing = TRUE;

	val = sim->fixed_dialing;
	ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"FixedDialing",
						DBUS_TYPE_BOOLEAN, &val);
}

static void sim_bdn_enabled(struct ofono_sim *sim)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	dbus_bool_t val;

	sim->barred_dialing = TRUE;

	val = sim->barred_dialing;
	ofono_dbus_signal_property_changed(conn, path,
						OFONO_SIM_MANAGER_INTERFACE,
						"BarredDialing",
						DBUS_TYPE_BOOLEAN, &val);
}

static void sim_efbdn_info_read_cb(int ok, unsigned char file_status,
					int total_length, int record_length,
					void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (!ok)
		goto out;

	if (file_status & SIM_FILE_STATUS_VALID)
		sim_bdn_enabled(sim);

out:
	if (sim->fixed_dialing != TRUE &&
			sim->barred_dialing != TRUE)
		sim_retrieve_imsi(sim);
}

static gboolean check_bdn_status(struct ofono_sim *sim)
{
	/*
	 * Check the status of Barred Dialing in the SIM-card
	 * (TS 11.11/TS 51.011, Section 11.5.1: BDN capability request).
	 * If BDN is allocated, activated in EFsst and EFbdn is validated,
	 * halt the SIM initialization.
	 */
	if (sim_sst_is_active(sim->efsst, sim->efsst_length,
			SIM_SST_SERVICE_BDN)) {
		sim_fs_read_info(sim->context, SIM_EFBDN_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				sim_efbdn_info_read_cb, sim);
		return TRUE;
	}

	return FALSE;
}

static void sim_efadn_info_read_cb(int ok, unsigned char file_status,
					int total_length, int record_length,
					void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (!ok)
		goto out;

	if (!(file_status & SIM_FILE_STATUS_VALID))
		sim_fdn_enabled(sim);

out:
	if (check_bdn_status(sim) != TRUE) {
		if (sim->fixed_dialing != TRUE &&
				sim->barred_dialing != TRUE)
			sim_retrieve_imsi(sim);
	}
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

	/*
	 * Check if Fixed Dialing is enabled in the SIM-card
	 * (TS 11.11/TS 51.011, Section 11.5.1: FDN capability request).
	 * If FDN is activated and ADN is invalidated,
	 * don't continue initialization routine.
	 */
	if (sim_sst_is_active(sim->efsst, sim->efsst_length,
				SIM_SST_SERVICE_FDN)) {
		sim_fs_read_info(sim->context, SIM_EFADN_FILEID,
					OFONO_SIM_FILE_STRUCTURE_FIXED,
					sim_efadn_info_read_cb, sim);
		return;
	}

	if (check_bdn_status(sim) == TRUE)
		return;

out:
	sim_retrieve_imsi(sim);
}

static void sim_efest_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	gboolean available;

	if (!ok)
		goto out;

	if (length < 1) {
		ofono_error("EFest shall contain at least one byte");
		goto out;
	}

	sim->efest = g_memdup(data, length);
	sim->efest_length = length;

	/*
	 * Check if Fixed Dialing is enabled in the USIM-card
	 * (TS 31.102, Section 5.3.2: FDN capability request).
	 * If FDN is activated, don't continue initialization routine.
	 */
	available = sim_ust_is_available(sim->efust, sim->efust_length,
						SIM_UST_SERVICE_FDN);
	if (available && sim_est_is_active(sim->efest, sim->efest_length,
						SIM_EST_SERVICE_FDN))
		sim_fdn_enabled(sim);

	/*
	 * Check the status of Barred Dialing in the USIM-card
	 * (TS 31.102, Section 5.3.2: BDN capability request).
	 * If BDN service is enabled, halt the USIM initialization.
	 */
	available = sim_ust_is_available(sim->efust, sim->efust_length,
						SIM_UST_SERVICE_BDN);
	if (available && sim_est_is_active(sim->efest, sim->efest_length,
						SIM_EST_SERVICE_BDN))
		sim_bdn_enabled(sim);

out:
	if (sim->fixed_dialing != TRUE &&
			sim->barred_dialing != TRUE)
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

	/*
	 * Check whether the SIM provides EFest file
	 * According to 3GPP TS 31.102 section 4.2.47, EFest file
	 * shall be present if FDN or BDN or EST is available
	 * Lets be paranoid and check for the special cases as well
	 * where EST is not available(FDN or BDN available), but EFest
	 * is present
	 */
	if (sim_ust_is_available(sim->efust, sim->efust_length,
				SIM_UST_SERVICE_ENABLED_SERVICE_TABLE) ||
			sim_ust_is_available(sim->efust, sim->efust_length,
				SIM_UST_SERVICE_FDN) ||
			sim_ust_is_available(sim->efust, sim->efust_length,
				SIM_UST_SERVICE_BDN)) {
		ofono_sim_read(sim->context, SIM_EFEST_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				sim_efest_read_cb, sim);

		return;
	}

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

	if (length < 4) {
		ofono_error("EFad should contain at least four bytes");
		return;
	}

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

	if (!ok || length != 1) {
		sim->phase = OFONO_SIM_PHASE_3G;

		ofono_sim_read(sim->context, SIM_EFUST_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				sim_efust_read_cb, sim);

		return;
	}

	switch (data[0]) {
	case 0:
		sim->phase = OFONO_SIM_PHASE_1G;
		break;
	case 2:
		sim->phase = OFONO_SIM_PHASE_2G;
		break;
	case 3:
		sim->phase = OFONO_SIM_PHASE_2G_PLUS;
		break;
	default:
		ofono_error("Unknown phase");
		return;
	}

	ofono_sim_read(sim->context, SIM_EFSST_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efsst_read_cb, sim);
}

static void sim_initialize_after_pin(struct ofono_sim *sim)
{
	sim->context = ofono_sim_context_create(sim);

	ofono_sim_read(sim->context, SIM_EFPHASE_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efphase_read_cb, sim);

	ofono_sim_read(sim->context, SIM_EFAD_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_ad_read_cb, sim);

	/*
	 * Read CPHS-support bits, this is still part of the SIM
	 * initialisation but no order is specified for it.
	 */
	ofono_sim_read(sim->context, SIM_EF_CPHS_INFORMATION_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_cphs_information_read_cb, sim);
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

	/* Proceed with sim initialization if we're not merely updating */
	if (!sim->language_prefs_update)
		__ofono_sim_recheck_pin(sim);

	sim->language_prefs_update = FALSE;
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

static void sim_iccid_changed(int id, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (sim->iccid) {
		g_free(sim->iccid);
		sim->iccid = NULL;
	}

	ofono_sim_read(sim->early_context, SIM_EF_ICCID_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_iccid_read_cb, sim);
}

static void sim_efli_efpl_changed(int id, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (sim->efli != NULL) /* This shouldn't happen */
		return;

	if (sim->language_prefs) {
		g_strfreev(sim->language_prefs);
		sim->language_prefs = NULL;
	}

	sim->language_prefs_update = TRUE;

	ofono_sim_read(sim->early_context, SIM_EFLI_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efli_read_cb, sim);

	ofono_sim_read(sim->early_context, SIM_EFPL_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efpl_read_cb, sim);
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

	if (sim->early_context == NULL)
		sim->early_context = ofono_sim_context_create(sim);

	/* Grab the EFiccid which is always available */
	ofono_sim_read(sim->early_context, SIM_EF_ICCID_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_iccid_read_cb, sim);
	ofono_sim_add_file_watch(sim->early_context, SIM_EF_ICCID_FILEID,
					sim_iccid_changed, sim, NULL);

	/* EFecc is read by the voicecall atom */

	/*
	 * According to 31.102 the EFli is read first and EFpl is then
	 * only read if none of the EFli languages are supported by user
	 * interface.  51.011 mandates the exact opposite, making EFpl/EFelp
	 * preferred over EFlp (same EFid as EFli, different format).
	 * However we don't depend on the user interface and so
	 * need to read both files now.
	 */
	ofono_sim_read(sim->early_context, SIM_EFLI_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efli_read_cb, sim);
	ofono_sim_add_file_watch(sim->early_context, SIM_EFLI_FILEID,
					sim_efli_efpl_changed, sim, NULL);

	ofono_sim_read(sim->early_context, SIM_EFPL_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_efpl_read_cb, sim);
	ofono_sim_add_file_watch(sim->early_context, SIM_EFPL_FILEID,
					sim_efli_efpl_changed, sim, NULL);
}

struct ofono_sim_context *ofono_sim_context_create(struct ofono_sim *sim)
{
	if (sim == NULL || sim->simfs == NULL)
		return NULL;

	return sim_fs_context_new(sim->simfs);
}

void ofono_sim_context_free(struct ofono_sim_context *context)
{
	return sim_fs_context_free(context);
}

int ofono_sim_read_bytes(struct ofono_sim_context *context, int id,
			unsigned short offset, unsigned short num_bytes,
			ofono_sim_file_read_cb_t cb, void *data)
{
	if (num_bytes == 0)
		return -1;

	return sim_fs_read(context, id, OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				offset, num_bytes, cb, data);
}

int ofono_sim_read(struct ofono_sim_context *context, int id,
			enum ofono_sim_file_structure expected_type,
			ofono_sim_file_read_cb_t cb, void *data)
{
	return sim_fs_read(context, id, expected_type, 0, 0, cb, data);
}

int ofono_sim_write(struct ofono_sim_context *context, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata)
{
	return sim_fs_write(context, id, cb, structure, record, data, length,
				userdata);
}

unsigned int ofono_sim_add_file_watch(struct ofono_sim_context *context,
					int id, ofono_sim_file_changed_cb_t cb,
					void *userdata,
					ofono_destroy_func destroy)
{
	return sim_fs_file_watch_add(context, id, cb, userdata, destroy);
}

void ofono_sim_remove_file_watch(struct ofono_sim_context *context,
					unsigned int id)
{
	sim_fs_file_watch_remove(context, id);
}

const char *ofono_sim_get_imsi(struct ofono_sim *sim)
{
	if (sim == NULL)
		return NULL;

	return sim->imsi;
}

const char *ofono_sim_get_mcc(struct ofono_sim *sim)
{
	if (sim == NULL)
		return NULL;

	return sim->mcc;
}

const char *ofono_sim_get_mnc(struct ofono_sim *sim)
{
	if (sim == NULL)
		return NULL;

	return sim->mnc;
}

const char *ofono_sim_get_spn(struct ofono_sim *sim)
{
	if (sim == NULL)
		return NULL;

	return sim->spn;
}

enum ofono_sim_phase ofono_sim_get_phase(struct ofono_sim *sim)
{
	if (sim == NULL)
		return OFONO_SIM_PHASE_UNKNOWN;

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

ofono_bool_t __ofono_sim_service_available(struct ofono_sim *sim,
						int ust_service,
						int sst_service)
{
	if (sim->efust)
		return sim_ust_is_available(sim->efust, sim->efust_length,
						ust_service);

	if (sim->efsst)
		return sim_sst_is_active(sim->efsst, sim->efsst_length,
						sst_service);

	return FALSE;
}

ofono_bool_t __ofono_sim_cphs_service_available(struct ofono_sim *sim,
						int cphs_service)
{
	return sim_cphs_is_active(sim->cphs_service_table, cphs_service);
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

static void sim_free_early_state(struct ofono_sim *sim)
{
	if (sim->iccid) {
		g_free(sim->iccid);
		sim->iccid = NULL;
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

	if (sim->early_context) {
		ofono_sim_context_free(sim->early_context);
		sim->early_context = NULL;
	}
}

static void sim_free_main_state(struct ofono_sim *sim)
{
	int i;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sim->pin_retries[i] = -1;

	memset(sim->locked_pins, 0, sizeof(sim->locked_pins));

	if (sim->imsi) {
		g_free(sim->imsi);
		sim->imsi = NULL;
	}

	sim->mcc[0] = '\0';
	sim->mnc[0] = '\0';

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
		sim->sdn_ready = FALSE;
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

	sim->phase = OFONO_SIM_PHASE_UNKNOWN;
	sim->cphs_phase = OFONO_SIM_CPHS_PHASE_NONE;
	sim->mnc_length = 0;
	memset(sim->cphs_service_table, 0, 2);

	if (sim->efimg) {
		g_free(sim->efimg);
		sim->efimg = NULL;
		sim->efimg_length = 0;
		g_free(sim->iidf_watch_ids);
		sim->iidf_watch_ids = NULL;
	}

	g_free(sim->iidf_image);
	sim->iidf_image = NULL;

	sim->fixed_dialing = FALSE;
	sim->barred_dialing = FALSE;

	if (sim->context) {
		ofono_sim_context_free(sim->context);
		sim->context = NULL;
	}
}

static void sim_free_state(struct ofono_sim *sim)
{
	sim_free_early_state(sim);
	sim_free_main_state(sim);
}

void ofono_sim_inserted_notify(struct ofono_sim *sim, ofono_bool_t inserted)
{
	if (inserted == TRUE && sim->state == OFONO_SIM_STATE_NOT_PRESENT)
		sim->state = OFONO_SIM_STATE_INSERTED;
	else if (inserted == FALSE && sim->state != OFONO_SIM_STATE_NOT_PRESENT)
		sim->state = OFONO_SIM_STATE_NOT_PRESENT;
	else
		return;

	if (!__ofono_atom_get_registered(sim->atom))
		return;

	sim_inserted_update(sim);
	call_state_watches(sim);

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

static void spn_watch_cb(gpointer data, gpointer user_data)
{
	struct ofono_watchlist_item *item = data;
	struct ofono_sim *sim = user_data;

	if (item->notify)
		((ofono_sim_spn_cb_t) item->notify)(sim->spn, sim->spn_dc,
							item->notify_data);
}

static inline void spn_watches_notify(struct ofono_sim *sim)
{
	if (sim->spn_watches->items)
		g_slist_foreach(sim->spn_watches->items, spn_watch_cb, sim);

	sim->flags &= ~SIM_FLAG_READING_SPN;
}

static void sim_spn_set(struct ofono_sim *sim, const void *data, int length,
						const unsigned char *dc)
{
	g_free(sim->spn);
	sim->spn = NULL;

	g_free(sim->spn_dc);
	sim->spn_dc = NULL;

	if (data == NULL)
		goto notify;

	/*
	 * TS 31.102 says:
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
	sim->spn = sim_string_to_utf8(data, length);
	if (sim->spn == NULL) {
		ofono_error("EFspn read successfully, but couldn't parse");
		goto notify;
	}

	if (strlen(sim->spn) == 0) {
		g_free(sim->spn);
		sim->spn = NULL;
		goto notify;
	}

	if (dc)
		sim->spn_dc = g_memdup(dc, 1);

notify:
	spn_watches_notify(sim);
}

static void sim_cphs_spn_short_read_cb(int ok, int length, int record,
					const unsigned char *data,
					int record_length, void *user_data)
{
	struct ofono_sim *sim = user_data;

	if (!ok) {
		sim_spn_set(sim, NULL, 0, NULL);
		return;
	}

	sim_spn_set(sim, data, length, NULL);
}

static void sim_cphs_spn_read_cb(int ok, int length, int record,
					const unsigned char *data,
					int record_length, void *user_data)
{
	struct ofono_sim *sim = user_data;

	if (!ok) {
		if (__ofono_sim_cphs_service_available(sim,
						SIM_CPHS_SERVICE_SHORT_SPN))
			ofono_sim_read(sim->context,
					SIM_EF_CPHS_SPN_SHORT_FILEID,
					OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
					sim_cphs_spn_short_read_cb, sim);
		else
			sim_spn_set(sim, NULL, 0, NULL);

		return;
	}

	sim_spn_set(sim, data, length, NULL);
}

static void sim_spn_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *user_data)
{
	struct ofono_sim *sim = user_data;

	if (!ok) {
		ofono_sim_read(sim->context, SIM_EF_CPHS_SPN_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				sim_cphs_spn_read_cb, sim);

		return;
	}

	sim_spn_set(sim, data + 1, length - 1, data);
}

static void sim_spn_changed(int id, void *userdata)
{
	struct ofono_sim *sim = userdata;

	if (sim->flags & SIM_FLAG_READING_SPN)
		return;

	sim->flags |= SIM_FLAG_READING_SPN;
	ofono_sim_read(sim->context, SIM_EFSPN_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			sim_spn_read_cb, sim);
}

static void sim_spn_init(struct ofono_sim *sim)
{
	sim->ef_spn_watch = ofono_sim_add_file_watch(sim->context,
					SIM_EFSPN_FILEID, sim_spn_changed, sim,
					NULL);

	sim->cphs_spn_watch = ofono_sim_add_file_watch(sim->context,
					SIM_EF_CPHS_SPN_FILEID,
					sim_spn_changed, sim, NULL);

	if (__ofono_sim_cphs_service_available(sim,
						SIM_CPHS_SERVICE_SHORT_SPN))
		sim->cphs_spn_short_watch = ofono_sim_add_file_watch(
				sim->context, SIM_EF_CPHS_SPN_SHORT_FILEID,
				sim_spn_changed, sim, NULL);
}

static void sim_spn_close(struct ofono_sim *sim)
{
	__ofono_watchlist_free(sim->spn_watches);
	sim->spn_watches = NULL;

	/*
	 * We have not initialized SPN logic at all yet, either because
	 * no netreg / gprs atom has been needed or we have not reached the
	 * post_sim state
	 */
	if (sim->ef_spn_watch == 0)
		return;

	ofono_sim_remove_file_watch(sim->context, sim->ef_spn_watch);
	sim->ef_spn_watch = 0;

	ofono_sim_remove_file_watch(sim->context, sim->cphs_spn_watch);
	sim->cphs_spn_watch = 0;

	if (sim->cphs_spn_short_watch) {
		ofono_sim_remove_file_watch(sim->context,
						sim->cphs_spn_short_watch);
		sim->cphs_spn_short_watch = 0;
	}

	sim->flags &= ~SIM_FLAG_READING_SPN;

	g_free(sim->spn);
	sim->spn = NULL;

	g_free(sim->spn_dc);
	sim->spn_dc = NULL;
}

ofono_bool_t ofono_sim_add_spn_watch(struct ofono_sim *sim, unsigned int *id,
					ofono_sim_spn_cb_t cb, void *data,
					ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *item;
	unsigned int watch_id;

	DBG("%p", sim);

	if (sim == NULL)
		return 0;

	item = g_new0(struct ofono_watchlist_item, 1);

	item->notify = cb;
	item->destroy = destroy;
	item->notify_data = data;

	watch_id = __ofono_watchlist_add_item(sim->spn_watches, item);
	if (watch_id == 0)
		return FALSE;

	*id = watch_id;

	if (sim->ef_spn_watch == 0) {
		sim_spn_init(sim);
		sim_spn_changed(0, sim);
		return TRUE;
	}

	if (sim->flags & SIM_FLAG_READING_SPN)
		return TRUE;

	((ofono_sim_spn_cb_t) item->notify)(sim->spn, sim->spn_dc,
							item->notify_data);
	return TRUE;
}

ofono_bool_t ofono_sim_remove_spn_watch(struct ofono_sim *sim, unsigned int *id)
{
	gboolean ret;

	DBG("%p", sim);

	if (sim == NULL)
		return FALSE;

	ret = __ofono_watchlist_remove_item(sim->spn_watches, *id);
	if (ret == TRUE)
		*id = 0;

	return ret;
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
		return;
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

	switch (pin_type) {
	case OFONO_SIM_PASSWORD_NONE:
	case OFONO_SIM_PASSWORD_SIM_PIN2:
	case OFONO_SIM_PASSWORD_SIM_PUK2:
		break;
	default:
		if (sim->state == OFONO_SIM_STATE_READY) {
			/* Force the sim state out of READY */
			sim_free_main_state(sim);

			sim->state = OFONO_SIM_STATE_LOCKED_OUT;
			call_state_watches(sim);
		}
		break;
	}

	sim_pin_retries_check(sim);

	switch (pin_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN2:
	case OFONO_SIM_PASSWORD_SIM_PUK2:
	case OFONO_SIM_PASSWORD_NONE:
		if (sim->state == OFONO_SIM_STATE_READY)
			break;

		/* Fall through */
		sim_initialize_after_pin(sim);
		break;
	default:
		break;
	}
}

void __ofono_sim_recheck_pin(struct ofono_sim *sim)
{
	if (sim->driver->query_passwd_state == NULL) {
		sim_initialize_after_pin(sim);
		return;
	}

	sim->driver->query_passwd_state(sim, sim_pin_query_cb, sim);
}

int ofono_sim_driver_register(const struct ofono_sim_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_sim_driver_unregister(const struct ofono_sim_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void emulator_remove_handler(struct ofono_atom *atom, void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	ofono_emulator_remove_handler(em, data);
}

static void sim_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);
	struct ofono_sim *sim = __ofono_atom_get_data(atom);

	__ofono_modem_foreach_registered_atom(modem,
						OFONO_ATOM_TYPE_EMULATOR_HFP,
						emulator_remove_handler,
						"+CNUM");

	__ofono_modem_remove_atom_watch(modem, sim->hfp_watch);

	__ofono_watchlist_free(sim->state_watches);
	sim->state_watches = NULL;

	sim_spn_close(sim);

	g_dbus_unregister_interface(conn, path, OFONO_SIM_MANAGER_INTERFACE);
	ofono_modem_remove_interface(modem, OFONO_SIM_MANAGER_INTERFACE);
}

static void sim_remove(struct ofono_atom *atom)
{
	struct ofono_sim *sim = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (sim == NULL)
		return;

	if (sim->driver != NULL && sim->driver->remove != NULL)
		sim->driver->remove(sim);

	sim_free_state(sim);

	sim_fs_free(sim->simfs);
	sim->simfs = NULL;

	g_free(sim);
}

struct ofono_sim *ofono_sim_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_sim *sim;
	GSList *l;
	int i;

	if (driver == NULL)
		return NULL;

	sim = g_try_new0(struct ofono_sim, 1);

	if (sim == NULL)
		return NULL;

	sim->phase = OFONO_SIM_PHASE_UNKNOWN;
	sim->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_SIM,
						sim_remove, sim);

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sim->pin_retries[i] = -1;

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

static void emulator_cnum_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	struct ofono_sim *sim = userdata;
	struct ofono_error result;
	GSList *l;
	const char *phone;
	/*
	 * '+CNUM: ,"+",,,4' + phone number + phone type on 3 digits max
	 * + terminating null
	 */
	char buf[OFONO_MAX_PHONE_NUMBER_LENGTH + 18 + 1];

	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY:
		for (l = sim->own_numbers; l; l = l->next) {
			struct ofono_phone_number *ph = l->data;

			phone = phone_number_to_string(ph);
			sprintf(buf, "+CNUM: ,\"%s\",%d,,4", phone, ph->type);
			ofono_emulator_send_info(em, buf, l->next == NULL ?
							TRUE : FALSE);
		}

		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		ofono_emulator_send_final(em, &result);
		break;

	default:
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
	};
}

static void emulator_hfp_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED)
		ofono_emulator_add_handler(em, "+CNUM", emulator_cnum_cb, data,
						NULL);
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
	sim->spn_watches = __ofono_watchlist_new(g_free);
	sim->simfs = sim_fs_new(sim, sim->driver);

	__ofono_atom_register(sim->atom, sim_unregister);

	ofono_sim_add_state_watch(sim, sim_ready, sim, NULL);

	if (sim->state > OFONO_SIM_STATE_NOT_PRESENT)
		sim_initialize(sim);

	sim->hfp_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_EMULATOR_HFP,
					emulator_hfp_watch, sim, NULL);
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

static ofono_bool_t is_valid_pin(const char *pin, unsigned int min,
					unsigned int max)
{
	unsigned int i;

	/* Pin must not be empty */
	if (pin == NULL || pin[0] == '\0')
		return FALSE;

	i = strlen(pin);
	if (i != strspn(pin, "0123456789"))
		return FALSE;

	if (min <= i && i <= max)
		return TRUE;

	return FALSE;
}

ofono_bool_t __ofono_is_valid_sim_pin(const char *pin,
					enum ofono_sim_password_type type)
{
	switch (type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		/* 11.11 Section 9.3 ("CHV"): 4..8 IA-5 digits */
		return is_valid_pin(pin, 4, 8);
		break;
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
	case OFONO_SIM_PASSWORD_PHNET_PIN:
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
	case OFONO_SIM_PASSWORD_PHSP_PIN:
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		/* 22.022 Section 14 4..16 IA-5 digits */
		return is_valid_pin(pin, 4, 16);
		break;
	case OFONO_SIM_PASSWORD_SIM_PUK:
	case OFONO_SIM_PASSWORD_SIM_PUK2:
	case OFONO_SIM_PASSWORD_PHFSIM_PUK:
	case OFONO_SIM_PASSWORD_PHNET_PUK:
	case OFONO_SIM_PASSWORD_PHNETSUB_PUK:
	case OFONO_SIM_PASSWORD_PHSP_PUK:
	case OFONO_SIM_PASSWORD_PHCORP_PUK:
		/* 11.11 Section 9.3 ("UNBLOCK CHV"), 8 IA-5 digits */
		return is_valid_pin(pin, 8, 8);
		break;
	case OFONO_SIM_PASSWORD_NONE:
		return is_valid_pin(pin, 0, 8);
		break;
	case OFONO_SIM_PASSWORD_INVALID:
		break;
	}

	return FALSE;
}

ofono_bool_t __ofono_is_valid_net_pin(const char *pin)
{
	return is_valid_pin(pin, 4, 4);
}

static void sim_file_changed_flush(struct ofono_sim *sim, int id)
{
	int i, imgid;

	if (id == SIM_EFIMG_FILEID)
		/* All cached images become invalid */
		sim_fs_image_cache_flush(sim->simfs);
	else if (sim->efimg) {
		/*
		 * Data and CLUT for image instances stored in the changed
		 * file need to be re-read.
		 */
		for (i = sim->efimg_length / 9 - 1; i >= 0; i--) {
			imgid = (sim->efimg[i * 9 + 3] << 8) |
				sim->efimg[i * 9 + 4];

			if (imgid == id)
				sim_fs_image_cache_flush_file(sim->simfs, i);
		}
	}

	sim_fs_cache_flush_file(sim->simfs, id);
}

void __ofono_sim_refresh(struct ofono_sim *sim, GSList *file_list,
			ofono_bool_t full_file_change, ofono_bool_t naa_init)
{
	GSList *l;
	gboolean reinit_naa = naa_init || full_file_change;

	/*
	 * Check if any files used in SIM initialisation procedure
	 * are affected, except EFiccid, EFpl, EFli.
	 */
	for (l = file_list; l; l = l->next) {
		struct stk_file *file = l->data;
		uint32_t mf, df, ef;

		if (file->len != 6)
			continue;

		mf = (file->file[0] << 8) | (file->file[1] << 0);
		df = (file->file[2] << 8) | (file->file[3] << 0);
		ef = (file->file[4] << 8) | (file->file[5] << 0);

		if (mf != 0x3f00)
			continue;

		/*
		 * 8.18: "the path '3F007FFF' indicates the relevant
		 * NAA Application dedicated file;".
		 */
		if (df == 0x7fff)
			df = 0x7f20;

#define DFGSM (0x7f20 << 16)
#define DFTEL (0x7f10 << 16)

		switch ((df << 16) | ef) {
		case DFGSM | SIM_EFEST_FILEID:
		case DFGSM | SIM_EFUST_FILEID: /* aka. EFSST */
		case DFGSM | SIM_EFPHASE_FILEID:
		case DFGSM | SIM_EFAD_FILEID:
		case DFTEL | SIM_EFBDN_FILEID:
		case DFTEL | SIM_EFADN_FILEID:
		case DFGSM | SIM_EF_CPHS_INFORMATION_FILEID:
			reinit_naa = TRUE;
			break;
		}
	}

	/* Flush cached content for affected files */
	if (full_file_change)
		sim_fs_cache_flush(sim->simfs);
	else {
		for (l = file_list; l; l = l->next) {
			struct stk_file *file = l->data;
			int id = (file->file[file->len - 2] << 8) |
				(file->file[file->len - 1] << 0);

			sim_file_changed_flush(sim, id);
		}
	}

	if (reinit_naa) {
		/* Force the sim state out of READY */
		sim_free_main_state(sim);

		sim->state = OFONO_SIM_STATE_INSERTED;
		__ofono_modem_sim_reset(__ofono_atom_get_modem(sim->atom));
	}

	/*
	 * Notify the subscribers of files that have changed and who
	 * haven't unsubsribed during the SIM state change.
	 */
	if (full_file_change)
		sim_fs_notify_file_watches(sim->simfs, -1);
	else {
		for (l = file_list; l; l = l->next) {
			struct stk_file *file = l->data;
			int id = (file->file[file->len - 2] << 8) |
				(file->file[file->len - 1] << 0);

			sim_fs_notify_file_watches(sim->simfs, id);
		}
	}

	if (reinit_naa) {
		/*
		 * REVISIT: There's some concern that on re-insertion the
		 * atoms will start to talk to the SIM before it becomes
		 * ready, on certain SIMs.
		 */
		/*
		 * Start initialization procedure from after EFiccid,
		 * EFli and EFpl are retrieved.
		 */
		__ofono_sim_recheck_pin(sim);
	}
}
