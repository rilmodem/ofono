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

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ofono.h"

#include "common.h"
#include "util.h"
#include "simutil.h"
#include "smsutil.h"

struct mailbox_state {
	gboolean indication;
	unsigned char message_count;
};

struct ofono_message_waiting {
	struct mailbox_state messages[5];
	unsigned char efmwis_length;
	unsigned char efmbdn_length;
	unsigned char efmbdn_record_id[5];
	unsigned int efmbdn_watch;
	unsigned char ef_cphs_mwis_length;
	unsigned char ef_cphs_mbdn_length;
	unsigned int ef_cphs_mbdn_watch;
	gboolean mbdn_not_provided;
	gboolean cphs_mbdn_not_provided;
	struct ofono_phone_number mailbox_number[5];
	struct ofono_sim *sim;
	struct ofono_sim_context *sim_context;
	struct ofono_atom *atom;
};

struct mbdn_set_request {
	struct ofono_message_waiting *mw;
	int mailbox;
	struct ofono_phone_number number;
	DBusMessage *msg;
	gboolean cphs;
};

static const char *mw_message_waiting_property_name[5] = {
	"VoicemailWaiting",
#if 0
	"FaxWaiting",
	"EmailWaiting",
	"OtherWaiting",
	"VideomailWaiting",
#endif
};

static const char *mw_message_count_property_name[5] = {
	"VoicemailMessageCount",
#if 0
	"FaxMessageCount",
	"EmailMessageCount",
	"OtherMessageCount",
	"VideomailMessageCount",
#endif
};

static const char *mw_mailbox_property_name[5] = {
	"VoicemailMailboxNumber",
#if 0
	"FaxMailboxNumber",
	"EmailMailboxNumber",
	"OtherMailboxNumber",
	"VideomailMailboxNumber",
#endif
};

static const int mw_mailbox_to_cphs_record[5] = {
	1, /* Line 1 mailbox */
	4, /* Fax mailbox */
	0,
	3, /* Data mailbox */
	0,
};

static void mbdn_set_cb(int ok, void *data);

static DBusMessage *mw_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_message_waiting *mw = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	int i;
	dbus_bool_t indication;
	unsigned char count;
	const char *number;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			OFONO_PROPERTIES_ARRAY_SIGNATURE, &dict);

	for (i = 0; i < 5; i++) {
		if (mw_message_waiting_property_name[i]) {
			indication = mw->messages[i].indication;

			ofono_dbus_dict_append(&dict,
					mw_message_waiting_property_name[i],
					DBUS_TYPE_BOOLEAN, &indication);
		}

		if (mw_message_count_property_name[i]) {
			count = mw->messages[i].message_count;

			ofono_dbus_dict_append(&dict,
					mw_message_count_property_name[i],
					DBUS_TYPE_BYTE, &count);
		}

		if (mw_mailbox_property_name[i]) {
			number = phone_number_to_string(&mw->mailbox_number[i]);

			ofono_dbus_dict_append(&dict,
					mw_mailbox_property_name[i],
					DBUS_TYPE_STRING, &number);
		}
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void cphs_mbdn_sync_cb(int ok, void *data)
{
	struct mbdn_set_request *req = data;

	if (!ok)
		ofono_info("Failed to synchronize CPHS MBDN record");

	g_free(req);
}

static DBusMessage *set_cphs_mbdn(struct ofono_message_waiting *mw,
					gboolean sync,
					int mailbox,
					const char *number,
					DBusMessage *msg)
{
	struct mbdn_set_request *req;
	unsigned char efmbdn[255];

	if ((mw->ef_cphs_mbdn_length && !mw_mailbox_to_cphs_record[mailbox]) ||
			mw->cphs_mbdn_not_provided == TRUE) {
		if (msg)
			return __ofono_error_not_supported(msg);

		return NULL;
	}

	if (mw->ef_cphs_mbdn_length == 0) {
		if (msg)
			return __ofono_error_sim_not_ready(msg);

		return NULL;
	}

	req = g_new0(struct mbdn_set_request, 1);

	req->mw = mw;
	req->mailbox = mailbox;
	string_to_phone_number(number, &req->number);
	req->cphs = TRUE;

	sim_adn_build(efmbdn, req->mw->ef_cphs_mbdn_length,
			&req->number, NULL);

	if (ofono_sim_write(mw->sim_context, SIM_EF_CPHS_MBDN_FILEID,
			sync ? cphs_mbdn_sync_cb : mbdn_set_cb,
			OFONO_SIM_FILE_STRUCTURE_FIXED,
			mw_mailbox_to_cphs_record[mailbox],
			efmbdn, mw->ef_cphs_mbdn_length, req) == -1) {
		g_free(req);

		if (msg)
			return __ofono_error_failed(msg);
	} else
		req->msg = msg ? dbus_message_ref(msg) : NULL;

	return NULL;
}

static void mbdn_set_cb(int ok, void *data)
{
	struct mbdn_set_request *req = data;
	struct ofono_phone_number *old = &req->mw->mailbox_number[req->mailbox];
	const char *property;
	DBusMessage *reply = NULL;

	if (!ok) {
		if (req->msg)
			reply = __ofono_error_failed(req->msg);

		goto out;
	}

	if (req->msg)
		reply = dbus_message_new_method_return(req->msg);

	if (g_str_equal(req->number.number, old->number) &&
			req->number.type == old->type)
		goto out;

	memcpy(old, &req->number, sizeof(struct ofono_phone_number));

	property = mw_mailbox_property_name[req->mailbox];

	if (property) {
		DBusConnection *conn = ofono_dbus_get_connection();
		const char *path = __ofono_atom_get_path(req->mw->atom);
		const char *number;

		number = phone_number_to_string(old);

		ofono_dbus_signal_property_changed(conn, path,
						OFONO_MESSAGE_WAITING_INTERFACE,
						property, DBUS_TYPE_STRING,
						&number);
	}

	/*
	 * Make a single attempt at keeping the CPHS version of the file
	 * in sync.
	 */
	if (req->cphs == FALSE)
		set_cphs_mbdn(req->mw, TRUE, req->mailbox,
				phone_number_to_string(&req->number), NULL);

out:
	if (req->msg && reply)
		__ofono_dbus_pending_reply(&req->msg, reply);

	g_free(req);
}

static DBusMessage *set_mbdn(struct ofono_message_waiting *mw, int mailbox,
			const char *number, DBusMessage *msg)
{
	struct mbdn_set_request *req;
	unsigned char efmbdn[255];

	/*
	 * If we have no 3GPP EFmbdn on the card, maybe the
	 * CPHS version is available
	 */
	if ((mw->efmbdn_length > 0 && mw->efmbdn_record_id[mailbox] == 0) ||
			mw->mbdn_not_provided == TRUE)
		return set_cphs_mbdn(mw, FALSE, mailbox, number, msg);

	if (mw->efmbdn_length == 0) {
		if (msg)
			return __ofono_error_sim_not_ready(msg);

		return NULL;
	}

	req = g_new0(struct mbdn_set_request, 1);

	req->mw = mw;
	req->mailbox = mailbox;
	string_to_phone_number(number, &req->number);
	req->cphs = FALSE;

	sim_adn_build(efmbdn, req->mw->efmbdn_length, &req->number, NULL);

	if (ofono_sim_write(req->mw->sim_context, SIM_EFMBDN_FILEID,
				mbdn_set_cb, OFONO_SIM_FILE_STRUCTURE_FIXED,
				req->mw->efmbdn_record_id[mailbox],
				efmbdn, req->mw->efmbdn_length, req) == -1) {
		g_free(req);

		if (msg)
			return __ofono_error_failed(msg);
	} else
		req->msg = msg ? dbus_message_ref(msg) : NULL;

	return NULL;
}

static DBusMessage *mw_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_message_waiting *mw = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name, *value;
	int i;

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);

	for (i = 0; i < 5; i++)
		if (mw_mailbox_property_name[i] &&
				!strcmp(name, mw_mailbox_property_name[i]))
			break;

	if (i < 5) {
		const char *cur_number;

		dbus_message_iter_next(&iter);

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_recurse(&iter, &var);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (!valid_phone_number_format(value))
			return __ofono_error_invalid_format(msg);

		cur_number = phone_number_to_string(&mw->mailbox_number[i]);

		if (g_str_equal(cur_number, value))
			return dbus_message_new_method_return(msg);

		return set_mbdn(mw, i, value, msg);
	}

	return __ofono_error_invalid_args(msg);
}

static GDBusMethodTable message_waiting_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	mw_get_properties	},
	{ "SetProperty",	"sv",	"",		mw_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable message_waiting_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static void update_indicator_and_emit(struct ofono_message_waiting *mw,
					int mailbox,
					struct mailbox_state *info)
{
	dbus_bool_t indication;
	unsigned char count;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(mw->atom);

	if (mw->messages[mailbox].message_count == info->message_count &&
			mw->messages[mailbox].indication == info->indication)
		return;

	memcpy(&mw->messages[mailbox], info, sizeof(struct mailbox_state));

	indication = info->indication;
	count = info->message_count;

	if (mw_message_waiting_property_name[mailbox] == NULL)
		return;

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_MESSAGE_WAITING_INTERFACE,
				mw_message_waiting_property_name[mailbox],
				DBUS_TYPE_BOOLEAN, &indication);

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_MESSAGE_WAITING_INTERFACE,
				mw_message_count_property_name[mailbox],
				DBUS_TYPE_BYTE, &count);
}

static void mw_cphs_mwis_read_cb(int ok, int total_length, int record,
					const unsigned char *data,
					int record_length, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;
	struct mailbox_state info;
	unsigned char indication;

	if (!ok || total_length < 1) {
		DBG("No CPHS MWIS on SIM");
		mw->ef_cphs_mwis_length = 0;
		return;
	}

	mw->ef_cphs_mwis_length = total_length;

	if (mw->efmwis_length != 0)
		return;

	/* Read Line 1 indication */
	indication = data[0] & 0xf;
	info.indication = (indication == 0xa);
	info.message_count = 0;
	update_indicator_and_emit(mw, 0, &info);

	if (total_length == 1)
		return;

	/* Read Fax indication */
	indication = data[1] & 0xf;
	info.indication = (indication == 0xa);
	info.message_count = 0;
	update_indicator_and_emit(mw, 1, &info);

	/* Read Data indication, map to 'Other' */
	indication = (data[1] >> 4) & 0xf;
	info.indication = (indication == 0xa);
	info.message_count = 0;
	update_indicator_and_emit(mw, 3, &info);
}

static void mw_mwis_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;
	int i, status;
	struct mailbox_state info;

	if (!ok || record_length < 5) {
		ofono_error("Unable to read waiting messages numbers "
				"from SIM");

		mw->efmwis_length = 0;

		return;
	}

	/* Handle only current identity (TODO: currently assumes first) */
	if (record != 1)
		return;

	status = data[0];
	data++;

	for (i = 0; i < 5 && i < record_length - 1; i++) {
		info.indication = (status >> i) & 1;
		info.message_count = info.indication ? data[0] : 0;

		update_indicator_and_emit(mw, i, &info);
	}

	mw->efmwis_length = record_length;
}

static void mw_cphs_mbdn_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;
	int i;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *value;

	if (!ok || record_length < 14 || total_length < record_length) {
		ofono_error("Unable to read CPHS mailbox dialling numbers "
				"from SIM");

		mw->ef_cphs_mbdn_length = 0;
		mw->cphs_mbdn_not_provided = TRUE;
		return;
	}

	for (i = 0; i < 5; i++)
		if (record == mw_mailbox_to_cphs_record[i])
			break;

	if (i == 5)
		return;

	mw->ef_cphs_mbdn_length = record_length;

	if (mw->mbdn_not_provided != TRUE)
		return;

	ofono_info("3GPP MBDN not provided, parsing CPHS..");

	if (sim_adn_parse(data, record_length, &mw->mailbox_number[i], NULL) ==
			FALSE)
		mw->mailbox_number[i].number[0] = '\0';

	if (mw_mailbox_property_name[i]) {
		const char *path = __ofono_atom_get_path(mw->atom);

		value = phone_number_to_string(&mw->mailbox_number[i]);

		ofono_dbus_signal_property_changed(conn, path,
				OFONO_MESSAGE_WAITING_INTERFACE,
				mw_mailbox_property_name[i],
				DBUS_TYPE_STRING, &value);
	}
}

static void mw_mbdn_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;
	int i;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *value;

	if (!ok || record_length < 14 || total_length < record_length) {
		ofono_error("Unable to read mailbox dialling numbers "
				"from SIM");

		mw->efmbdn_length = 0;
		mw->mbdn_not_provided = TRUE;
		return;
	}

	for (i = 0; i < 5; i++)
		if (record == mw->efmbdn_record_id[i])
			break;

	if (i == 5)
		return;

	if (sim_adn_parse(data, record_length, &mw->mailbox_number[i], NULL) ==
			FALSE)
		mw->mailbox_number[i].number[0] = '\0';

	if (mw_mailbox_property_name[i]) {
		const char *path = __ofono_atom_get_path(mw->atom);

		value = phone_number_to_string(&mw->mailbox_number[i]);

		ofono_dbus_signal_property_changed(conn, path,
				OFONO_MESSAGE_WAITING_INTERFACE,
				mw_mailbox_property_name[i],
				DBUS_TYPE_STRING, &value);
	}

	mw->efmbdn_length = record_length;
}

static void mw_mbdn_changed(int id, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;
	int err;

	mw->efmbdn_length = 0;
	mw->mbdn_not_provided = FALSE;

	err = ofono_sim_read(mw->sim_context, SIM_EFMBDN_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				mw_mbdn_read_cb, mw);
	if (err != 0)
		ofono_error("Unable to read EF-MBDN from SIM");
}

static void mw_cphs_mbdn_changed(int id, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;

	mw->ef_cphs_mbdn_length = 0;
	mw->cphs_mbdn_not_provided = FALSE;

	ofono_sim_read(mw->sim_context, SIM_EF_CPHS_MBDN_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED,
			mw_cphs_mbdn_read_cb, mw);
}

const struct ofono_phone_number *__ofono_message_waiting_get_mbdn(
					struct ofono_message_waiting *mw,
					unsigned int index)
{
	return &mw->mailbox_number[index];
}

static void mw_mbi_read_cb(int ok, int total_length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;
	int i, err;

	if (!ok || record_length < 4) {
		ofono_error("Unable to read mailbox identifies "
				"from SIM");

		mw->efmbdn_length = 0;
		mw->mbdn_not_provided = TRUE;

		goto out;
	}

	/* Handle only current identity (TODO: currently assumes first) */
	if (record != 1)
		return;

	for (i = 0; i < 5 && i < record_length; i++)
		mw->efmbdn_record_id[i] = data[i];

	err = ofono_sim_read(mw->sim_context, SIM_EFMBDN_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				mw_mbdn_read_cb, mw);
	mw->efmbdn_watch = ofono_sim_add_file_watch(mw->sim_context,
						SIM_EFMBDN_FILEID,
						mw_mbdn_changed, mw, NULL);

	if (err != 0)
		ofono_error("Unable to read EF-MBDN from SIM");

out:
	/*
	 * Mailbox numbers located in Byte 1, bits 6 & 5,
	 * Check for Activated & Allocated
	 */
	if (__ofono_sim_cphs_service_available(mw->sim,
					SIM_CPHS_SERVICE_MAILBOX_NUMBERS)) {
		ofono_sim_read(mw->sim_context, SIM_EF_CPHS_MBDN_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				mw_cphs_mbdn_read_cb, mw);
		mw->ef_cphs_mbdn_watch = ofono_sim_add_file_watch(
						mw->sim_context,
						SIM_EF_CPHS_MBDN_FILEID,
						mw_cphs_mbdn_changed, mw, NULL);
	}
}

static void mw_mwis_write_cb(int ok, void *userdata)
{
	if (!ok)
		ofono_error("Writing new EF-MWIS failed");
}

static void mw_set_indicator(struct ofono_message_waiting *mw, int profile,
				enum sms_mwi_type type,
				gboolean present, unsigned char messages)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	unsigned char efmwis[255];  /* Max record size */
	int i;

	if (mw == NULL)
		return;

	/* Handle only current identity (TODO: currently assumes first) */
	if (profile != 1)
		return;

	if (mw->messages[type].indication == present &&
			mw->messages[type].message_count == messages)
		return;

	if (mw->messages[type].indication != present) {
		dbus_bool_t indication;
		const char *path = __ofono_atom_get_path(mw->atom);

		indication = present;
		mw->messages[type].indication = present;

		if (mw_message_waiting_property_name[type])
			ofono_dbus_signal_property_changed(conn, path,
					OFONO_MESSAGE_WAITING_INTERFACE,
					mw_message_waiting_property_name[type],
					DBUS_TYPE_BOOLEAN, &indication);
	}

	if (mw->messages[type].message_count != messages) {
		const char *path = __ofono_atom_get_path(mw->atom);

		mw->messages[type].message_count = messages;

		if (mw_message_waiting_property_name[type])
			ofono_dbus_signal_property_changed(conn, path,
					OFONO_MESSAGE_WAITING_INTERFACE,
					mw_message_count_property_name[type],
					DBUS_TYPE_BYTE, &messages);
	}

	/* Writes MWI states and/or MBDN back to SIM */
	if (mw->efmwis_length < 5) {
		if (mw->ef_cphs_mwis_length >= 1)
			goto try_cphs;

		ofono_error("Unable to update MWIS indicator");
		return;
	}

	/* Fill in numbers of messages in bytes 1 to X of EF-MWIS */
	for (i = 0; i < 5 && i < mw->efmwis_length - 1; i++)
		efmwis[i + 1] = mw->messages[i].message_count;

	/* Fill in indicator state bits in byte 0 */
	for (i = 0; i < 5 && i < mw->efmwis_length - 1; i++)
		if (mw->messages[i].indication)
			efmwis[0] |= 1 << i;

	if (ofono_sim_write(mw->sim_context, SIM_EFMWIS_FILEID,
				mw_mwis_write_cb,
				OFONO_SIM_FILE_STRUCTURE_FIXED, 1,
				efmwis, mw->efmwis_length, mw) != 0) {
		ofono_error("Queuing a EF-MWI write to SIM failed");
	}

	if (mw->ef_cphs_mwis_length == 0)
		return;

try_cphs:
	memset(efmwis, 0x55, 255);

	efmwis[0] = mw->messages[0].indication ? 0xa : 0x5;

	if (mw->ef_cphs_mwis_length > 1)
		efmwis[1] = mw->messages[1].indication ? 0xa : 0x5 |
			mw->messages[3].indication ? 0xa0 : 0x50;

	if (ofono_sim_write(mw->sim_context, SIM_EF_CPHS_MWIS_FILEID,
				mw_mwis_write_cb,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT, 0,
				efmwis, mw->ef_cphs_mwis_length, mw) != 0)
		ofono_error("Queuing a EF-MWIS write to SIM failed (CPHS)");
}

static void handle_special_sms_iei(struct ofono_message_waiting *mw,
					const guint8 *iei, gboolean *discard)
{
	enum sms_mwi_type type;
	int profile;
	gboolean set;

	/* Parse type & storage byte */
	if (discard)
		*discard = (iei[0] & (1 << 7)) ? FALSE : TRUE;

	type = iei[0] & 0x1f;
	if (type > SMS_MWI_TYPE_OTHER) {
		if (type == (SMS_MWI_TYPE_OTHER | 4))
			type = SMS_MWI_TYPE_VIDEO;
		else
			/*
			 * 23.040 9.2.3.24.2: "Terminals should be capable of
			 * receiving any values in octet 1, even including
			 * those marked as Reserved."  Treat Reserved as
			 * "Other".
			 */
			type = SMS_MWI_TYPE_OTHER;
	}

	set = iei[1] > 0 ? TRUE : FALSE;
	profile = ((iei[0] >> 5) & 3) + 1;

	mw_set_indicator(mw, profile, type, set, iei[1]);
}

static void handle_enhanced_voicemail_iei(struct ofono_message_waiting *mw,
						const guint8 *iei,
						gboolean *discard, int length)
{
	int profile, n;
	gboolean set;
	struct sms_address mailbox_address;

	if (length < 3)
		return;

	/* ENHANCED_VOICE_MAIL_PDU_TYPE */
	if (!(iei[0] & 1)) {
		/* 9.2.3.24.13.1 Enhanced Voice Mail Notification */

		/* MULTIPLE_SUBSCRIBER_PROFILE */
		profile = ((iei[0] >> 2) & 3) + 1;

		/* SM_STORAGE */
		if (discard)
			*discard = (iei[0] & (1 << 4)) ? FALSE : TRUE;

		/* VM_MAILBOX_ACCESS_ADDRESS */
		n = 0;
		if (!sms_decode_address_field(iei + 1, length - 1, &n,
					FALSE, &mailbox_address))
			return;

		/* TODO: VM_MESSAGE_PRIORITY_INDICATION */

		/* Other parameters currently not supported */

		if (length < n + 3)
			return;

		set = iei[n + 1] > 0 ? TRUE : FALSE;
		mw_set_indicator(mw, profile, SMS_MWI_TYPE_VOICE,
					set, iei[n + 1]);
	} else {
		/* 9.2.3.24.13.2 Enhanced Voice Delete Confirmation */

		/* MULTIPLE_SUBSCRIBER_PROFILE */
		profile = ((iei[0] >> 2) & 3) + 1;

		/* SM_STORAGE */
		if (discard)
			*discard = (iei[0] & (1 << 4)) ? FALSE : TRUE;

		/* VM_MAILBOX_ACCESS_ADDRESS */
		n = 0;
		if (!sms_decode_address_field(iei + 1, length - 1, &n,
					FALSE, &mailbox_address))
			return;

		/* Other parameters currently not supported */

		if (length < n + 3)
			return;

		set = iei[n + 1] > 0 ? TRUE : FALSE;
		mw_set_indicator(mw, profile, SMS_MWI_TYPE_VOICE,
					set, iei[n + 1]);
	}

	if (mailbox_address.address[0] != '\0')
		set_mbdn(mw, SMS_MWI_TYPE_VOICE,
				sms_address_to_string(&mailbox_address), NULL);
}

void __ofono_message_waiting_mwi(struct ofono_message_waiting *mw,
					struct sms *sms, gboolean *out_discard)
{
	gboolean active, discard;
	enum sms_mwi_type type;
	int profile = 1, iei_found = 0;

	if (out_discard)
		*out_discard = FALSE;

	/*
	 * Check MWI types in the order from highest priority to lowest
	 * because they must override one another.
	 */

	if (sms->deliver.udhi) {
		guint8 evm_iei[140];
		struct sms_udh_iter iter;
		enum sms_iei iei;

		if (!sms_udh_iter_init(sms, &iter))
			return;

		while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
				SMS_IEI_INVALID) {
			switch (iei) {
			case SMS_IEI_ENHANCED_VOICE_MAIL_INFORMATION:
				sms_udh_iter_get_ie_data(&iter, evm_iei);

				handle_enhanced_voicemail_iei(mw, evm_iei,
						out_discard,
						sms_udh_iter_get_ie_length(
							&iter));
				return;
			default:
				break;
			}

			sms_udh_iter_next(&iter);
		}
	}

	if (sms->deliver.udhi) {
		guint8 special_iei[4];
		struct sms_udh_iter iter;
		enum sms_iei iei;

		if (!sms_udh_iter_init(sms, &iter))
			return;

		while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
				SMS_IEI_INVALID) {
			switch (iei) {
			case SMS_IEI_SPECIAL_MESSAGE_INDICATION:
				if (sms_udh_iter_get_ie_length(&iter) != 2)
					break;
				sms_udh_iter_get_ie_data(&iter, special_iei);

				handle_special_sms_iei(mw, special_iei,
						&discard);
				if (out_discard)
					*out_discard = *out_discard || discard;
				iei_found = 1;
				break;
			default:
				break;
			}

			sms_udh_iter_next(&iter);
		}

		if (iei_found) {
			/*
			 * 23.040 9.2.3.24.2 says "In the event of a
			 * conflict between this setting and the setting
			 * of the Data Coding Scheme (see 3GPP TS 23.038 [9])
			 * then the message shall be stored if either the DCS
			 * indicates this, or Octet 1 above indicates this."
			 */
			if (sms_mwi_dcs_decode(sms->deliver.dcs, NULL,
						NULL, NULL, &discard)) {
				if (out_discard)
					*out_discard = *out_discard || discard;
			}

			return;
		}
	}

	if (sms_mwi_dcs_decode(sms->deliver.dcs, &type,
				NULL, &active, out_discard)) {
		mw_set_indicator(mw, profile, type, active, 0);

		return;
	}

	if (sms->deliver.pid == SMS_PID_TYPE_RETURN_CALL)
		return;
}

static void message_waiting_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);
	struct ofono_message_waiting *mw = __ofono_atom_get_data(atom);

	if (mw->sim_context) {
		ofono_sim_context_free(mw->sim_context);
		mw->sim_context = NULL;
	}

	mw->sim = NULL;

	g_dbus_unregister_interface(conn, path,
					OFONO_MESSAGE_WAITING_INTERFACE);
	ofono_modem_remove_interface(modem, OFONO_MESSAGE_WAITING_INTERFACE);
}

static void mw_mwis_changed(int id, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;

	mw->efmwis_length = 0;

	ofono_sim_read(mw->sim_context, SIM_EFMWIS_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED,
			mw_mwis_read_cb, mw);
}

static void mw_cphs_mwis_changed(int id, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;

	mw->ef_cphs_mwis_length = 0;

	ofono_sim_read(mw->sim_context, SIM_EF_CPHS_MWIS_FILEID,
			OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
			mw_cphs_mwis_read_cb, mw);
}

static void mw_mbi_changed(int id, void *userdata)
{
	struct ofono_message_waiting *mw = userdata;

	mw->efmbdn_length = 0;
	mw->mbdn_not_provided = FALSE;

	mw->ef_cphs_mbdn_length = 0;
	mw->cphs_mbdn_not_provided = FALSE;

	ofono_sim_remove_file_watch(mw->sim_context, mw->efmbdn_watch);
	ofono_sim_remove_file_watch(mw->sim_context, mw->ef_cphs_mbdn_watch);

	ofono_sim_read(mw->sim_context, SIM_EFMBI_FILEID,
			OFONO_SIM_FILE_STRUCTURE_FIXED,
			mw_mbi_read_cb, mw);
}

void ofono_message_waiting_register(struct ofono_message_waiting *mw)
{
	DBusConnection *conn;
	const char *path;
	struct ofono_modem *modem;

	if (mw == NULL)
		return;

	conn = ofono_dbus_get_connection();
	modem = __ofono_atom_get_modem(mw->atom);
	path = __ofono_atom_get_path(mw->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_MESSAGE_WAITING_INTERFACE,
					message_waiting_methods,
					message_waiting_signals,
					NULL, mw, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_MESSAGE_WAITING_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_MESSAGE_WAITING_INTERFACE);

	mw->sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);
	if (mw->sim) {
		/* Assume that if sim atom exists, it is ready */
		mw->sim_context = ofono_sim_context_create(mw->sim);

		/* Loads MWI states and MBDN from SIM */
		ofono_sim_read(mw->sim_context, SIM_EFMWIS_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				mw_mwis_read_cb, mw);
		ofono_sim_read(mw->sim_context, SIM_EFMBI_FILEID,
				OFONO_SIM_FILE_STRUCTURE_FIXED,
				mw_mbi_read_cb, mw);

		/* Also read CPHS MWIS field */
		ofono_sim_read(mw->sim_context, SIM_EF_CPHS_MWIS_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
				mw_cphs_mwis_read_cb, mw);

		/*
		 * The operator could send us SMS mwi updates, but let's be
		 * extra careful and track the file contents too.
		 */
		ofono_sim_add_file_watch(mw->sim_context, SIM_EFMWIS_FILEID,
						mw_mwis_changed, mw, NULL);
		ofono_sim_add_file_watch(mw->sim_context,
						SIM_EF_CPHS_MWIS_FILEID,
						mw_cphs_mwis_changed, mw, NULL);

		ofono_sim_add_file_watch(mw->sim_context, SIM_EFMBI_FILEID,
						mw_mbi_changed, mw, NULL);
	}

	__ofono_atom_register(mw->atom, message_waiting_unregister);
}

static void mw_remove(struct ofono_atom *atom)
{
	struct ofono_message_waiting *mw = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (mw == NULL)
		return;

	g_free(mw);
}

struct ofono_message_waiting *ofono_message_waiting_create(struct ofono_modem *modem)
{
	struct ofono_message_waiting *mw;

	mw = g_try_new0(struct ofono_message_waiting, 1);

	if (mw == NULL)
		return NULL;

	mw->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPE_MESSAGE_WAITING,
					mw_remove, mw);

	return mw;
}

void ofono_message_waiting_remove(struct ofono_message_waiting *mw)
{
	__ofono_atom_free(mw->atom);
}
