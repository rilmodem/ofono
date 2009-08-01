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

typedef void (*ofono_sim_ready_notify_cb_t)(struct ofono_modem *modem);

typedef void (*ofono_sim_file_read_cb_t)(struct ofono_modem *modem, int ok,
					enum ofono_sim_file_structure structure,
					int total_length, int record,
					const unsigned char *data,
					int record_length, void *userdata);
typedef void (*ofono_sim_file_write_cb_t)(struct ofono_modem *modem, int ok,
					void *userdata);

void ofono_sim_manager_init(struct ofono_modem *modem);
void ofono_sim_manager_exit(struct ofono_modem *modem);

const char *ofono_sim_get_imsi(struct ofono_modem *modem);

int ofono_sim_ready_notify_register(struct ofono_modem *modem,
					ofono_sim_ready_notify_cb_t cb);
void ofono_sim_ready_notify_unregister(struct ofono_modem *modem,
					ofono_sim_ready_notify_cb_t cb);

int ofono_sim_get_ready(struct ofono_modem *modem);
void ofono_sim_set_ready(struct ofono_modem *modem);

/* This will queue an operation to read all available records with id from the
 * SIM.  Callback cb will be called every time a record has been read, or once
 * if an error has occurred.  For transparent files, the callback will only
 * be called once.
 *
 * Returns 0 if the request could be queued, -1 otherwise.
 */
int ofono_sim_read(struct ofono_modem *modem, int id,
			ofono_sim_file_read_cb_t cb, void *data);

int ofono_sim_write(struct ofono_modem *modem, int id,
			ofono_sim_file_write_cb_t cb,
			enum ofono_sim_file_structure structure, int record,
			const unsigned char *data, int length, void *userdata);
