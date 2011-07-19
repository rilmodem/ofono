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

#ifndef __OFONO_SIM_H
#define __OFONO_SIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_sim;
struct ofono_sim_context;

/* 51.011 Section 9.3 */
enum ofono_sim_file_structure {
	OFONO_SIM_FILE_STRUCTURE_TRANSPARENT = 0,
	OFONO_SIM_FILE_STRUCTURE_FIXED = 1,
	OFONO_SIM_FILE_STRUCTURE_CYCLIC = 3
};

enum ofono_sim_password_type {
	OFONO_SIM_PASSWORD_NONE = 0,
	OFONO_SIM_PASSWORD_SIM_PIN,
	OFONO_SIM_PASSWORD_PHSIM_PIN,
	OFONO_SIM_PASSWORD_PHFSIM_PIN,
	OFONO_SIM_PASSWORD_SIM_PIN2,
	OFONO_SIM_PASSWORD_PHNET_PIN,
	OFONO_SIM_PASSWORD_PHNETSUB_PIN,
	OFONO_SIM_PASSWORD_PHSP_PIN,
	OFONO_SIM_PASSWORD_PHCORP_PIN,
	OFONO_SIM_PASSWORD_SIM_PUK,
	OFONO_SIM_PASSWORD_PHFSIM_PUK,
	OFONO_SIM_PASSWORD_SIM_PUK2,
	OFONO_SIM_PASSWORD_PHNET_PUK,
	OFONO_SIM_PASSWORD_PHNETSUB_PUK,
	OFONO_SIM_PASSWORD_PHSP_PUK,
	OFONO_SIM_PASSWORD_PHCORP_PUK,
	OFONO_SIM_PASSWORD_INVALID,
};

enum ofono_sim_phase {
	OFONO_SIM_PHASE_1G,
	OFONO_SIM_PHASE_2G,
	OFONO_SIM_PHASE_2G_PLUS,
	OFONO_SIM_PHASE_3G,
	OFONO_SIM_PHASE_UNKNOWN,
};

enum ofono_sim_cphs_phase {
	OFONO_SIM_CPHS_PHASE_NONE,
	OFONO_SIM_CPHS_PHASE_1G,
	OFONO_SIM_CPHS_PHASE_2G,
};

enum ofono_sim_state {
	OFONO_SIM_STATE_NOT_PRESENT,
	OFONO_SIM_STATE_INSERTED,
	OFONO_SIM_STATE_LOCKED_OUT,
	OFONO_SIM_STATE_READY,
};

typedef void (*ofono_sim_file_info_cb_t)(const struct ofono_error *error,
					int filelength,
					enum ofono_sim_file_structure structure,
					int recordlength,
					const unsigned char access[3],
					unsigned char file_status,
					void *data);

typedef void (*ofono_sim_read_cb_t)(const struct ofono_error *error,
					const unsigned char *sdata, int length,
					void *data);

typedef void (*ofono_sim_write_cb_t)(const struct ofono_error *error,
					void *data);

typedef void (*ofono_sim_imsi_cb_t)(const struct ofono_error *error,
					const char *imsi, void *data);

typedef void (*ofono_sim_state_event_cb_t)(enum ofono_sim_state new_state,
					void *data);

typedef void (*ofono_sim_file_read_cb_t)(int ok, int total_length, int record,
					const unsigned char *data,
					int record_length, void *userdata);
typedef void (*ofono_sim_file_changed_cb_t)(int id, void *userdata);

typedef void (*ofono_sim_file_write_cb_t)(int ok, void *userdata);

typedef void (*ofono_sim_passwd_cb_t)(const struct ofono_error *error,
					enum ofono_sim_password_type type,
					void *data);

typedef void (*ofono_sim_pin_retries_cb_t)(const struct ofono_error *error,
			int retries[OFONO_SIM_PASSWORD_INVALID], void *data);

typedef void (*ofono_sim_lock_unlock_cb_t)(const struct ofono_error *error,
					void *data);

typedef void (*ofono_sim_locked_cb_t)(const struct ofono_error *error,
					int locked, void *data);

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
	void (*query_passwd_state)(struct ofono_sim *sim,
			ofono_sim_passwd_cb_t cb, void *data);
	void (*send_passwd)(struct ofono_sim *sim, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data);
	void (*query_pin_retries)(struct ofono_sim *sim,
				ofono_sim_pin_retries_cb_t cb, void *data);
	void (*reset_passwd)(struct ofono_sim *sim, const char *puk,
			const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data);
	void (*change_passwd)(struct ofono_sim *sim,
			enum ofono_sim_password_type type,
			const char *old_passwd, const char *new_passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data);
	void (*lock)(struct ofono_sim *sim, enum ofono_sim_password_type type,
			int enable, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data);
	void (*query_locked)(struct ofono_sim *sim,
			enum ofono_sim_password_type type,
			ofono_sim_locked_cb_t cb, void *data);
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
const char *ofono_sim_get_mcc(struct ofono_sim *sim);
const char *ofono_sim_get_mnc(struct ofono_sim *sim);
enum ofono_sim_phase ofono_sim_get_phase(struct ofono_sim *sim);

enum ofono_sim_cphs_phase ofono_sim_get_cphs_phase(struct ofono_sim *sim);
const unsigned char *ofono_sim_get_cphs_service_table(struct ofono_sim *sim);

unsigned int ofono_sim_add_state_watch(struct ofono_sim *sim,
					ofono_sim_state_event_cb_t cb,
					void *data, ofono_destroy_func destroy);

void ofono_sim_remove_state_watch(struct ofono_sim *sim, unsigned int id);

enum ofono_sim_state ofono_sim_get_state(struct ofono_sim *sim);

void ofono_sim_inserted_notify(struct ofono_sim *sim, ofono_bool_t inserted);

struct ofono_sim_context *ofono_sim_context_create(struct ofono_sim *sim);
void ofono_sim_context_free(struct ofono_sim_context *context);

/* This will queue an operation to read all available records with id from the
 * SIM.  Callback cb will be called every time a record has been read, or once
 * if an error has occurred.  For transparent files, the callback will only
 * be called once.
 *
 * Returns 0 if the request could be queued, -1 otherwise.
 */
int ofono_sim_read(struct ofono_sim_context *context, int id,
			enum ofono_sim_file_structure expected,
			ofono_sim_file_read_cb_t cb, void *data);

int ofono_sim_write(struct ofono_sim_context *context, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata);

int ofono_sim_read_bytes(struct ofono_sim_context *context, int id,
			unsigned short offset, unsigned short num_bytes,
			ofono_sim_file_read_cb_t cb, void *data);

unsigned int ofono_sim_add_file_watch(struct ofono_sim_context *context,
					int id, ofono_sim_file_changed_cb_t cb,
					void *userdata,
					ofono_destroy_func destroy);
void ofono_sim_remove_file_watch(struct ofono_sim_context *context,
					unsigned int id);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SIM_H */
