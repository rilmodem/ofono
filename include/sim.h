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

#ifndef __OFONO_SIM_H
#define __OFONO_SIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_sim;

/* 51.011 Section 9.3 */
enum ofono_sim_file_structure {
	OFONO_SIM_FILE_STRUCTURE_TRANSPARENT = 0,
	OFONO_SIM_FILE_STRUCTURE_FIXED = 1,
	OFONO_SIM_FILE_STRUCTURE_CYCLIC = 3
};

typedef void (*ofono_sim_file_info_cb_t)(const struct ofono_error *error,
					int filelength,
					enum ofono_sim_file_structure structure,
					int recordlength,
					const unsigned char access[3],
					void *data);

typedef void (*ofono_sim_read_cb_t)(const struct ofono_error *error,
					const unsigned char *sdata, int length,
					void *data);

typedef void (*ofono_sim_write_cb_t)(const struct ofono_error *error,
					void *data);

typedef void (*ofono_sim_imsi_cb_t)(const struct ofono_error *error,
					const char *imsi, void *data);

typedef void (*ofono_sim_ready_notify_cb_t)(void *data);

typedef void (*ofono_sim_file_read_cb_t)(int ok,
					enum ofono_sim_file_structure structure,
					int total_length, int record,
					const unsigned char *data,
					int record_length, void *userdata);

typedef void (*ofono_sim_file_write_cb_t)(int ok, void *userdata);

struct ofono_sim_driver {
	const char *name;
	int (*probe)(struct ofono_sim *sim, unsigned int vendor, void *data);
	void (*remove)(struct ofono_sim *sim);
	void (*read_file_info)(struct ofono_sim *sim, int fileid,
			ofono_sim_file_info_cb_t cb, void *data);
	void (*read_file_transparent)(struct ofono_sim *sim, int fileid,
			int start, int length,
			ofono_sim_read_cb_t cb, void *data);
	void (*read_file_linear)(struct ofono_sim *sim, int fileid,
			int record, int length,
			ofono_sim_read_cb_t cb, void *data);
	void (*read_file_cyclic)(struct ofono_sim *sim, int fileid,
			int record, int length,
			ofono_sim_read_cb_t cb, void *data);
	void (*write_file_transparent)(struct ofono_sim *sim, int fileid,
			int start, int length, const unsigned char *value,
			ofono_sim_write_cb_t cb, void *data);
	void (*write_file_linear)(struct ofono_sim *sim, int fileid,
			int record, int length, const unsigned char *value,
			ofono_sim_write_cb_t cb, void *data);
	void (*write_file_cyclic)(struct ofono_sim *sim, int fileid,
			int length, const unsigned char *value,
			ofono_sim_write_cb_t cb, void *data);
	void (*read_imsi)(struct ofono_sim *sim,
			ofono_sim_imsi_cb_t cb, void *data);
};

int ofono_sim_driver_register(const struct ofono_sim_driver *d);
void ofono_sim_driver_unregister(const struct ofono_sim_driver *d);

struct ofono_sim *ofono_sim_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_sim_register(struct ofono_sim *sim);
void ofono_sim_remove(struct ofono_sim *sim);

void ofono_sim_set_data(struct ofono_sim *sim, void *data);
void *ofono_sim_get_data(struct ofono_sim *sim);

const char *ofono_sim_get_imsi(struct ofono_sim *sim);

unsigned int ofono_sim_add_ready_watch(struct ofono_sim *sim,
				ofono_sim_ready_notify_cb_t cb,
				void *data, ofono_destroy_func destroy);

void ofono_sim_remove_ready_watch(struct ofono_sim *sim, unsigned int id);

int ofono_sim_get_ready(struct ofono_sim *sim);
void ofono_sim_set_ready(struct ofono_sim *sim);

/* This will queue an operation to read all available records with id from the
 * SIM.  Callback cb will be called every time a record has been read, or once
 * if an error has occurred.  For transparent files, the callback will only
 * be called once.
 *
 * Returns 0 if the request could be queued, -1 otherwise.
 */
int ofono_sim_read(struct ofono_sim *sim, int id,
			ofono_sim_file_read_cb_t cb, void *data);

int ofono_sim_write(struct ofono_sim *sim, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata);
#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SIM_H */
