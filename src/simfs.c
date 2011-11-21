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

#include <glib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>

#include "ofono.h"

#include "simfs.h"
#include "simutil.h"
#include "storage.h"

#define SIM_CACHE_MODE 0600
#define SIM_CACHE_BASEPATH STORAGEDIR "/%s-%i"
#define SIM_CACHE_VERSION SIM_CACHE_BASEPATH "/version"
#define SIM_CACHE_PATH SIM_CACHE_BASEPATH "/%04x"
#define SIM_CACHE_HEADER_SIZE 39
#define SIM_FILE_INFO_SIZE 7
#define SIM_IMAGE_CACHE_BASEPATH STORAGEDIR "/%s-%i/images"
#define SIM_IMAGE_CACHE_PATH SIM_IMAGE_CACHE_BASEPATH "/%d.xpm"

#define SIM_FS_VERSION 2

static gboolean sim_fs_op_next(gpointer user_data);
static gboolean sim_fs_op_read_record(gpointer user);
static gboolean sim_fs_op_read_block(gpointer user_data);

struct sim_fs_op {
	int id;
	unsigned char *buffer;
	enum ofono_sim_file_structure structure;
	unsigned short offset;
	gboolean info_only;
	int num_bytes;
	int length;
	int record_length;
	int current;
	gconstpointer cb;
	gboolean is_read;
	void *userdata;
	struct ofono_sim_context *context;
};

static void sim_fs_op_free(struct sim_fs_op *node)
{
	g_free(node->buffer);
	g_free(node);
}

struct sim_fs {
	GQueue *op_q;
	gint op_source;
	unsigned char bitmap[32];
	int fd;
	struct ofono_sim *sim;
	const struct ofono_sim_driver *driver;
	GSList *contexts;
};

void sim_fs_free(struct sim_fs *fs)
{
	if (fs == NULL)
		return;

	if (fs->op_source) {
		g_source_remove(fs->op_source);
		fs->op_source = 0;
	}

	/*
	 * Note: users of sim_fs must not assume that the callback happens
	 * for operations still in progress
	 */
	if (fs->op_q) {
		g_queue_foreach(fs->op_q, (GFunc) sim_fs_op_free, NULL);
		g_queue_free(fs->op_q);
		fs->op_q = NULL;
	}

	while (fs->contexts)
		sim_fs_context_free(fs->contexts->data);

	g_free(fs);
}

struct file_watch {
	struct ofono_watchlist_item item;
	int ef;
};

struct ofono_sim_context {
	struct sim_fs *fs;
	struct ofono_watchlist *file_watches;
};

struct sim_fs *sim_fs_new(struct ofono_sim *sim,
				const struct ofono_sim_driver *driver)
{
	struct sim_fs *fs;

	fs = g_try_new0(struct sim_fs, 1);
	if (fs == NULL)
		return NULL;

	fs->sim = sim;
	fs->driver = driver;
	fs->fd = -1;

	return fs;
}

struct ofono_sim_context *sim_fs_context_new(struct sim_fs *fs)
{
	struct ofono_sim_context *context =
		g_try_new0(struct ofono_sim_context, 1);

	if (context == NULL)
		return NULL;

	context->fs = fs;
	fs->contexts = g_slist_prepend(fs->contexts, context);

	return context;
}

void sim_fs_context_free(struct ofono_sim_context *context)
{
	struct sim_fs *fs = context->fs;
	int n = 0;
	struct sim_fs_op *op;

	if (fs->op_q) {
		while ((op = g_queue_peek_nth(fs->op_q, n)) != NULL) {
			if (op->context != context) {
				n += 1;
				continue;
			}

			if (n == 0) {
				op->cb = NULL;

				n += 1;
				continue;
			}

			sim_fs_op_free(op);
			g_queue_remove(fs->op_q, op);
		}
	}

	if (context->file_watches)
		__ofono_watchlist_free(context->file_watches);

	fs->contexts = g_slist_remove(fs->contexts, context);
	g_free(context);
}

unsigned int sim_fs_file_watch_add(struct ofono_sim_context *context, int id,
					ofono_sim_file_changed_cb_t cb,
					void *userdata,
					ofono_destroy_func destroy)
{
	struct file_watch *watch;

	if (cb == NULL)
		return 0;

	if (context->file_watches == NULL)
		context->file_watches = __ofono_watchlist_new(g_free);

	watch = g_new0(struct file_watch, 1);

	watch->ef = id;
	watch->item.notify = cb;
	watch->item.notify_data = userdata;
	watch->item.destroy = destroy;

	return __ofono_watchlist_add_item(context->file_watches,
					(struct ofono_watchlist_item *) watch);
}

void sim_fs_file_watch_remove(struct ofono_sim_context *context,
				unsigned int id)
{
	__ofono_watchlist_remove_item(context->file_watches, id);
}

void sim_fs_notify_file_watches(struct sim_fs *fs, int id)
{
	GSList *l;

	for (l = fs->contexts; l; l = l->next) {
		struct ofono_sim_context *context = l->data;
		GSList *k;

		for (k = context->file_watches->items; k; k = k->next) {
			struct file_watch *w = k->data;
			ofono_sim_file_changed_cb_t notify = w->item.notify;

			if (id == -1 || w->ef == id)
				notify(w->ef, w->item.notify_data);
		}
	}

}

static void sim_fs_end_current(struct sim_fs *fs)
{
	struct sim_fs_op *op = g_queue_pop_head(fs->op_q);

	if (g_queue_get_length(fs->op_q) > 0)
		fs->op_source = g_idle_add(sim_fs_op_next, fs);

	if (fs->fd != -1) {
		TFR(close(fs->fd));
		fs->fd = -1;
	}

	memset(fs->bitmap, 0, sizeof(fs->bitmap));

	sim_fs_op_free(op);
}

static void sim_fs_op_error(struct sim_fs *fs)
{
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);

	if (op->cb == NULL) {
		sim_fs_end_current(fs);
		return;
	}

	if (op->info_only == TRUE)
		((sim_fs_read_info_cb_t) op->cb)
			(0, 0, 0, 0, op->userdata);
	else if (op->is_read == TRUE)
		((ofono_sim_file_read_cb_t) op->cb)
			(0, 0, 0, 0, 0, op->userdata);
	else
		((ofono_sim_file_write_cb_t) op->cb)
			(0, op->userdata);

	sim_fs_end_current(fs);
}

static gboolean cache_block(struct sim_fs *fs, int block, int block_len,
				const unsigned char *data, int num_bytes)
{
	int offset;
	int bit;
	ssize_t r;
	unsigned char b;

	if (fs->fd == -1)
		return FALSE;

	if (lseek(fs->fd, block * block_len +
				SIM_CACHE_HEADER_SIZE, SEEK_SET) == (off_t) -1)
		return FALSE;

	r = TFR(write(fs->fd, data, num_bytes));

	if (r != num_bytes)
		return FALSE;

	/* update present bit for this block */
	offset = block / 8;
	bit = block % 8;

	/* lseek to correct byte (skip file info) */
	lseek(fs->fd, offset + SIM_FILE_INFO_SIZE, SEEK_SET);

	b = fs->bitmap[offset];
	b |= 1 << bit;

	r = TFR(write(fs->fd, &b, sizeof(b)));

	if (r != sizeof(b))
		return FALSE;

	fs->bitmap[offset] = b;

	return TRUE;
}

static void sim_fs_op_write_cb(const struct ofono_error *error, void *data)
{
	struct sim_fs *fs = data;
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);
	ofono_sim_file_write_cb_t cb = op->cb;

	if (cb == NULL) {
		sim_fs_end_current(fs);
		return;
	}

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		cb(1, op->userdata);
	else
		cb(0, op->userdata);

	sim_fs_end_current(fs);
}

static void sim_fs_op_read_block_cb(const struct ofono_error *error,
					const unsigned char *data, int len,
					void *user)
{
	struct sim_fs *fs = user;
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);
	int start_block;
	int end_block;
	int bufoff;
	int dataoff;
	int tocopy;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		sim_fs_op_error(fs);
		return;
	}

	start_block = op->offset / 256;
	end_block = (op->offset + (op->num_bytes - 1)) / 256;

	if (op->current == start_block) {
		bufoff = 0;
		dataoff = op->offset % 256;
		tocopy = MIN(256 - op->offset % 256,
				op->num_bytes - op->current * 256);
	} else {
		bufoff = (op->current - start_block - 1) * 256 +
				op->offset % 256;
		dataoff = 0;
		tocopy = MIN(256, op->num_bytes - op->current * 256);
	}

	DBG("bufoff: %d, dataoff: %d, tocopy: %d",
				bufoff, dataoff, tocopy);

	memcpy(op->buffer + bufoff, data + dataoff, tocopy);
	cache_block(fs, op->current, 256, data, len);

	if (op->cb == NULL) {
		sim_fs_end_current(fs);
		return;
	}

	op->current++;

	if (op->current > end_block) {
		ofono_sim_file_read_cb_t cb = op->cb;

		cb(1, op->num_bytes, 0, op->buffer,
				op->record_length, op->userdata);

		sim_fs_end_current(fs);
	} else {
		fs->op_source = g_idle_add(sim_fs_op_read_block, fs);
	}
}

static gboolean sim_fs_op_read_block(gpointer user_data)
{
	struct sim_fs *fs = user_data;
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);
	int start_block;
	int end_block;
	unsigned short read_bytes;

	fs->op_source = 0;

	if (op->cb == NULL) {
		sim_fs_end_current(fs);
		return FALSE;
	}

	start_block = op->offset / 256;
	end_block = (op->offset + (op->num_bytes - 1)) / 256;

	if (op->current == start_block) {
		op->buffer = g_try_new0(unsigned char, op->num_bytes);

		if (op->buffer == NULL) {
			sim_fs_op_error(fs);
			return FALSE;
		}
	}

	while (fs->fd != -1 && op->current <= end_block) {
		int offset = op->current / 8;
		int bit = 1 << op->current % 8;
		int bufoff;
		int seekoff;
		int toread;

		if ((fs->bitmap[offset] & bit) == 0)
			break;

		if (op->current == start_block) {
			bufoff = 0;
			seekoff = SIM_CACHE_HEADER_SIZE + op->current * 256 +
				op->offset % 256;
			toread = MIN(256 - op->offset % 256,
					op->num_bytes - op->current * 256);
		} else {
			bufoff = (op->current - start_block - 1) * 256 +
					op->offset % 256;
			seekoff = SIM_CACHE_HEADER_SIZE + op->current * 256;
			toread = MIN(256, op->num_bytes - op->current * 256);
		}

		DBG("bufoff: %d, seekoff: %d, toread: %d",
				bufoff, seekoff, toread);

		if (lseek(fs->fd, seekoff, SEEK_SET) == (off_t) -1)
			break;

		if (TFR(read(fs->fd, op->buffer + bufoff, toread)) != toread)
			break;

		op->current += 1;
	}

	if (op->current > end_block) {
		ofono_sim_file_read_cb_t cb = op->cb;

		cb(1, op->num_bytes, 0, op->buffer,
				op->record_length, op->userdata);

		sim_fs_end_current(fs);

		return FALSE;
	}

	if (fs->driver->read_file_transparent == NULL) {
		sim_fs_op_error(fs);
		return FALSE;
	}

	read_bytes = MIN(op->length - op->current * 256, 256);
	fs->driver->read_file_transparent(fs->sim, op->id,
						op->current * 256,
						read_bytes,
						sim_fs_op_read_block_cb, fs);

	return FALSE;
}

static void sim_fs_op_retrieve_cb(const struct ofono_error *error,
					const unsigned char *data, int len,
					void *user)
{
	struct sim_fs *fs = user;
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);
	int total = op->length / op->record_length;
	ofono_sim_file_read_cb_t cb = op->cb;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		sim_fs_op_error(fs);
		return;
	}

	cache_block(fs, op->current - 1, op->record_length,
			data, op->record_length);

	if (cb == NULL) {
		sim_fs_end_current(fs);
		return;
	}

	cb(1, op->length, op->current, data, op->record_length, op->userdata);

	if (op->current < total) {
		op->current += 1;
		fs->op_source = g_idle_add(sim_fs_op_read_record, fs);
	} else {
		sim_fs_end_current(fs);
	}
}

static gboolean sim_fs_op_read_record(gpointer user)
{
	struct sim_fs *fs = user;
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);
	const struct ofono_sim_driver *driver = fs->driver;
	int total = op->length / op->record_length;
	unsigned char buf[256];

	fs->op_source = 0;

	if (op->cb == NULL) {
		sim_fs_end_current(fs);
		return FALSE;
	}

	while (fs->fd != -1 && op->current <= total) {
		int offset = (op->current - 1) / 8;
		int bit = 1 << ((op->current - 1) % 8);
		ofono_sim_file_read_cb_t cb = op->cb;

		if ((fs->bitmap[offset] & bit) == 0)
			break;

		if (lseek(fs->fd, (op->current - 1) * op->record_length +
				SIM_CACHE_HEADER_SIZE, SEEK_SET) == (off_t) -1)
			break;

		if (TFR(read(fs->fd, buf, op->record_length)) !=
				op->record_length)
			break;

		cb(1, op->length, op->current,
				buf, op->record_length, op->userdata);

		op->current += 1;
	}

	if (op->current > total) {
		sim_fs_end_current(fs);

		return FALSE;
	}

	switch (op->structure) {
	case OFONO_SIM_FILE_STRUCTURE_FIXED:
		if (driver->read_file_linear == NULL) {
			sim_fs_op_error(fs);
			return FALSE;
		}

		driver->read_file_linear(fs->sim, op->id, op->current,
						op->record_length,
						sim_fs_op_retrieve_cb, fs);
		break;
	case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
		if (driver->read_file_cyclic == NULL) {
			sim_fs_op_error(fs);
			return FALSE;
		}

		driver->read_file_cyclic(fs->sim, op->id, op->current,
						op->record_length,
						sim_fs_op_retrieve_cb, fs);
		break;
	default:
		ofono_error("Unrecognized file structure, this can't happen");
	}

	return FALSE;
}

static void sim_fs_op_cache_fileinfo(struct sim_fs *fs,
					const struct ofono_error *error,
					int length,
					enum ofono_sim_file_structure structure,
					int record_length,
					const unsigned char access[3],
					unsigned char file_status)
{
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);
	const char *imsi = ofono_sim_get_imsi(fs->sim);
	enum ofono_sim_phase phase = ofono_sim_get_phase(fs->sim);
	enum sim_file_access update;
	enum sim_file_access invalidate;
	enum sim_file_access rehabilitate;
	unsigned char fileinfo[SIM_CACHE_HEADER_SIZE];
	gboolean cache;
	char *path;

	/* TS 11.11, Section 9.3 */
	update = file_access_condition_decode(access[0] & 0xf);
	rehabilitate = file_access_condition_decode((access[2] >> 4) & 0xf);
	invalidate = file_access_condition_decode(access[2] & 0xf);

	/* Never cache card holder writable files */
	cache = (update == SIM_FILE_ACCESS_ADM ||
			update == SIM_FILE_ACCESS_NEVER) &&
			(invalidate == SIM_FILE_ACCESS_ADM ||
				invalidate == SIM_FILE_ACCESS_NEVER) &&
			(rehabilitate == SIM_FILE_ACCESS_ADM ||
				rehabilitate == SIM_FILE_ACCESS_NEVER);

	if (imsi == NULL || phase == OFONO_SIM_PHASE_UNKNOWN || cache == FALSE)
		return;

	memset(fileinfo, 0, SIM_CACHE_HEADER_SIZE);

	fileinfo[0] = error->type;
	fileinfo[1] = length >> 8;
	fileinfo[2] = length & 0xff;
	fileinfo[3] = structure;
	fileinfo[4] = record_length >> 8;
	fileinfo[5] = record_length & 0xff;
	fileinfo[6] = file_status;

	path = g_strdup_printf(SIM_CACHE_PATH, imsi, phase, op->id);
	fs->fd = TFR(open(path, O_WRONLY | O_CREAT | O_TRUNC, SIM_CACHE_MODE));
	g_free(path);

	if (fs->fd == -1)
		return;

	if (TFR(write(fs->fd, fileinfo, SIM_CACHE_HEADER_SIZE)) ==
			SIM_CACHE_HEADER_SIZE)
		return;

	TFR(close(fs->fd));
	fs->fd = -1;
}

static void sim_fs_op_info_cb(const struct ofono_error *error, int length,
				enum ofono_sim_file_structure structure,
				int record_length,
				const unsigned char access[3],
				unsigned char file_status,
				void *data)
{
	struct sim_fs *fs = data;
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		sim_fs_op_error(fs);
		return;
	}

	sim_fs_op_cache_fileinfo(fs, error, length, structure, record_length,
					access, file_status);

	if (structure != op->structure) {
		ofono_error("Requested file structure differs from SIM: %x",
				op->id);
		sim_fs_op_error(fs);
		return;
	}

	if (op->cb == NULL) {
		sim_fs_end_current(fs);
		return;
	}

	op->structure = structure;
	op->length = length;

	if (structure == OFONO_SIM_FILE_STRUCTURE_TRANSPARENT) {
		if (op->num_bytes == 0)
			op->num_bytes = op->length;

		op->record_length = length;
		op->current = op->offset / 256;

		if (op->info_only == FALSE)
			fs->op_source = g_idle_add(sim_fs_op_read_block, fs);
	} else {
		op->record_length = record_length;
		op->current = 1;

		if (op->info_only == FALSE)
			fs->op_source = g_idle_add(sim_fs_op_read_record, fs);
	}

	if (op->info_only == TRUE) {
		/*
		 * It's an info-only request, so there is no need to request
		 * actual contents of the EF. Just return the EF-info.
		 */
		sim_fs_read_info_cb_t cb = op->cb;

		cb(1, file_status, op->length,
			op->record_length, op->userdata);

		sim_fs_end_current(fs);
	}
}

static gboolean sim_fs_op_check_cached(struct sim_fs *fs)
{
	const char *imsi = ofono_sim_get_imsi(fs->sim);
	enum ofono_sim_phase phase = ofono_sim_get_phase(fs->sim);
	struct sim_fs_op *op = g_queue_peek_head(fs->op_q);
	char *path;
	int fd;
	ssize_t len;
	unsigned char fileinfo[SIM_CACHE_HEADER_SIZE];
	int error_type;
	int file_length;
	enum ofono_sim_file_structure structure;
	int record_length;
	unsigned char file_status;

	if (imsi == NULL || phase == OFONO_SIM_PHASE_UNKNOWN)
		return FALSE;

	path = g_strdup_printf(SIM_CACHE_PATH, imsi, phase, op->id);

	if (path == NULL)
		return FALSE;

	fd = TFR(open(path, O_RDWR));
	g_free(path);

	if (fd == -1) {
		if (errno != ENOENT)
			DBG("Error %i opening cache file for "
					"fileid %04x, IMSI %s",
					errno, op->id, imsi);

		return FALSE;
	}

	len = TFR(read(fd, fileinfo, SIM_CACHE_HEADER_SIZE));

	if (len != SIM_CACHE_HEADER_SIZE)
		goto error;

	error_type = fileinfo[0];
	file_length = (fileinfo[1] << 8) | fileinfo[2];
	structure = fileinfo[3];
	record_length = (fileinfo[4] << 8) | fileinfo[5];
	file_status = fileinfo[6];

	if (structure == OFONO_SIM_FILE_STRUCTURE_TRANSPARENT)
		record_length = file_length;

	if (record_length == 0 || file_length < record_length)
		goto error;

	op->length = file_length;
	op->record_length = record_length;
	memcpy(fs->bitmap, fileinfo + SIM_FILE_INFO_SIZE,
			SIM_CACHE_HEADER_SIZE - SIM_FILE_INFO_SIZE);
	fs->fd = fd;

	if (error_type != OFONO_ERROR_TYPE_NO_ERROR ||
			structure != op->structure) {
		sim_fs_op_error(fs);
		return TRUE;
	}

	if (op->info_only == TRUE) {
		/*
		 * It's an info-only request, so there is no need to request
		 * actual contents of the EF. Just return the EF-info.
		 */
		sim_fs_read_info_cb_t cb = op->cb;

		cb(1, file_status, op->length,
			op->record_length, op->userdata);

		sim_fs_end_current(fs);
	} else if (structure == OFONO_SIM_FILE_STRUCTURE_TRANSPARENT) {
		if (op->num_bytes == 0)
			op->num_bytes = op->length;

		op->current = op->offset / 256;
		fs->op_source = g_idle_add(sim_fs_op_read_block, fs);
	} else {
		op->current = 1;
		fs->op_source = g_idle_add(sim_fs_op_read_record, fs);
	}

	return TRUE;

error:
	TFR(close(fd));
	return FALSE;
}

static gboolean sim_fs_op_next(gpointer user_data)
{
	struct sim_fs *fs = user_data;
	const struct ofono_sim_driver *driver = fs->driver;
	struct sim_fs_op *op;

	fs->op_source = 0;

	if (fs->op_q == NULL)
		return FALSE;

	op = g_queue_peek_head(fs->op_q);

	if (op->cb == NULL) {
		sim_fs_end_current(fs);
		return FALSE;
	}

	if (op->is_read == TRUE) {
		if (sim_fs_op_check_cached(fs))
			return FALSE;

		driver->read_file_info(fs->sim, op->id, sim_fs_op_info_cb, fs);
	} else {
		switch (op->structure) {
		case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:
			driver->write_file_transparent(fs->sim, op->id, 0,
					op->length, op->buffer,
					sim_fs_op_write_cb, fs);
			break;
		case OFONO_SIM_FILE_STRUCTURE_FIXED:
			driver->write_file_linear(fs->sim, op->id, op->current,
					op->length, op->buffer,
					sim_fs_op_write_cb, fs);
			break;
		case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
			driver->write_file_cyclic(fs->sim, op->id,
					op->length, op->buffer,
					sim_fs_op_write_cb, fs);
			break;
		default:
			ofono_error("Unrecognized file structure, "
					"this can't happen");
		}

		g_free(op->buffer);
		op->buffer = NULL;
	}

	return FALSE;
}

int sim_fs_read_info(struct ofono_sim_context *context, int id,
			enum ofono_sim_file_structure expected_type,
			sim_fs_read_info_cb_t cb, void *data)
{
	struct sim_fs *fs = context->fs;
	struct sim_fs_op *op;

	if (cb == NULL)
		return -EINVAL;

	if (fs->driver == NULL)
		return -EINVAL;

	if (fs->driver->read_file_info == NULL)
		return -ENOSYS;

	if (fs->op_q == NULL)
		fs->op_q = g_queue_new();

	op = g_try_new0(struct sim_fs_op, 1);
	if (op == NULL)
		return -ENOMEM;

	op->id = id;
	op->structure = expected_type;
	op->cb = cb;
	op->userdata = data;
	op->is_read = TRUE;
	op->info_only = TRUE;
	op->context = context;

	g_queue_push_tail(fs->op_q, op);

	if (g_queue_get_length(fs->op_q) == 1)
		fs->op_source = g_idle_add(sim_fs_op_next, fs);

	return 0;
}

int sim_fs_read(struct ofono_sim_context *context, int id,
		enum ofono_sim_file_structure expected_type,
		unsigned short offset, unsigned short num_bytes,
		ofono_sim_file_read_cb_t cb, void *data)
{
	struct sim_fs *fs = context->fs;
	struct sim_fs_op *op;

	if (cb == NULL)
		return -EINVAL;

	if (fs->driver == NULL)
		return -EINVAL;

	if (fs->driver->read_file_info == NULL) {
		cb(0, 0, 0, NULL, 0, data);
		return -ENOSYS;
	}

	if (fs->op_q == NULL)
		fs->op_q = g_queue_new();

	op = g_try_new0(struct sim_fs_op, 1);
	if (op == NULL)
		return -ENOMEM;

	op->id = id;
	op->structure = expected_type;
	op->cb = cb;
	op->userdata = data;
	op->is_read = TRUE;
	op->offset = offset;
	op->num_bytes = num_bytes;
	op->info_only = FALSE;
	op->context = context;

	g_queue_push_tail(fs->op_q, op);

	if (g_queue_get_length(fs->op_q) == 1)
		fs->op_source = g_idle_add(sim_fs_op_next, fs);

	return 0;
}

int sim_fs_write(struct ofono_sim_context *context, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata)
{
	struct sim_fs *fs = context->fs;
	struct sim_fs_op *op;
	gconstpointer fn = NULL;

	if (cb == NULL)
		return -EINVAL;

	if (fs->driver == NULL)
		return -EINVAL;

	switch (structure) {
	case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:
		fn = fs->driver->write_file_transparent;
		break;
	case OFONO_SIM_FILE_STRUCTURE_FIXED:
		fn = fs->driver->write_file_linear;
		break;
	case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
		fn = fs->driver->write_file_cyclic;
		break;
	default:
		ofono_error("Unrecognized file structure, this can't happen");
	}

	if (fn == NULL)
		return -ENOSYS;

	if (fs->op_q == NULL)
		fs->op_q = g_queue_new();

	op = g_try_new0(struct sim_fs_op, 1);
	if (op == NULL)
		return -ENOMEM;

	op->id = id;
	op->cb = cb;
	op->userdata = userdata;
	op->is_read = FALSE;
	op->buffer = g_memdup(data, length);
	op->structure = structure;
	op->length = length;
	op->current = record;
	op->context = context;

	g_queue_push_tail(fs->op_q, op);

	if (g_queue_get_length(fs->op_q) == 1)
		fs->op_source = g_idle_add(sim_fs_op_next, fs);

	return 0;
}

void sim_fs_cache_image(struct sim_fs *fs, const char *image, int id)
{
	const char *imsi;
	enum ofono_sim_phase phase;

	if (fs == NULL || image == NULL)
		return;

	imsi = ofono_sim_get_imsi(fs->sim);
	if (imsi == NULL)
		return;

	phase = ofono_sim_get_phase(fs->sim);
	if (phase == OFONO_SIM_PHASE_UNKNOWN)
		return;

	write_file((const unsigned char *) image, strlen(image),
			SIM_CACHE_MODE, SIM_IMAGE_CACHE_PATH, imsi,
			phase, id);
}

char *sim_fs_get_cached_image(struct sim_fs *fs, int id)
{
	const char *imsi;
	enum ofono_sim_phase phase;
	unsigned short image_length;
	int fd;
	char *buffer;
	char *path;
	int len;
	struct stat st_buf;

	if (fs == NULL)
		return NULL;

	imsi = ofono_sim_get_imsi(fs->sim);
	if (imsi == NULL)
		return NULL;

	phase = ofono_sim_get_phase(fs->sim);
	if (phase == OFONO_SIM_PHASE_UNKNOWN)
		return NULL;

	path = g_strdup_printf(SIM_IMAGE_CACHE_PATH, imsi, phase, id);

	TFR(stat(path, &st_buf));
	fd = TFR(open(path, O_RDONLY));
	g_free(path);

	if (fd < 0)
		return NULL;

	image_length = st_buf.st_size;
	buffer = g_try_new0(char, image_length + 1);

	if (buffer == NULL) {
		TFR(close(fd));
		return NULL;
	}

	len = TFR(read(fd, buffer, image_length));
	TFR(close(fd));

	if (len != image_length) {
		g_free(buffer);
		return NULL;
	}

	return buffer;
}

static void remove_cachefile(const char *imsi, enum ofono_sim_phase phase,
				const struct dirent *file)
{
	int id;
	char *path;

	if (file->d_type != DT_REG)
		return;

	if (sscanf(file->d_name, "%4x", &id) != 1)
		return;

	path = g_strdup_printf(SIM_CACHE_PATH, imsi, phase, id);
	remove(path);
	g_free(path);
}

static void remove_imagefile(const char *imsi, enum ofono_sim_phase phase,
				const struct dirent *file)
{
	int id;
	char *path;

	if (file->d_type != DT_REG)
		return;

	if (sscanf(file->d_name, "%d", &id) != 1)
		return;

	path = g_strdup_printf(SIM_IMAGE_CACHE_PATH, imsi, phase, id);
	remove(path);
	g_free(path);
}

void sim_fs_check_version(struct sim_fs *fs)
{
	const char *imsi = ofono_sim_get_imsi(fs->sim);
	enum ofono_sim_phase phase = ofono_sim_get_phase(fs->sim);
	unsigned char version;

	if (imsi == NULL || phase == OFONO_SIM_PHASE_UNKNOWN)
		return;

	if (read_file(&version, 1, SIM_CACHE_VERSION, imsi, phase) == 1)
		if (version == SIM_FS_VERSION)
			return;

	sim_fs_cache_flush(fs);

	version = SIM_FS_VERSION;
	write_file(&version, 1, SIM_CACHE_MODE, SIM_CACHE_VERSION, imsi, phase);
}

void sim_fs_cache_flush(struct sim_fs *fs)
{
	const char *imsi = ofono_sim_get_imsi(fs->sim);
	enum ofono_sim_phase phase = ofono_sim_get_phase(fs->sim);
	char *path = g_strdup_printf(SIM_CACHE_BASEPATH, imsi, phase);
	struct dirent **entries;
	int len = scandir(path, &entries, NULL, alphasort);

	g_free(path);

	if (len > 0) {
		/* Remove all file ids */
		while (len--) {
			remove_cachefile(imsi, phase, entries[len]);
			g_free(entries[len]);
		}

		g_free(entries);
	}

	sim_fs_image_cache_flush(fs);
}

void sim_fs_cache_flush_file(struct sim_fs *fs, int id)
{
	const char *imsi = ofono_sim_get_imsi(fs->sim);
	enum ofono_sim_phase phase = ofono_sim_get_phase(fs->sim);
	char *path = g_strdup_printf(SIM_CACHE_PATH, imsi, phase, id);

	remove(path);
	g_free(path);
}

void sim_fs_image_cache_flush(struct sim_fs *fs)
{
	const char *imsi = ofono_sim_get_imsi(fs->sim);
	enum ofono_sim_phase phase = ofono_sim_get_phase(fs->sim);
	char *path = g_strdup_printf(SIM_IMAGE_CACHE_BASEPATH, imsi, phase);
	struct dirent **entries;
	int len = scandir(path, &entries, NULL, alphasort);

	g_free(path);

	if (len <= 0)
		return;

	/* Remove everything */
	while (len--) {
		remove_imagefile(imsi, phase, entries[len]);
		g_free(entries[len]);
	}

	g_free(entries);
}

void sim_fs_image_cache_flush_file(struct sim_fs *fs, int id)
{
	const char *imsi = ofono_sim_get_imsi(fs->sim);
	enum ofono_sim_phase phase = ofono_sim_get_phase(fs->sim);
	char *path = g_strdup_printf(SIM_IMAGE_CACHE_PATH, imsi, phase, id);

	remove(path);
	g_free(path);
}
