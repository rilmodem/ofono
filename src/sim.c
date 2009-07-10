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

static gboolean sim_op_next(gpointer user_data);
static gboolean sim_op_retrieve_next(gpointer user);

struct sim_file_op {
	int id;
	enum ofono_sim_file_structure structure;
	int length;
	int record_length;
	int current;
	ofono_sim_file_read_cb_t cb;
	void *userdata;
};

struct sim_manager_data {
	struct ofono_sim_ops *ops;
	int flags;
	DBusMessage *pending;
	char *imsi;
	GSList *own_numbers;
	GSList *ready_notify;
	gboolean ready;
	GQueue *simop_q;

	int dcbyte;

	GSList *spdi;

	int own_numbers_num;
	int own_numbers_size;
	int own_numbers_current;

	GSList *opl;
	int opl_num;
	int opl_size;
	int opl_current;

	struct pnn_operator *pnn;
	int pnn_num;
	int pnn_size;
	int pnn_current;
};

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

struct pnn_operator {
	char *longname;
	char *shortname;
};

static void sim_file_op_free(struct sim_file_op *node)
{
	g_free(node);
}

static struct sim_manager_data *sim_manager_create()
{
	return g_try_new0(struct sim_manager_data, 1);
}

static void sim_manager_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct sim_manager_data *data = modem->sim_manager;
	int i;

	if (data->imsi) {
		g_free(data->imsi);
		data->imsi = NULL;
	}

	if (data->own_numbers) {
		g_slist_foreach(data->own_numbers, (GFunc)g_free, NULL);
		g_slist_free(data->own_numbers);
		data->own_numbers = NULL;
	}

	if (data->simop_q) {
		g_queue_foreach(data->simop_q, (GFunc)sim_file_op_free, NULL);
		g_queue_free(data->simop_q);
		data->simop_q = NULL;
	}

	if (data->spdi) {
		g_slist_foreach(data->spdi, (GFunc)g_free, NULL);
		g_slist_free(data->spdi);
		data->spdi = NULL;
	}

	if (data->opl) {
		g_slist_foreach(data->opl, (GFunc)g_free, NULL);
		g_slist_free(data->opl);
		data->opl = NULL;
	}

	if (data->pnn) {
		for (i = 0; i < data->pnn_num; i ++) {
			if (data->pnn[i].longname)
				g_free(data->pnn[i].longname);
			if (data->pnn[i].shortname)
				g_free(data->pnn[i].shortname);
		}
		g_free(data->pnn);
		data->pnn = NULL;
		data->pnn_num = 0;
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
	char **own_numbers;

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

	own_numbers = get_own_numbers(sim->own_numbers);

	dbus_gsm_dict_append_array(&dict, "SubscriberNumbers",
					DBUS_TYPE_STRING, &own_numbers);
	dbus_gsm_free_string_array(own_numbers);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable sim_manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	sim_get_properties	},
	{ }
};

static GDBusSignalTable sim_manager_signals[] = { { } };

static char *network_name_parse(const unsigned char *buffer, int length)
{
	unsigned char *endp;
	unsigned char dcs;
	int i;

	if (length < 1)
		return NULL;

	dcs = *buffer ++;
	length --;

	/* TODO: "The MS should add the letters for the Country's
	 * Initials and a separator (e.g. a space)" */
	if (is_bit_set(dcs, 4))
		ofono_error("Network Name DCS implies country initials");

	switch (dcs & (7 << 4)) {
	case 0x00:
		endp = memchr(buffer, 0xff, length);
		if (endp)
			length = endp - buffer;
		return convert_gsm_to_utf8(buffer, length,
				NULL, NULL, 0xff);
	case 0x10:
		if ((length % 2) == 1) {
			if (buffer[length - 1] != 0xff)
				return NULL;

			length = length - 1;
		}

		for (i = 0; i < length; i += 2)
			if (buffer[i] == 0xff && buffer[i + 1] == 0xff)
				break;

		return g_convert(buffer, length, "UTF-8//TRANSLIT", "UCS-2BE",
					NULL, NULL, NULL);
	}

	return NULL;
}

enum sim_fileids {
	SIM_EFMSISDN_FILEID = 0x6f40,
	SIM_EFSPN_FILEID = 0x6f46,
	SIM_EFPNN_FILEID = 0x6fc5,
	SIM_EFOPL_FILEID = 0x6fc6,
	SIM_EFSPDI_FILEID = 0x6fcd,
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
	sim->spn = sim_string_to_utf8(sdata, length);

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

static void sim_msisdn_read_cb(struct ofono_modem *modem, int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct sim_manager_data *sim = modem->sim_manager;
	int total = length / record_length;
	struct ofono_phone_number *ph;
	int number_len;
	int ton_npi;

	if (!ok)
		return;

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

	if (length < 14 || record_length < 14 || length < record_length)
		return;

	/* Skip Alpha-Identifier field */
	data += record_length - 14;

	number_len = *data++;
	ton_npi = *data++;

	if (number_len > 11 || ton_npi == 0xff)
		goto check;

	ph = g_new(struct ofono_phone_number, 1);

	ph->type = bit_field(ton_npi, 4, 3);

	/* BCD coded, however the TON/NPI is given by the first byte */
	number_len = (number_len - 1) * 2;

	extract_bcd_number(data, number_len, ph->number);

	sim->own_numbers = g_slist_prepend(sim->own_numbers, ph);

check:
	if (record == total && sim->own_numbers) {
		char **own_numbers;
		DBusConnection *conn = dbus_gsm_connection();

		/* All records retrieved */
		sim->own_numbers = g_slist_reverse(sim->own_numbers);

		own_numbers = get_own_numbers(sim->own_numbers);

		dbus_gsm_signal_array_property_changed(conn, modem->path,
							SIM_MANAGER_INTERFACE,
							"SubscriberNumbers",
							DBUS_TYPE_STRING,
							&own_numbers);
		dbus_gsm_free_string_array(own_numbers);
	}
}

struct sim_operator {
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
};

static void parse_mcc_mnc(struct sim_operator *oper, const guint8 *bcd)
{
	char *mcc = oper->mcc;
	char *mnc = oper->mnc;
	guint8 digit;

	digit = (bcd[0] >> 0) & 0xf;
	if (digit != 0xf)
		*mcc ++ = '0' + digit;
	digit = (bcd[0] >> 4) & 0xf;
	if (digit != 0xf)
		*mcc ++ = '0' + digit;
	digit = (bcd[1] >> 0) & 0xf;
	if (digit != 0xf)
		*mcc ++ = '0' + digit;
	digit = (bcd[2] >> 0) & 0xf;
	if (digit != 0xf)
		*mnc ++ = '0' + digit;
	digit = (bcd[2] >> 4) & 0xf;
	if (digit != 0xf)
		*mnc ++ = '0' + digit;
	digit = (bcd[1] >> 4) & 0xf;
	if (digit != 0xf)
		*mnc ++ = '0' + digit;
}

static struct sim_operator *sim_operator_alloc(const guint8 *bcd)
{
	struct sim_operator *spdi = g_new0(struct sim_operator, 1);

	parse_mcc_mnc(spdi, bcd);

	return spdi;
}

static gint spdi_operator_compare(gconstpointer a, gconstpointer b)
{
	const struct sim_operator *opa = a;
	const struct sim_operator *opb = b;
	gint r;

	if ((r = strcmp(opa->mcc, opb->mcc)))
		return r;

	return strcmp(opa->mnc, opb->mnc);
}

gboolean ofono_operator_in_spdi(struct ofono_modem *modem,
				const struct ofono_network_operator *op)
{
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_operator spdi_op;

	if (!sim)
		return FALSE;

	g_strlcpy(spdi_op.mcc, op->mcc, sizeof(spdi_op.mcc));
	g_strlcpy(spdi_op.mnc, op->mnc, sizeof(spdi_op.mnc));

	return g_slist_find_custom(sim->spdi,
			&spdi_op, spdi_operator_compare) != NULL;
}

static void sim_spdi_read_cb(const struct ofono_error *error,
				const unsigned char *spdidata,
				int length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	const guint8 *plmn_list;
	GSList *l;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || length <= 5)
		return;

	plmn_list = ber_tlv_find_by_tag(spdidata, 0x80, length, &length);
	if (!plmn_list) {
		ofono_debug("Couldn't parse the EF-SPDI contents as a TLV");
		return;
	}

	for (length /= 3; length --; plmn_list += 3) {
		if ((plmn_list[0] & plmn_list[1] & plmn_list[2]) == 0xff)
			continue;

		sim->spdi = g_slist_insert_sorted(sim->spdi,
				sim_operator_alloc(plmn_list),
				spdi_operator_compare);
	}

	if (sim->spdi)
		for (l = sim->update_spn_notify; l; l = l->next)
			sim_spn_notify(modem, l->data);
}

static void sim_spdi_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int dummy, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || length <= 5 ||
			structure != OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		return;

	sim->ops->read_file_transparent(modem, SIM_EFSPDI_FILEID, 0, length,
					sim_spdi_read_cb, modem);
}

static gboolean sim_retrieve_spdi(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;

	sim->ops->read_file_info(modem, SIM_EFSPDI_FILEID,
					sim_spdi_info_cb, modem);

	return FALSE;
}

struct opl_operator {
	struct sim_operator mcc_mnc;
	guint16 lac_tac_low;
	guint16 lac_tac_high;
	guint8 id;
};

static struct opl_operator *opl_operator_alloc(const guint8 *record)
{
	struct opl_operator *oper = g_new0(struct opl_operator, 1);

	parse_mcc_mnc(&oper->mcc_mnc, record);
	record += 3;

	oper->lac_tac_low = (record[0] << 8) | record[1];
	record += 2;
	oper->lac_tac_high = (record[0] << 8) | record[1];
	record += 2;

	oper->id = record[0];
	if (!oper->id) {
		/* TODO: name to be taken from other sources, see TS 22.101 */
	}

	return oper;
}

static gint opl_operator_compare(gconstpointer a, gconstpointer b)
{
	const struct opl_operator *opa = a;
	const struct sim_operator *opb = b;
	int i;

	for (i = 0; opb->mcc[i] | opa->mcc_mnc.mcc[i]; i ++)
		if (opb->mcc[i] != opa->mcc_mnc.mcc[i] &&
				!(opa->mcc_mnc.mcc[i] == '0' + 0xd &&
					opb->mcc[i]))
			return opa->mcc_mnc.mcc[i] - opb->mcc[i];
	for (i = 0; opb->mnc[i] | opa->mcc_mnc.mnc[i]; i ++)
		if (opb->mnc[i] != opa->mcc_mnc.mnc[i] &&
				!(opa->mcc_mnc.mnc[i] == '0' + 0xd &&
					opb->mnc[i]))
			return opa->mcc_mnc.mnc[i] - opb->mnc[i];

	if (opa->lac_tac_low > 0x0000 || opa->lac_tac_high < 0xfffe)
		return 1;

	return 0;
}

static void sim_opl_read_cb(const struct ofono_error *error,
		const unsigned char *sdata, int length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	struct opl_operator *oper;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto skip;

	if (length < sim->opl_size)
		goto skip;

	oper = opl_operator_alloc(sdata);
	if (oper->id > sim->pnn_num) {
		g_free(oper);
		goto skip;
	}

	sim->opl = g_slist_prepend(sim->opl, oper);

skip:
	sim->opl_current ++;
	if (sim->opl_current < sim->opl_num)
		sim->ops->read_file_linear(modem, SIM_EFOPL_FILEID,
						sim->opl_current,
						sim->opl_size,
						sim_opl_read_cb, modem);
	else
		/* All records retrieved */
		if (sim->opl)
			sim->opl = g_slist_reverse(sim->opl);
}

static void sim_opl_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int record_length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || length < 8 ||
			record_length < 8 ||
			structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

	sim->opl_current = 0;
	sim->opl_size = record_length;
	sim->opl_num = length / record_length;
	sim->ops->read_file_linear(modem, SIM_EFOPL_FILEID, 0,
			record_length, sim_opl_read_cb, modem);
}

static gboolean sim_retrieve_opl(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;

	sim->ops->read_file_info(modem, SIM_EFOPL_FILEID,
			sim_opl_info_cb, modem);

	return FALSE;
}

const char *ofono_operator_name_sim_override(struct ofono_modem *modem,
		const char *mcc, const char *mnc)
{
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_operator op;
	GSList *l;
	const struct opl_operator *opl_op;

	g_strlcpy(op.mcc, mcc, sizeof(op.mcc));
	g_strlcpy(op.mnc, mnc, sizeof(op.mnc));

	l = g_slist_find_custom(sim->opl, &op, opl_operator_compare);
	if (!l)
		return NULL;
	opl_op = l->data;

	return sim->pnn[opl_op->id - 1].longname;
}

static gboolean pnn_operator_parse(struct pnn_operator *oper,
				const guint8 *tlv, int length)
{
	const char *name;
	int namelength;

	name = ber_tlv_find_by_tag(tlv, 0x43, length, &namelength);
	if (!name || !namelength)
		return FALSE;
	oper->longname = network_name_parse(name, namelength);

	name = ber_tlv_find_by_tag(tlv, 0x45, length, &namelength);
	if (name && namelength)
		oper->shortname = network_name_parse(name, namelength);

	if (ber_tlv_find_by_tag(tlv, 0x80, length, &namelength))
		ofono_debug("%i octets of addition PLMN information "
				"present in EF-PNN");

	return TRUE;
}

static void sim_pnn_read_cb(const struct ofono_error *error,
		const unsigned char *pnndata, int length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	struct opl_operator *oper;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto skip;

	if (length < sim->pnn_size)
		goto skip;

	pnn_operator_parse(&sim->pnn[sim->pnn_current], pnndata, length);

skip:
	sim->pnn_current ++;
	if (sim->pnn_current < sim->pnn_num)
		sim->ops->read_file_linear(modem, SIM_EFPNN_FILEID,
						sim->pnn_current,
						sim->pnn_size,
						sim_pnn_read_cb, modem);
	else
		/* All records retrieved */
		/* We now need EF-OPL if it's there for PNN to be
		 * useful.  */
		sim_retrieve_opl(modem);
}

static void sim_pnn_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int record_length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || length < 3 ||
			record_length < 3 ||
			structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		/* If PNN is not present then OPL is not useful, don't
		 * retrieve it.  If OPL is not there then PNN[1] will
		 * still be used for the HPLMN and/or EHPLMN, if PNN
		 * is present.  */
		return;

	sim->pnn_current = 0;
	sim->pnn_size = record_length;
	sim->pnn_num = length / record_length;
	sim->pnn = g_new0(struct pnn_operator, sim->pnn_num);
	sim->ops->read_file_linear(modem, SIM_EFPNN_FILEID, 0,
			record_length, sim_pnn_read_cb, modem);
}

static gboolean sim_retrieve_pnn(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;

	sim->ops->read_file_info(modem, SIM_EFPNN_FILEID,
			sim_pnn_info_cb, modem);

	return FALSE;
}

static void sim_ready(struct ofono_modem *modem)
{
	ofono_sim_read(modem, SIM_EFMSISDN_FILEID, sim_msisdn_read_cb, NULL);
}

static void sim_imsi_cb(const struct ofono_error *error, const char *imsi,
		void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Unable to read IMSI, emergency calls only");
		return;
	}

	sim->imsi = g_strdup(imsi);

	ofono_sim_ready(modem);
}

static gboolean sim_retrieve_imsi(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;

	if (!sim->ops->read_imsi) {
		ofono_error("IMSI retrieval not implemented,"
				" only emergency calls will be available");
		return FALSE;
	}

	sim->ops->read_imsi(modem, sim_imsi_cb, modem);

	return FALSE;
}

static void sim_op_error(struct ofono_modem *modem)
{
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op = g_queue_pop_head(sim->simop_q);

	op->cb(modem, 0, 0, 0, 0, 0, 0, op->userdata);

	sim_file_op_free(op);

	if (g_queue_get_length(sim->simop_q) > 0)
		g_timeout_add(0, sim_op_next, modem);
}

static void sim_op_retrieve_cb(const struct ofono_error *error,
				const unsigned char *data, int len, void *user)
{
	struct ofono_modem *modem = user;
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);
	int total = op->length / op->record_length;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		if (op->current == 1)
			sim_op_error(modem);

		return;
	}

	op->cb(modem, 1, op->structure, op->length, op->current,
		data, op->record_length, op->userdata);

	if (op->current == total) {
		op = g_queue_pop_head(sim->simop_q);

		sim_file_op_free(op);

		if (g_queue_get_length(sim->simop_q) > 0)
			g_timeout_add(0, sim_op_next, modem);
	} else {
		op->current += 1;
		g_timeout_add(0, sim_op_retrieve_next, modem);
	}
}

static gboolean sim_op_retrieve_next(gpointer user)
{
	struct ofono_modem *modem = user;
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);

	switch (op->structure) {
	case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:
		if (!sim->ops->read_file_transparent) {
			sim_op_error(modem);
			return FALSE;
		}

		sim->ops->read_file_transparent(modem, op->id, 0, op->length,
						sim_op_retrieve_cb, modem);
		break;
	case OFONO_SIM_FILE_STRUCTURE_FIXED:
		if (!sim->ops->read_file_linear) {
			sim_op_error(modem);
			return FALSE;
		}

		sim->ops->read_file_linear(modem, op->id, op->current,
						op->record_length,
						sim_op_retrieve_cb, modem);
		break;
	case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
		if (!sim->ops->read_file_cyclic) {
			sim_op_error(modem);
			return FALSE;
		}

		sim->ops->read_file_cyclic(modem, op->id, op->current,
						op->record_length,
						sim_op_retrieve_cb, modem);
		break;
	default:
		ofono_error("Unrecognized file structure, this can't happen");
	}

	return FALSE;
}

static void sim_op_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int record_length, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		sim_op_error(modem);
		return;
	}

	op->structure = structure;
	op->length = length;
	op->record_length = record_length;
	op->current = 1;

	g_timeout_add(0, sim_op_retrieve_next, modem);
}

static gboolean sim_op_next(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op;

	if (!sim->simop_q)
		return FALSE;

	op = g_queue_peek_head(sim->simop_q);

	sim->ops->read_file_info(modem, op->id, sim_op_info_cb, modem);

	return FALSE;
}

int ofono_sim_read(struct ofono_modem *modem, int id,
			ofono_sim_file_read_cb_t cb, void *data)
{
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op;

	if (!cb)
		return -1;

	if (modem->sim_manager == NULL)
		return -1;

	if (!sim->ops)
		return -1;

	if (!sim->ops->read_file_info)
		return -1;

	/* TODO: We must first check the EFust table to see whether
	 * this file can be read at all
	 */

	if (!sim->simop_q)
		sim->simop_q = g_queue_new();

	op = g_new0(struct sim_file_op, 1);

	op->id = id;
	op->cb = cb;
	op->userdata = data;

	g_queue_push_tail(sim->simop_q, op);

	if (g_queue_get_length(sim->simop_q) == 1)
		g_timeout_add(0, sim_op_next, modem);

	return 0;
}

int ofono_sim_write(struct ofono_modem *modem, int id,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length)
{
	return -1;
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

	ofono_sim_ready_notify_register(modem, sim_ready);

	/* Perform SIM initialization according to 3GPP 31.102 Section 5.1.1.2
	 * The assumption here is that if sim manager is being initialized,
	 * then sim commands are implemented, and the sim manager is then
	 * responsible for checking the PIN, reading the IMSI and signaling
	 * SIM ready condition.
	 *
	 * The procedure according to 31.102 is roughly:
	 * Read EFecc
	 * Read EFli and EFpl
	 * SIM Pin check
	 * Read EFust
	 * Read EFest
	 * Read IMSI
	 *
	 * At this point we signal the SIM ready condition and allow
	 * arbitrary files to be written or read, assuming their presence
	 * in the EFust
	 */
	g_timeout_add(0, sim_retrieve_imsi, modem);
}

const char *ofono_sim_get_imsi(struct ofono_modem *modem)
{
	if (modem->sim_manager == NULL)
		return NULL;

	return modem->sim_manager->imsi;
}

int ofono_sim_ready_notify_register(struct ofono_modem *modem,
					ofono_sim_ready_notify_cb_t cb)
{
	if (modem->sim_manager == NULL)
		return -1;

	modem->sim_manager->ready_notify =
		g_slist_append(modem->sim_manager->ready_notify, cb);

	return 0;
}

void ofono_sim_ready_notify_unregister(struct ofono_modem *modem,
					ofono_sim_ready_notify_cb_t cb)
{
	if (modem->sim_manager == NULL)
		return;

	modem->sim_manager->ready_notify =
		g_slist_remove(modem->sim_manager->ready_notify, cb);
}

int ofono_sim_get_ready(struct ofono_modem *modem)
{
	if (modem->sim_manager == NULL)
		return 0;

	if (modem->sim_manager->ready == TRUE)
		return 1;

	return 0;
}

void ofono_sim_set_ready(struct ofono_modem *modem)
{
	GSList *l;

	if (modem->sim_manager == NULL)
		return;

	if (modem->sim_manager->ready == TRUE)
		return;

	modem->sim_manager->ready = TRUE;

	for (l = modem->sim_manager->ready_notify; l; l = l->next) {
		ofono_sim_ready_notify_cb_t cb = l->data;

		cb(modem);
	}
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
