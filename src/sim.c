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
#include "util.h"
#include "smsutil.h"
#include "sim.h"

#define SIM_MANAGER_INTERFACE "org.ofono.SimManager"

/* 27.007 Section 7.1 Subscriber Number */
struct own_number {
	struct ofono_phone_number phone_number;
	int speed;
	int service;
	int itc;
	int npi;
};

struct sim_manager_data {
	struct ofono_sim_ops *ops;
	int flags;
	DBusMessage *pending;
	char *imsi;
	GSList *own_numbers;
	char *spn;
	int dcbyte;

	GSList *update_spn_notify;

	int own_numbers_num;
	int own_numbers_size;
	int own_numbers_current;
};

static char **own_numbers_by_type(GSList *own_numbers, int type)
{
	int nelem;
	GSList *l;
	struct own_number *num;
	char **ret;

	if (!own_numbers)
		return NULL;

	for (nelem = 0, l = own_numbers; l; l = l->next) {
		num = l->data;

		if (num->service != type)
			continue;

		nelem++;
	}

	if (nelem == 0) {
		if (g_slist_length(own_numbers) != 1)
			return NULL;

		num = own_numbers->data;

		/* Generic case */
		if (num->service != -1)
			return NULL;

		ret = g_new0(char *, 2);

		ret[0] = g_strdup(phone_number_to_string(&num->phone_number));

		return ret;
	}

	ret = g_new0(char *, nelem + 1);

	nelem = 0;
	for (l = own_numbers; l; l = l->next) {
		num = l->data;

		if (num->service != type)
			continue;

		ret[nelem++] = g_strdup(phone_number_to_string(&num->phone_number));
	}

	return ret;
}

static struct sim_manager_data *sim_manager_create()
{
	return g_try_new0(struct sim_manager_data, 1);
}

static void sim_manager_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct sim_manager_data *data = modem->sim_manager;

	if (data->imsi) {
		g_free(data->imsi);
		data->imsi = NULL;
	}

	if (data->own_numbers) {
		g_slist_foreach(data->own_numbers, (GFunc)g_free, NULL);
		g_slist_free(data->own_numbers);
		data->own_numbers = NULL;
	}

	if (data->spn) {
		g_free(data->spn);
		data->spn = NULL;
	}
}

static DBusMessage *sim_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char **own_voice;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	if (sim->imsi)
		dbus_gsm_dict_append(&dict, "SubscriberIdentity",
					DBUS_TYPE_STRING, &sim->imsi);

	if (sim->spn)
		dbus_gsm_dict_append(&dict, "ServiceProvider",
					DBUS_TYPE_STRING, &sim->spn);

	own_voice = own_numbers_by_type(sim->own_numbers,
					OWN_NUMBER_SERVICE_TYPE_VOICE);

	if (own_voice) {
		dbus_gsm_dict_append_array(&dict, "VoiceSubscriberNumber",
						DBUS_TYPE_STRING, &own_voice);
		dbus_gsm_free_string_array(own_voice);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable sim_manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	sim_get_properties	},
	{ }
};

static GDBusSignalTable sim_manager_signals[] = { { } };

enum sim_fileids {
	SIM_EFMSISDN_FILEID = 0x6f40,
	SIM_EFSPN_FILEID = 0x6f46,
};

#define SIM_EFSPN_DC_HOME_PLMN_BIT 0x1
#define SIM_EFSPN_DC_ROAMING_SPN_BIT 0x2

static void sim_spn_notify(struct ofono_modem *modem, update_spn_cb cb)
{
	struct sim_manager_data *sim = modem->sim_manager;

	cb(modem, sim->spn,
			sim->dcbyte & SIM_EFSPN_DC_HOME_PLMN_BIT,
			!(sim->dcbyte & SIM_EFSPN_DC_ROAMING_SPN_BIT));
}

static void sim_spn_read_cb(const struct ofono_error *error,
		const unsigned char *sdata, int length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	unsigned char *endp;
	GSList *l;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || length <= 1)
		return;

	sim->dcbyte = sdata[0];
	sdata++;
	length--;

	/* Successfully read the SPN from the SIM DB */
	endp = memchr(sdata, 0xff, length);
	if (endp)
		length = endp - sdata;

	/*
	 * The specification has this to say about the encoding of SPN:
	 * Coding:
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
	 * Assume the first option.
	 */
	sim->spn = convert_gsm_to_utf8(sdata, length, NULL, NULL, 0xff);

	for (l = sim->update_spn_notify; l; l = l->next)
		sim_spn_notify(modem, l->data);
}

static void sim_spn_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int dummy, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || length <= 1 ||
			structure != OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		return;

	sim->ops->read_file_transparent(modem, SIM_EFSPN_FILEID, 0, length,
					sim_spn_read_cb, modem);
}

static gboolean sim_retrieve_spn(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;

	sim->ops->read_file_info(modem, SIM_EFSPN_FILEID,
					sim_spn_info_cb, modem);

	return FALSE;
}

static void sim_imsi_cb(const struct ofono_error *error, const char *imsi,
		void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		return;

	sim->imsi = g_strdup(imsi);
}

static gboolean sim_retrieve_imsi(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;

	sim->ops->read_imsi(modem, sim_imsi_cb, modem);

	return FALSE;
}

static void sim_msisdn_read_cb(const struct ofono_error *error,
		const unsigned char *sdata, int length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	struct own_number *ph;
	int number_len;
	int ton_npi;
	int i, digit;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto skip;

	if (length < sim->own_numbers_size)
		goto skip;

	/* Skip Alpha-Identifier field */
	sdata += sim->own_numbers_size - 14;

	number_len = *sdata++;
	ton_npi = *sdata++;

	if (number_len > 11 || ton_npi == 0xff)
		goto skip;

	ph = g_new(struct own_number, 1);

	ph->speed = -1;
	ph->service = -1;
	ph->itc = -1;
	ph->phone_number.type = bit_field(ton_npi, 4, 3);
	ph->npi = bit_field(ton_npi, 0, 4);

	/* BCD coded, however the TON/NPI is given by the first byte */
	number_len = (number_len - 1) * 2;

	extract_bcd_number(sdata, number_len, ph->phone_number.number);

	sim->own_numbers = g_slist_prepend(sim->own_numbers, ph);

skip:
	sim->own_numbers_current ++;
	if (sim->own_numbers_current < sim->own_numbers_num)
		sim->ops->read_file_linear(modem, SIM_EFMSISDN_FILEID,
						sim->own_numbers_current,
						sim->own_numbers_size,
						sim_msisdn_read_cb, modem);
	else
		/* All records retrieved */
		if (sim->own_numbers)
			sim->own_numbers = g_slist_reverse(sim->own_numbers);
}

static void sim_msisdn_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int record_length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || length < 14 ||
			record_length < 14 ||
			structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

	sim->own_numbers_current = 0;
	sim->own_numbers_size = record_length;
	sim->own_numbers_num = length / record_length;
	sim->ops->read_file_linear(modem, SIM_EFMSISDN_FILEID, 0,
			record_length, sim_msisdn_read_cb, modem);
}

static gboolean sim_retrieve_own_number(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;

	sim->ops->read_file_info(modem, SIM_EFMSISDN_FILEID,
			sim_msisdn_info_cb, modem);

	return FALSE;
}

static void initialize_sim_manager(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (!g_dbus_register_interface(conn, modem->path,
					SIM_MANAGER_INTERFACE,
					sim_manager_methods,
					sim_manager_signals,
					NULL, modem,
					sim_manager_destroy)) {
		ofono_error("Could not register SIMManager interface");
		sim_manager_destroy(modem);

		return;
	}

	ofono_debug("SIMManager interface for modem: %s created",
			modem->path);

	modem_add_interface(modem, SIM_MANAGER_INTERFACE);

	if (modem->sim_manager->ops->read_file_transparent)
		g_timeout_add(0, sim_retrieve_spn, modem);

	if (modem->sim_manager->ops->read_imsi)
		g_timeout_add(0, sim_retrieve_imsi, modem);

	if (modem->sim_manager->ops->read_file_linear)
		g_timeout_add(0, sim_retrieve_own_number, modem);
}

int ofono_sim_manager_register(struct ofono_modem *modem,
					struct ofono_sim_ops *ops)
{
	if (modem == NULL)
		return -1;
	if (modem->sim_manager == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->sim_manager->ops = ops;

	initialize_sim_manager(modem);

	return 0;
}

void ofono_sim_manager_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = dbus_gsm_connection();

	g_dbus_unregister_interface(conn, modem->path,
					SIM_MANAGER_INTERFACE);
	modem_remove_interface(modem, SIM_MANAGER_INTERFACE);
}

void ofono_sim_manager_init(struct ofono_modem *modem)
{
	modem->sim_manager = sim_manager_create();
}

void ofono_sim_manager_exit(struct ofono_modem *modem)
{
	if (modem->sim_manager == NULL)
		return;

	g_free(modem->sim_manager);

	modem->sim_manager = 0;
}

int ofono_spn_update_notify_register(struct ofono_modem *modem,
		update_spn_cb cb)
{
	if (modem->sim_manager == NULL)
		return -1;

	modem->sim_manager->update_spn_notify =
		g_slist_append(modem->sim_manager->update_spn_notify, cb);

	if (modem->sim_manager->spn)
		sim_spn_notify(modem, cb);

	return 0;
}

int ofono_spn_update_notify_unregister(struct ofono_modem *modem,
		update_spn_cb cb)
{
	if (modem->sim_manager == NULL)
		return -1;

	modem->sim_manager->update_spn_notify =
		g_slist_remove(modem->sim_manager->update_spn_notify, cb);
	return 0;
}
