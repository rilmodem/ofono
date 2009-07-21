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
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "ofono.h"

#include "dbus-gsm.h"
#include "modem.h"
#include "driver.h"
#include "common.h"
#include "util.h"
#include "smsutil.h"
#include "sim.h"
#include "simutil.h"

#define SIM_MANAGER_INTERFACE "org.ofono.SimManager"

static gboolean sim_op_next(gpointer user_data);
static gboolean sim_op_retrieve_next(gpointer user);

struct sim_file_op {
	int id;
	int cache;
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

static GDBusSignalTable sim_manager_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};


static void sim_msisdn_read_cb(struct ofono_modem *modem, int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct sim_manager_data *sim = modem->sim_manager;
	int total;
	struct ofono_phone_number *ph;
	int number_len;
	int ton_npi;

	if (!ok)
		return;

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

	if (record_length < 14 || length < record_length)
		return;

	total = length / record_length;

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

	ofono_sim_set_ready(modem);
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

static int create_dirs(const char *filename, const mode_t mode)
{
	struct stat st;
	char *dir;
	const char *prev, *next;
	int err;

	err = stat(filename, &st);
	if (!err && S_ISREG(st.st_mode))
		return 0;

	dir = g_malloc(strlen(filename) + 1);
	strcpy(dir, "/");

	for (prev = filename; (next = strchr(prev + 1, '/')); prev = next)
		if (next > prev + 1) {
			strncat(dir, prev + 1, next - prev);

			if (mkdir(dir, mode) && errno != EEXIST) {
				g_free(dir);
				return -1;
			}
		}

	g_free(dir);
	return 0;
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

#define SIM_CACHE_MODE 0600
#define SIM_CACHE_PATH CONFIG_LOCALSTATEDIR "/lib/ofono/%s/%04x"
#define SIM_CACHE_PATH_LEN(imsilen) (strlen(SIM_CACHE_PATH) - 2 + imsilen)
#define SIM_CACHE_HEADER_SIZE 6

static void sim_op_retrieve_cb(const struct ofono_error *error,
				const unsigned char *data, int len, void *user)
{
	struct ofono_modem *modem = user;
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);
	int total = op->length / op->record_length;

	char *imsi = sim->imsi;
	char *path;
	int fd;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		if (op->current == 1)
			sim_op_error(modem);

		return;
	}

	op->cb(modem, 1, op->structure, op->length, op->current,
		data, op->record_length, op->userdata);

	if (op->cache && imsi) {
		/* Cache the record */
		path = g_strdup_printf(SIM_CACHE_PATH, imsi, op->id);
		fd = open(path, O_WRONLY);
		g_free(path);

		if (fd == -1)
			goto next;

		if (lseek(fd, (op->current - 1) * op->record_length +
					SIM_CACHE_HEADER_SIZE, SEEK_SET) !=
				(off_t) -1)
			write(fd, data, op->record_length);
		close(fd);
	}

next:
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
				int record_length,
				enum ofono_sim_file_access *access, void *data)
{
	struct ofono_modem *modem = data;
	struct sim_manager_data *sim = modem->sim_manager;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);

	char *imsi = sim->imsi;
	char *path;
	unsigned char fileinfo[6];
	int fd = -1;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		sim_op_error(modem);
		return;
	}

	op->structure = structure;
	op->length = length;
	/* Never cache card holder writable files */
	op->cache = (
			access[OFONO_SIM_FILE_CONDITION_UPDATE] ==
			OFONO_SIM_FILE_ACCESS_ADM ||
			access[OFONO_SIM_FILE_CONDITION_UPDATE] ==
			OFONO_SIM_FILE_ACCESS_NEVER);

	if (structure == OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		op->record_length = length;
	else
		op->record_length = record_length;

	op->current = 1;

	g_timeout_add(0, sim_op_retrieve_next, modem);

	if (op->cache && imsi) {
		path = g_strdup_printf(SIM_CACHE_PATH, imsi, op->id);
		if (create_dirs(path, SIM_CACHE_MODE | S_IXUSR) == 0)
			fd = open(path, O_WRONLY | O_CREAT, SIM_CACHE_MODE);
		g_free(path);

		if (fd == -1) {
			ofono_debug("Error %i creating cache file for "
					"fileid %04x, IMSI %s",
					errno, op->id, imsi);
			return;
		}

		fileinfo[0] = error->type;
		fileinfo[1] = length >> 8;
		fileinfo[2] = length & 0xff;
		fileinfo[3] = structure;
		fileinfo[4] = record_length >> 8;
		fileinfo[5] = record_length & 0xff;

		write(fd, fileinfo, 6);
		close(fd);
	}
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

struct sim_cache_callback {
	ofono_sim_file_read_cb_t cb;
	void *userdata;
	struct ofono_modem *modem;
	int error;
	int fd;
	enum ofono_sim_file_structure structure;
	unsigned int record_length;
	unsigned int total;
};

static gboolean sim_op_cached_callback(gpointer user)
{
	struct sim_cache_callback *cbs = user;
	guint8 buffer[cbs->record_length];
	unsigned int record;

	if (cbs->error != OFONO_ERROR_TYPE_NO_ERROR) {
		cbs->cb(cbs->modem, 0, 0, 0, 0, 0, 0, 0);
		goto cleanup;
	}

	for (record = 0; record < cbs->total; record++) {
		if (read(cbs->fd, buffer, cbs->record_length) <
				(int) cbs->record_length) {
			cbs->cb(cbs->modem, 0, 0, 0, 0, 0, 0, 0);
			break;
		}

		cbs->cb(cbs->modem, 1, cbs->structure,
				cbs->record_length * cbs->total, record + 1,
				buffer, cbs->record_length, cbs->userdata);
	}

cleanup:
	close(cbs->fd);
	g_free(cbs);

	return FALSE;
}

static gboolean sim_op_check_cached(struct ofono_modem *modem, int fileid,
			ofono_sim_file_read_cb_t cb, void *data)
{
	struct sim_manager_data *sim = modem->sim_manager;
	char *imsi = sim->imsi;
	char *path;
	int fd;
	unsigned char fileinfo[SIM_CACHE_HEADER_SIZE];
	ssize_t len;
	struct ofono_error error;
	unsigned int file_length;
	enum ofono_sim_file_structure structure;
	unsigned int record_length;
	struct sim_cache_callback *cbs;

	if (!imsi)
		return FALSE;

	path = g_strdup_printf(SIM_CACHE_PATH, imsi, fileid);
	fd = open(path, O_RDONLY);
	g_free(path);

	if (fd == -1) {
		if (errno != ENOENT)
			ofono_debug("Error %i opening cache file for "
					"fileid %04x, IMSI %s",
					errno, fileid, imsi);

		return FALSE;
	}

	len = read(fd, fileinfo, SIM_CACHE_HEADER_SIZE);
	if (len != SIM_CACHE_HEADER_SIZE)
		return FALSE;

	error.type = fileinfo[0];
	file_length = (fileinfo[1] << 8) | fileinfo[2];
	structure = fileinfo[3];
	record_length = (fileinfo[4] << 8) | fileinfo[5];

	if (structure == OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		record_length = file_length;
	if (record_length == 0 || file_length < record_length)
		return FALSE;

	cbs = g_new(struct sim_cache_callback, 1);
	cbs->cb = cb;
	cbs->userdata = data;
	cbs->modem = modem;
	cbs->error = error.type;
	cbs->fd = fd;
	cbs->structure = structure;
	cbs->record_length = record_length;
	cbs->total = file_length / record_length;
	g_timeout_add(0, sim_op_cached_callback, cbs);

	return TRUE;
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

	if (sim_op_check_cached(modem, id, cb, data))
		return 0;

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
