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

struct sim_fs;

typedef void (*sim_fs_read_info_cb_t)(int ok, unsigned char file_status,
					int total_length, int record_length,
					void *userdata);

struct sim_fs *sim_fs_new(struct ofono_sim *sim,
				const struct ofono_sim_driver *driver);
struct ofono_sim_context *sim_fs_context_new(struct sim_fs *fs);

unsigned int sim_fs_file_watch_add(struct ofono_sim_context *context,
					int id, ofono_sim_file_changed_cb_t cb,
					void *userdata,
					ofono_destroy_func destroy);
void sim_fs_file_watch_remove(struct ofono_sim_context *context,
					unsigned int id);

/* Id of -1 notifies all watches, serving as a wildcard */
void sim_fs_notify_file_watches(struct sim_fs *fs, int id);

int sim_fs_read(struct ofono_sim_context *context, int id,
		enum ofono_sim_file_structure expected_type,
		unsigned short offset, unsigned short num_bytes,
		const unsigned char *path, unsigned int len,
		ofono_sim_file_read_cb_t cb, void *data);

int sim_fs_read_info(struct ofono_sim_context *context, int id,
		enum ofono_sim_file_structure expected_type,
		sim_fs_read_info_cb_t cb, void *data);

void sim_fs_check_version(struct sim_fs *fs);

int sim_fs_write(struct ofono_sim_context *context, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata);

char *sim_fs_get_cached_image(struct sim_fs *fs, int id);

void sim_fs_cache_image(struct sim_fs *fs, const char *image, int id);

void sim_fs_cache_flush(struct sim_fs *fs);
void sim_fs_cache_flush_file(struct sim_fs *fs, int id);
void sim_fs_image_cache_flush(struct sim_fs *fs);
void sim_fs_image_cache_flush_file(struct sim_fs *fs, int id);

void sim_fs_free(struct sim_fs *fs);
void sim_fs_context_free(struct ofono_sim_context *context);
