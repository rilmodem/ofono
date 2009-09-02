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

#ifdef TEMP_FAILURE_RETRY
#define TFR TEMP_FAILURE_RETRY
#else
#define TFR
#endif

#define SIM_MANAGER_INTERFACE "org.ofono.SimManager"

#define SIM_CACHE_MODE 0600
#define SIM_CACHE_PATH STORAGEDIR "/%s/%04x"
#define SIM_CACHE_PATH_LEN(imsilen) (strlen(SIM_CACHE_PATH) - 2 + imsilen)
#define SIM_CACHE_HEADER_SIZE 6

static GSList *g_drivers = NULL;

static gboolean sim_op_next(gpointer user_data);
static gboolean sim_op_retrieve_next(gpointer user);
static void sim_own_numbers_update(struct ofono_sim *sim);

struct sim_file_op {
	int id;
	gboolean cache;
	enum ofono_sim_file_structure structure;
	int length;
	int record_length;
	int current;
	gconstpointer cb;
	gboolean is_read;
	void *buffer;
	void *userdata;
};

struct ofono_sim {
	char *imsi;
	unsigned char mnc_length;
	GSList *own_numbers;
	GSList *new_numbers;
	GSList *service_numbers;
	gboolean sdn_ready;
	gboolean ready;
	GQueue *simop_q;
	gint simop_source;
	unsigned char efmsisdn_length;
	unsigned char efmsisdn_records;
	unsigned int next_ready_watch_id;
	GSList *ready_watches;
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

struct sim_ready_watch {
	unsigned int id;
	ofono_sim_ready_notify_cb_t notify;
	void *data;
	ofono_destroy_func destroy;
};

struct service_number {
	char *id;
	struct ofono_phone_number ph;
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

static void sim_file_op_free(struct sim_file_op *node)
{
	g_free(node);
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

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	if (sim->imsi)
		ofono_dbus_dict_append(&dict, "SubscriberIdentity",
					DBUS_TYPE_STRING, &sim->imsi);

	if (sim->mnc_length)
		ofono_dbus_dict_append(&dict, "MobileNetworkCodeLength",
					DBUS_TYPE_BYTE, &sim->mnc_length);

	own_numbers = get_own_numbers(sim->own_numbers);

	ofono_dbus_dict_append_array(&dict, "SubscriberNumbers",
					DBUS_TYPE_STRING, &own_numbers);
	g_strfreev(own_numbers);

	if (sim->service_numbers && sim->sdn_ready) {
		service_numbers = get_service_numbers(sim->service_numbers);

		ofono_dbus_dict_append_dict(&dict, "ServiceDiallingNumbers",
						DBUS_TYPE_STRING,
						&service_numbers);
		g_strfreev(service_numbers);
	}

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
		} else
			memset(efmsisdn, 0xff, sim->efmsisdn_length);

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

static GDBusMethodTable sim_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	sim_get_properties	},
	{ "SetProperty",	"sv",	"",		sim_set_property,
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

static void sim_msisdn_read_cb(int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	int total;
	struct ofono_phone_number ph;

	if (!ok)
		goto check;

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

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
							SIM_MANAGER_INTERFACE,
							"SubscriberNumbers",
							DBUS_TYPE_STRING,
							&own_numbers);
		g_strfreev(own_numbers);
	} else {
		g_slist_foreach(sim->new_numbers, (GFunc) g_free, NULL);
		g_slist_free(sim->new_numbers);
	}

	sim->new_numbers = NULL;
}

static void sim_ad_read_cb(int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_sim *sim = userdata;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sim->atom);
	int new_mnc_length;

	if (!ok)
		return;

	if (structure != OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		return;

	if (length < 4)
		return;

	new_mnc_length = data[3] & 0xf;

	if (sim->mnc_length == new_mnc_length)
		return;

	sim->mnc_length = new_mnc_length;

	ofono_dbus_signal_property_changed(conn, path,
					SIM_MANAGER_INTERFACE,
					"MobileNetworkCodeLength",
					DBUS_TYPE_BYTE, &sim->mnc_length);
}

static gint service_number_compare(gconstpointer a, gconstpointer b)
{
	const struct service_number *sdn = a;
	const char *id = b;

	return strcmp(sdn->id, id);
}

static void sim_sdn_read_cb(int ok,
				enum ofono_sim_file_structure structure,
				int length, int record,
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

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		return;

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
						SIM_MANAGER_INTERFACE,
						"ServiceDiallingNumbers",
						DBUS_TYPE_STRING,
						&service_numbers);
		g_strfreev(service_numbers);
	}
}

static void sim_own_numbers_update(struct ofono_sim *sim)
{
	ofono_sim_read(sim, SIM_EFMSISDN_FILEID,
			sim_msisdn_read_cb, sim);
}

static void sim_ready(void *user)
{
	struct ofono_sim *sim = user;

	sim_own_numbers_update(sim);

	ofono_sim_read(sim, SIM_EFAD_FILEID, sim_ad_read_cb, sim);
	ofono_sim_read(sim, SIM_EFSDN_FILEID, sim_sdn_read_cb, sim);
}

static void sim_imsi_cb(const struct ofono_error *error, const char *imsi,
		void *data)
{
	struct ofono_sim *sim = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Unable to read IMSI, emergency calls only");
		return;
	}

	sim->imsi = g_strdup(imsi);

	ofono_sim_set_ready(sim);
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

static void sim_op_error(struct ofono_sim *sim)
{
	struct sim_file_op *op = g_queue_pop_head(sim->simop_q);

	if (g_queue_get_length(sim->simop_q) > 0)
		sim->simop_source = g_timeout_add(0, sim_op_next, sim);

	if (op->is_read == TRUE)
		((ofono_sim_file_read_cb_t) op->cb)
			(0, 0, 0, 0, 0, 0, op->userdata);
	else
		((ofono_sim_file_write_cb_t) op->cb)
			(0, op->userdata);

	sim_file_op_free(op);
}

static gboolean cache_record(const char *path, int current, int record_len,
				const unsigned char *data)
{
	int r = 0;
	int fd;

	fd = TFR(open(path, O_WRONLY));

	if (fd == -1)
		return FALSE;

	if (lseek(fd, (current - 1) * record_len +
				SIM_CACHE_HEADER_SIZE, SEEK_SET) !=
			(off_t) -1)
		r = TFR(write(fd, data, record_len));
	TFR(close(fd));

	if (r < record_len) {
		unlink(path);
		return FALSE;
	}

	return TRUE;
}

static void sim_op_retrieve_cb(const struct ofono_error *error,
				const unsigned char *data, int len, void *user)
{
	struct ofono_sim *sim = user;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);
	int total = op->length / op->record_length;
	ofono_sim_file_read_cb_t cb = op->cb;
	char *imsi = sim->imsi;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		sim_op_error(sim);
		return;
	}

	cb(1, op->structure, op->length, op->current,
		data, op->record_length, op->userdata);

	if (op->cache && imsi) {
		char *path = g_strdup_printf(SIM_CACHE_PATH, imsi, op->id);

		op->cache = cache_record(path, op->current, op->record_length,
						data);
		g_free(path);
	}

	if (op->current == total) {
		op = g_queue_pop_head(sim->simop_q);

		sim_file_op_free(op);

		if (g_queue_get_length(sim->simop_q) > 0)
			sim->simop_source = g_timeout_add(0, sim_op_next, sim);
	} else {
		op->current += 1;
		sim->simop_source = g_timeout_add(0, sim_op_retrieve_next, sim);
	}
}

static gboolean sim_op_retrieve_next(gpointer user)
{
	struct ofono_sim *sim = user;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);

	sim->simop_source = 0;

	switch (op->structure) {
	case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:
		if (!sim->driver->read_file_transparent) {
			sim_op_error(sim);
			return FALSE;
		}

		sim->driver->read_file_transparent(sim, op->id, 0, op->length,
						sim_op_retrieve_cb, sim);
		break;
	case OFONO_SIM_FILE_STRUCTURE_FIXED:
		if (!sim->driver->read_file_linear) {
			sim_op_error(sim);
			return FALSE;
		}

		sim->driver->read_file_linear(sim, op->id, op->current,
						op->record_length,
						sim_op_retrieve_cb, sim);
		break;
	case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
		if (!sim->driver->read_file_cyclic) {
			sim_op_error(sim);
			return FALSE;
		}

		sim->driver->read_file_cyclic(sim, op->id, op->current,
						op->record_length,
						sim_op_retrieve_cb, sim);
		break;
	default:
		ofono_error("Unrecognized file structure, this can't happen");
	}

	return FALSE;
}

static gboolean cache_info(const char *path, const unsigned char *info, int len)
{
	int fd;
	int r;

	if (create_dirs(path, SIM_CACHE_MODE | S_IXUSR) != 0)
		return FALSE;

	fd = TFR(open(path, O_WRONLY | O_CREAT, SIM_CACHE_MODE));

	if (fd == -1) {
		ofono_debug("Error %i creating cache file %s",
				errno, path);
		return FALSE;
	}

	r = TFR(write(fd, info, len));
	TFR(close(fd));

	if (r < len) {
		unlink(path);
		return FALSE;
	}

	return TRUE;
}

static void sim_op_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int record_length,
				const unsigned char access[3], void *data)
{
	struct ofono_sim *sim = data;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);
	char *imsi = sim->imsi;
	enum sim_file_access update;
	enum sim_file_access invalidate;
	enum sim_file_access rehabilitate;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		sim_op_error(sim);
		return;
	}

	/* TS 11.11, Section 9.3 */
	update = file_access_condition_decode(access[0] & 0xf);
	rehabilitate = file_access_condition_decode((access[2] >> 4) & 0xf);
	invalidate = file_access_condition_decode(access[2] & 0xf);

	op->structure = structure;
	op->length = length;
	/* Never cache card holder writable files */
	op->cache = (update == SIM_FILE_ACCESS_ADM ||
			update == SIM_FILE_ACCESS_NEVER) &&
			(invalidate == SIM_FILE_ACCESS_ADM ||
				invalidate == SIM_FILE_ACCESS_NEVER) &&
			(rehabilitate == SIM_FILE_ACCESS_ADM ||
				rehabilitate == SIM_FILE_ACCESS_NEVER);

	if (structure == OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		op->record_length = length;
	else
		op->record_length = record_length;

	op->current = 1;

	sim->simop_source = g_timeout_add(0, sim_op_retrieve_next, sim);

	if (op->cache && imsi) {
		char *path = g_strdup_printf(SIM_CACHE_PATH, imsi, op->id);
		unsigned char fileinfo[6];

		fileinfo[0] = error->type;
		fileinfo[1] = length >> 8;
		fileinfo[2] = length & 0xff;
		fileinfo[3] = structure;
		fileinfo[4] = record_length >> 8;
		fileinfo[5] = record_length & 0xff;

		op->cache = cache_info(path, fileinfo, 6);

		g_free(path);
	}
}

static void sim_op_write_cb(const struct ofono_error *error, void *data)
{
	struct ofono_sim *sim = data;
	struct sim_file_op *op = g_queue_pop_head(sim->simop_q);
	ofono_sim_file_write_cb_t cb = op->cb;

	if (g_queue_get_length(sim->simop_q) > 0)
		sim->simop_source = g_timeout_add(0, sim_op_next, sim);

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		cb(1, op->userdata);
	else
		cb(0, op->userdata);

	sim_file_op_free(op);
}

static gboolean sim_op_check_cached(struct ofono_sim *sim)
{
	char *imsi = sim->imsi;
	struct sim_file_op *op = g_queue_peek_head(sim->simop_q);
	ofono_sim_file_read_cb_t cb = op->cb;
	char *path;
	int fd;
	unsigned char fileinfo[SIM_CACHE_HEADER_SIZE];
	ssize_t len;
	int error_type;
	unsigned int file_length;
	enum ofono_sim_file_structure structure;
	unsigned int record_length;
	unsigned int record;
	guint8 *buffer = NULL;
	gboolean ret = FALSE;

	if (!imsi)
		return FALSE;

	path = g_strdup_printf(SIM_CACHE_PATH, imsi, op->id);

	fd = TFR(open(path, O_RDONLY));
	g_free(path);

	if (fd == -1) {
		if (errno != ENOENT)
			ofono_debug("Error %i opening cache file for "
					"fileid %04x, IMSI %s",
					errno, op->id, imsi);

		return FALSE;
	}

	len = TFR(read(fd, fileinfo, SIM_CACHE_HEADER_SIZE));

	if (len != SIM_CACHE_HEADER_SIZE)
		goto cleanup;

	error_type = fileinfo[0];
	file_length = (fileinfo[1] << 8) | fileinfo[2];
	structure = fileinfo[3];
	record_length = (fileinfo[4] << 8) | fileinfo[5];

	if (structure == OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		record_length = file_length;

	if (record_length == 0 || file_length < record_length)
		goto cleanup;

	if (error_type != OFONO_ERROR_TYPE_NO_ERROR) {
		ret = TRUE;
		cb(0, 0, 0, 0, 0, 0, 0);
		goto cleanup;
	}

	buffer = g_malloc(file_length);

	len = TFR(read(fd, buffer, file_length));

	if (len < (ssize_t)file_length)
		goto cleanup;

	for (record = 0; record < file_length / record_length; record++) {
		cb(1, structure, file_length, record + 1,
			&buffer[record * record_length], record_length,
			op->userdata);
	}

	ret = TRUE;

cleanup:
	if (buffer)
		g_free(buffer);

	TFR(close(fd));

	return ret;
}

static gboolean sim_op_next(gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_file_op *op;

	sim->simop_source = 0;

	if (!sim->simop_q)
		return FALSE;

	op = g_queue_peek_head(sim->simop_q);

	if (op->is_read == TRUE) {
		if (sim_op_check_cached(sim)) {
			op = g_queue_pop_head(sim->simop_q);

			sim_file_op_free(op);

			if (g_queue_get_length(sim->simop_q) > 0)
				sim->simop_source =
					g_timeout_add(0, sim_op_next, sim);

			return FALSE;
		}

		sim->driver->read_file_info(sim, op->id, sim_op_info_cb, sim);
	} else {
		switch (op->structure) {
		case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:
			sim->driver->write_file_transparent(sim, op->id, 0,
					op->length, op->buffer,
					sim_op_write_cb, sim);
			break;
		case OFONO_SIM_FILE_STRUCTURE_FIXED:
			sim->driver->write_file_linear(sim, op->id, op->current,
					op->length, op->buffer,
					sim_op_write_cb, sim);
			break;
		case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
			sim->driver->write_file_cyclic(sim, op->id,
					op->length, op->buffer,
					sim_op_write_cb, sim);
			break;
		default:
			ofono_error("Unrecognized file structure, "
					"this can't happen");
		}

		g_free(op->buffer);
	}

	return FALSE;
}

int ofono_sim_read(struct ofono_sim *sim, int id,
			ofono_sim_file_read_cb_t cb, void *data)
{
	struct sim_file_op *op;

	if (!cb)
		return -1;

	if (sim == NULL)
		return -1;

	if (!sim->driver)
		return -1;

	if (!sim->driver->read_file_info)
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
	op->is_read = TRUE;

	g_queue_push_tail(sim->simop_q, op);

	if (g_queue_get_length(sim->simop_q) == 1)
		sim->simop_source = g_timeout_add(0, sim_op_next, sim);

	return 0;
}

int ofono_sim_write(struct ofono_sim *sim, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata)
{
	struct sim_file_op *op;
	gconstpointer fn = NULL;

	if (!cb)
		return -1;

	if (sim == NULL)
		return -1;

	if (!sim->driver)
		return -1;

	switch (structure) {
	case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:
		fn = sim->driver->write_file_transparent;
		break;
	case OFONO_SIM_FILE_STRUCTURE_FIXED:
		fn = sim->driver->write_file_linear;
		break;
	case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
		fn = sim->driver->write_file_cyclic;
		break;
	default:
		ofono_error("Unrecognized file structure, this can't happen");
	}

	if (fn == NULL)
		return -1;

	if (!sim->simop_q)
		sim->simop_q = g_queue_new();

	op = g_new0(struct sim_file_op, 1);

	op->id = id;
	op->cb = cb;
	op->userdata = userdata;
	op->is_read = FALSE;
	op->buffer = g_memdup(data, length);
	op->structure = structure;
	op->length = length;
	op->current = record;

	g_queue_push_tail(sim->simop_q, op);

	if (g_queue_get_length(sim->simop_q) == 1)
		sim->simop_source = g_timeout_add(0, sim_op_next, sim);

	return 0;
}

const char *ofono_sim_get_imsi(struct ofono_sim *sim)
{
	if (sim == NULL)
		return NULL;

	return sim->imsi;
}

static void remove_all_watches(struct ofono_sim *sim)
{
	struct sim_ready_watch *watch;
	GSList *c;

	for (c = sim->ready_watches; c; c = c->next) {
		watch = c->data;

		if (watch->destroy)
			watch->destroy(watch->data);

		g_free(watch);
	}

	g_slist_free(sim->ready_watches);
	sim->ready_watches = NULL;
}

unsigned int ofono_sim_add_ready_watch(struct ofono_sim *sim,
				ofono_sim_ready_notify_cb_t notify,
				void *data, ofono_destroy_func destroy)
{
	struct sim_ready_watch *watch;

	DBG("%p", sim);

	if (sim == NULL)
		return 0;

	if (notify == NULL)
		return 0;

	watch = g_new0(struct sim_ready_watch, 1);

	watch->id = ++sim->next_ready_watch_id;
	watch->notify = notify;
	watch->destroy = destroy;
	watch->data = data;

	sim->ready_watches = g_slist_prepend(sim->ready_watches, watch);

	DBG("id: %u", watch->id);

	return watch->id;
}

void ofono_sim_remove_ready_watch(struct ofono_sim *sim, unsigned int id)
{
	struct sim_ready_watch *watch;
	GSList *p;
	GSList *c;

	if (sim == NULL)
		return;

	DBG("%p, %u", sim, id);

	p = NULL;
	c = sim->ready_watches;

	while (c) {
		watch = c->data;

		if (watch->id != id) {
			p = c;
			c = c->next;
			continue;
		}

		if (p)
			p->next = c->next;
		else
			sim->ready_watches = c->next;

		if (watch->destroy)
			watch->destroy(watch->data);

		g_free(watch);
		g_slist_free_1(c);

		return;
	}
}

int ofono_sim_get_ready(struct ofono_sim *sim)
{
	if (sim == NULL)
		return 0;

	if (sim->ready == TRUE)
		return 1;

	return 0;
}

void ofono_sim_set_ready(struct ofono_sim *sim)
{
	GSList *l;

	if (sim == NULL)
		return;

	if (sim->ready == TRUE)
		return;

	sim->ready = TRUE;

	for (l = sim->ready_watches; l; l = l->next) {
		struct sim_ready_watch *watch = l->data;

		watch->notify(watch->data);
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

	remove_all_watches(__ofono_atom_get_data(atom));

	g_dbus_unregister_interface(conn, path,
					SIM_MANAGER_INTERFACE);
	ofono_modem_remove_interface(modem, SIM_MANAGER_INTERFACE);
}

static void sim_remove(struct ofono_atom *atom)
{
	struct ofono_sim *sim = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (sim == NULL)
		return;

	if (sim->driver && sim->driver->remove)
		sim->driver->remove(sim);

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

	if (sim->simop_source) {
		g_source_remove(sim->simop_source);
		sim->simop_source = 0;
	}

	if (sim->simop_q) {
		g_queue_foreach(sim->simop_q, (GFunc)sim_file_op_free, NULL);
		g_queue_free(sim->simop_q);
		sim->simop_q = NULL;
	}

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
					SIM_MANAGER_INTERFACE,
					sim_methods, sim_signals, NULL,
					sim, NULL)) {
		ofono_error("Could not create %s interface",
				SIM_MANAGER_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, SIM_MANAGER_INTERFACE);

	__ofono_atom_register(sim->atom, sim_unregister);

	ofono_sim_add_ready_watch(sim, sim_ready, sim, NULL);

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
	sim_retrieve_imsi(sim);
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
